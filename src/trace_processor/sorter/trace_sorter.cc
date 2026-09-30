/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/bits.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/core/util/heap.h"
#include "src/trace_processor/core/util/sort.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/sorter/trace_token_buffer.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/bump_allocator.h"

namespace perfetto::trace_processor {

TraceSorter::TraceSorter(TraceProcessorContext* context,
                         SortingMode sorting_mode,
                         EventHandling event_handling)
    : sorting_mode_(sorting_mode),
      storage_(context->storage.get()),
      global_stats_tracker_(context->global_stats_tracker.get()),
      event_handling_(event_handling) {}

TraceSorter::~TraceSorter() {
  // If trace processor encountered a fatal error, it's possible for some events
  // to have been pushed without evicting them by pushing to the next stage. Do
  // that now.
  for (auto& queue : queues_) {
    for (const auto& event : queue.events_) {
      queue.sink->OnDiscardedEvent(this, GetTokenBufferId(event));
    }
  }
}

bool TraceSorter::SetSortingMode(SortingMode sorting_mode) {
  // Early out if the new sorting mode matches the old.
  if (sorting_mode == sorting_mode_) {
    return true;
  }
  // We cannot transition back to a more relaxed mode after having left that
  // mode.
  if (sorting_mode_ != SortingMode::kDefault) {
    return false;
  }
  // We cannot change sorting mode after having extracted one or more events.
  if (latest_pushed_event_ts_ != std::numeric_limits<int64_t>::min()) {
    return false;
  }
  sorting_mode_ = sorting_mode;
  return true;
}

void TraceSorter::Queue::Sort(TraceTokenBuffer& buffer, bool use_slow_sorting) {
  PERFETTO_DCHECK(needs_sorting());
  PERFETTO_DCHECK(sort_start_idx_ < events_.size());

  // If sort_min_ts_ has been set, it will no long be max_int, and so will be
  // smaller than max_ts_.
  PERFETTO_DCHECK(sort_min_ts_ < std::numeric_limits<int64_t>::max());

  // We know that all events between [0, sort_start_idx_] are sorted. Within
  // this range, perform a bound search and find the iterator for the min
  // timestamp that broke the monotonicity. Re-sort from there to the end.
  auto sort_end = events_.begin() + static_cast<ssize_t>(sort_start_idx_);
  if (use_slow_sorting) {
    PERFETTO_DCHECK(sort_min_ts_ <= max_ts_);
    PERFETTO_DCHECK(std::is_sorted(events_.begin(), sort_end,
                                   TimestampedEvent::SlowOperatorLess{buffer}));
  } else {
    PERFETTO_DCHECK(sort_min_ts_ < max_ts_);
    PERFETTO_DCHECK(std::is_sorted(events_.begin(), sort_end));
  }
  auto sort_begin = std::lower_bound(events_.begin(), sort_end, sort_min_ts_,
                                     &TimestampedEvent::Compare);
  // Allocation ids grow in the order events are pushed, and those before
  // `sort_end` are already in order, so sorting stably on the timestamp alone
  // also orders ties by allocation id.
  auto first = static_cast<size_t>(sort_begin - events_.begin());
  std::vector<TimestampedEvent> events(events_.size() - first);
  std::vector<TimestampedEvent> scratch(events.size());
  // Calls `fn` on the parts of the queue holding them, which may wrap around.
  auto for_each_part = [this, first, &events](auto fn) {
    for (size_t at = 0; at < events.size();) {
      size_t count;
      TimestampedEvent* part = events_.contiguous_at(first + at, &count);
      fn(part, at, count * sizeof(TimestampedEvent));
      at += count;
    }
  };
  for_each_part([&events](TimestampedEvent* part, size_t at, size_t bytes) {
    memcpy(events.data() + at, part, bytes);
  });
  int64_t min_ts = sort_min_ts_;
  auto key_bits = static_cast<uint32_t>(
      64 - base::CountLeadZeros64(static_cast<uint64_t>(max_ts_ - min_ts)));
  TimestampedEvent* sorted = core::StableSortByKey(
      events.data(), events.data() + events.size(), scratch.data(), key_bits,
      [min_ts](const TimestampedEvent& e) {
        return static_cast<uint64_t>(e.ts - min_ts);
      },
      [](const TimestampedEvent& e) { return e.alloc_id(); });
  if (use_slow_sorting) {
    // Slow sorting also orders events with the same timestamp by their type.
    TimestampedEvent::SlowOperatorLess less{buffer};
    TimestampedEvent* end = sorted + events.size();
    for (TimestampedEvent* it = sorted; it != end;) {
      int64_t ts = it->ts;
      TimestampedEvent* run_end = std::find_if(
          it, end, [ts](const TimestampedEvent& e) { return e.ts != ts; });
      if (run_end - it > 1) {
        std::sort(it, run_end, less);
      }
      it = run_end;
    }
  }
  for_each_part([sorted](TimestampedEvent* part, size_t at, size_t bytes) {
    memcpy(part, sorted + at, bytes);
  });
  sort_start_idx_ = 0;
  sort_min_ts_ = 0;

  // At this point |events_| must be fully sorted
  if (use_slow_sorting) {
    PERFETTO_DCHECK(std::is_sorted(events_.begin(), events_.end(),
                                   TimestampedEvent::SlowOperatorLess{buffer}));
  } else {
    PERFETTO_DCHECK(std::is_sorted(events_.begin(), events_.end()));
  }
}

// Removes all the events in |queues_| that are earlier than the given
// packet index and moves them to the next parser stages, respecting global
// timestamp order. This function is a "extract min from N sorted queues", with
// some little cleverness: we know that events tend to be bursty, so events are
// not going to be randomly distributed on the N |queues_|.
// Upon each iteration this function finds the first two queues (if any) that
// have the oldest events, and extracts events from the 1st until hitting the
// min_ts of the 2nd. Imagine the queues are as follows:
//
//  q0           {min_ts: 10  max_ts: 30}
//  q1    {min_ts:5              max_ts: 35}
//  q2              {min_ts: 12    max_ts: 40}
//
// We know that we can extract all events from q1 until we hit ts=10 without
// looking at any other queue. After hitting ts=10, the next min-queue comes
// from a heap: queues interleave finely, so this repeats every event or two,
// and re-scanning all of them each time dominated extraction.
void TraceSorter::SortAndExtractEventsUntilAllocId(
    BumpAllocator::AllocId limit_alloc_id) {
  constexpr int64_t kTsMax = std::numeric_limits<int64_t>::max();
  // A min-heap of the non-empty queues by their earliest event, ties broken
  // by queue index, so equal timestamps come out in queue order.
  auto later = [](const QueueHeapEntry& a, const QueueHeapEntry& b) {
    return a.min_ts != b.min_ts ? a.min_ts > b.min_ts : a.queue > b.queue;
  };
  std::vector<QueueHeapEntry>& heap = queue_heap_;
  auto rebuild_heap = [&] {
    heap.clear();
    for (uint32_t i = 0; i < queues_.size(); ++i) {
      if (!queues_[i].events_.empty()) {
        PERFETTO_DCHECK(queues_[i].max_ts_ <= append_max_ts_);
        heap.push_back({queues_[i].min_ts_, i});
      }
    }
    std::make_heap(heap.begin(), heap.end(), later);
  };
  rebuild_heap();
  while (!heap.empty()) {
    uint32_t min_queue_idx = heap[0].queue;
    // The earliest event of any other queue.
    int64_t next_queue_ts = kTsMax;
    for (size_t child = 1; child <= 2 && child < heap.size(); ++child) {
      next_queue_ts = std::min(next_queue_ts, heap[child].min_ts);
    }

    auto& queue = queues_[min_queue_idx];
    auto& events = queue.events_;
    if (queue.needs_sorting()) {
      queue.Sort(token_buffer_, use_slow_sorting_);
    }
    PERFETTO_DCHECK(queue.min_ts_ == events.front().ts);

    // Extract events from the min-queue until hitting either: (1) the
    // earliest event of another queue or (2) the alloc id limit, whichever
    // comes first.
    uint64_t pushes = push_count_;
    size_t num_extracted = 0;
    for (auto& event : events) {
      if (event.alloc_id() >= limit_alloc_id) {
        break;
      }

      if (event.ts > next_queue_ts) {
        // We should never hit this condition on the first extraction as the
        // min-queue's first event is no later than any other queue's.
        PERFETTO_DCHECK(num_extracted > 0);
        break;
      }
      ++num_extracted;

      if (event.ts < latest_pushed_event_ts_) {
        global_stats_tracker_->IncrementGlobalStats(
            stats::sorter_push_event_out_of_order);
        queue.sink->OnDiscardedEvent(this, GetTokenBufferId(event));
        continue;
      }
      latest_pushed_event_ts_ = event.ts;

      if (PERFETTO_UNLIKELY(event_handling_ == EventHandling::kSortAndDrop)) {
        // Parse* would extract this event and push it to the next stage. Since
        // we are skipping that, just extract and discard it.
        queue.sink->OnDiscardedEvent(this, GetTokenBufferId(event));
        continue;
      }
      PERFETTO_DCHECK(event_handling_ == EventHandling::kSortAndPush);

      queue.sink->OnSortedEvent(this, event.ts, GetTokenBufferId(event));
    }  // for (event: events)

    // The earliest event cannot be extracted without going past the limit.
    if (!num_extracted) {
      break;
    }

    // Now remove the entries from the event buffer and update the queue-local
    // and global time bounds.
    events.erase_front(num_extracted);
    events.shrink_to_fit();

    // Since we likely just removed a bunch of items try to reduce the memory
    // usage of the token buffer.
    token_buffer_.FreeMemory();

    // Update the queue timestamps to reflect the bounds after extraction.
    if (events.empty()) {
      queue.min_ts_ = kTsMax;
      queue.max_ts_ = 0;
    } else {
      queue.min_ts_ = queue.events_.front().ts;
    }

    // Parsing pushed events, which may have moved the earliest event of any
    // queue: start over.
    if (PERFETTO_UNLIKELY(push_count_ != pushes)) {
      rebuild_heap();
      continue;
    }
    // The queue's earliest event only moved later, so it moves down from the
    // top; an empty queue is replaced there by the last.
    if (events.empty()) {
      QueueHeapEntry last = heap.back();
      heap.pop_back();
      if (!heap.empty()) {
        core::HeapSiftDown(heap.data(), heap.size(), last, later);
      }
    } else {
      core::HeapSiftDown(heap.data(), heap.size(),
                         QueueHeapEntry{queue.min_ts_, min_queue_idx}, later);
    }
  }
}

TraceSorter::UntypedSink::~UntypedSink() = default;

}  // namespace perfetto::trace_processor
