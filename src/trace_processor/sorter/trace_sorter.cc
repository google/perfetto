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
#include <memory>
#include <utility>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_record.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/bump_allocator.h"

namespace perfetto {
namespace trace_processor {

TraceSorter::TraceSorter(TraceProcessorContext* context,
                         std::unique_ptr<TraceParser> parser,
                         SortingMode sorting_mode)
    : context_(context),
      parser_(std::move(parser)),
      sorting_mode_(sorting_mode) {
  const char* env = getenv("TRACE_PROCESSOR_SORT_ONLY");
  bypass_next_stage_for_testing_ = env && !strcmp(env, "1");
  if (bypass_next_stage_for_testing_)
    PERFETTO_ELOG("TEST MODE: bypassing protobuf parsing stage");
}

TraceSorter::~TraceSorter() {
  // If trace processor encountered a fatal error, it's possible for some events
  // to have been pushed without evicting them by pushing to the next stage. Do
  // that now.
  for (auto& queue : queues_) {
    for (const auto& event : queue.events_) {
      ExtractAndDiscardTokenizedObject(event);
    }
  }
}

void TraceSorter::Queue::Sort() {
  PERFETTO_DCHECK(needs_sorting());
  PERFETTO_DCHECK(sort_start_idx_ < events_.size());

  // If sort_min_ts_ has been set, it will no long be max_int, and so will be
  // smaller than max_ts_.
  PERFETTO_DCHECK(sort_min_ts_ < max_ts_);

  // We know that all events between [0, sort_start_idx_] are sorted. Within
  // this range, perform a bound search and find the iterator for the min
  // timestamp that broke the monotonicity. Re-sort from there to the end.
  auto sort_end = events_.begin() + static_cast<ssize_t>(sort_start_idx_);
  PERFETTO_DCHECK(std::is_sorted(events_.begin(), sort_end));
  auto sort_begin = std::lower_bound(events_.begin(), sort_end, sort_min_ts_,
                                     &TimestampedEvent::Compare);
  std::sort(sort_begin, events_.end());
  sort_start_idx_ = 0;
  sort_min_ts_ = 0;

  // At this point |events_| must be fully sorted
  PERFETTO_DCHECK(std::is_sorted(events_.begin(), events_.end()));
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
// looking at any other queue. After hitting ts=10, we need to re-look to all of
// them to figure out the next min-event.
// There are more suitable data structures to do this (e.g. keeping a min-heap
// to avoid re-scanning all the queues all the times) but doesn't seem worth it.
// With Android traces (that have 8 CPUs) this function accounts for ~1-3% cpu
// time in a profiler.
void TraceSorter::SortAndExtractEventsUntilAllocId(
    BumpAllocator::AllocId limit_alloc_id) {
  constexpr int64_t kTsMax = std::numeric_limits<int64_t>::max();
  for (;;) {
    size_t min_queue_idx = 0;  // The index of the queue with the min(ts).

    // The top-2 min(ts) among all queues.
    // queues_[min_queue_idx].events.timestamp == min_queue_ts[0].
    int64_t min_queue_ts[2]{kTsMax, kTsMax};

    // This loop identifies the queue which starts with the earliest event and
    // also remembers the earliest event of the 2nd queue (in min_queue_ts[1]).
    bool all_queues_empty = true;
    for (size_t i = 0; i < queues_.size(); i++) {
      auto& queue = queues_[i];
      if (queue.events_.empty())
        continue;
      all_queues_empty = false;

      PERFETTO_DCHECK(queue.max_ts_ <= append_max_ts_);
      if (queue.min_ts_ < min_queue_ts[0]) {
        min_queue_ts[1] = min_queue_ts[0];
        min_queue_ts[0] = queue.min_ts_;
        min_queue_idx = i;
      } else if (queue.min_ts_ < min_queue_ts[1]) {
        min_queue_ts[1] = queue.min_ts_;
      }
    }
    if (all_queues_empty)
      break;

    Queue& queue = queues_[min_queue_idx];
    auto& events = queue.events_;
    if (queue.needs_sorting())
      queue.Sort();
    PERFETTO_DCHECK(queue.min_ts_ == events.front().ts);

    // Now that we identified the min-queue, extract all events from it until
    // we hit either: (1) the min-ts of the 2nd queue or (2) the packet index
    // limit, whichever comes first.
    size_t num_extracted = 0;
    for (auto& event : events) {
      if (event.alloc_id() >= limit_alloc_id) {
        break;
      }

      if (event.ts > min_queue_ts[1]) {
        // We should never hit this condition on the first extraction as by
        // the algorithm above (event.ts =) min_queue_ts[0] <= min_queue[1].
        PERFETTO_DCHECK(num_extracted > 0);
        break;
      }

      ++num_extracted;
      MaybeExtractEvent(min_queue_idx, event);
    }  // for (event: events)

    // The earliest event cannot be extracted without going past the limit.
    if (!num_extracted)
      break;

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
  }  // for(;;)
}

void TraceSorter::ParseTracePacket(const TimestampedEvent& event) {
  TraceTokenBuffer::Id id = GetTokenBufferId(event);
  switch (static_cast<TimestampedEvent::Type>(event.event_type)) {
    case TimestampedEvent::Type::kTraceBlobView:
      parser_->ParseTraceBlobView(event.ts,
                                  token_buffer_.Extract<TraceBlobView>(id));
      return;
    case TimestampedEvent::Type::kTracePacket:
      parser_->ParseTracePacket(event.ts,
                                token_buffer_.Extract<TracePacketData>(id));
      return;
    case TimestampedEvent::Type::kTrackEvent:
      parser_->ParseTrackEvent(event.ts,
                               token_buffer_.Extract<TrackEventData>(id));
      return;
    case TimestampedEvent::Type::kFuchsiaRecord:
      parser_->ParseFuchsiaRecord(event.ts,
                                  token_buffer_.Extract<FuchsiaRecord>(id));
      return;
    case TimestampedEvent::Type::kJsonValue:
      parser_->ParseJsonPacket(
          event.ts, std::move(token_buffer_.Extract<JsonEvent>(id).value));
      return;
    case TimestampedEvent::Type::kSystraceLine:
      parser_->ParseSystraceLine(event.ts,
                                 token_buffer_.Extract<SystraceLine>(id));
      return;
    case TimestampedEvent::Type::kInlineSchedSwitch:
    case TimestampedEvent::Type::kInlineSchedWaking:
    case TimestampedEvent::Type::kFtraceEvent:
      PERFETTO_FATAL("Invalid event type");
  }
  PERFETTO_FATAL("For GCC");
}

void TraceSorter::ParseFtracePacket(uint32_t cpu,
                                    const TimestampedEvent& event) {
  TraceTokenBuffer::Id id = GetTokenBufferId(event);
  switch (static_cast<TimestampedEvent::Type>(event.event_type)) {
    case TimestampedEvent::Type::kInlineSchedSwitch:
      parser_->ParseInlineSchedSwitch(
          cpu, event.ts, token_buffer_.Extract<InlineSchedSwitch>(id));
      return;
    case TimestampedEvent::Type::kInlineSchedWaking:
      parser_->ParseInlineSchedWaking(
          cpu, event.ts, token_buffer_.Extract<InlineSchedWaking>(id));
      return;
    case TimestampedEvent::Type::kFtraceEvent:
      parser_->ParseFtraceEvent(cpu, event.ts,
                                token_buffer_.Extract<TracePacketData>(id));
      return;
    case TimestampedEvent::Type::kTrackEvent:
    case TimestampedEvent::Type::kSystraceLine:
    case TimestampedEvent::Type::kTracePacket:
    case TimestampedEvent::Type::kTraceBlobView:
    case TimestampedEvent::Type::kJsonValue:
    case TimestampedEvent::Type::kFuchsiaRecord:
      PERFETTO_FATAL("Invalid event type");
  }
  PERFETTO_FATAL("For GCC");
}

void TraceSorter::ExtractAndDiscardTokenizedObject(
    const TimestampedEvent& event) {
  TraceTokenBuffer::Id id = GetTokenBufferId(event);
  switch (static_cast<TimestampedEvent::Type>(event.event_type)) {
    case TimestampedEvent::Type::kTraceBlobView:
      base::ignore_result(token_buffer_.Extract<TraceBlobView>(id));
      return;
    case TimestampedEvent::Type::kTracePacket:
      base::ignore_result(token_buffer_.Extract<TracePacketData>(id));
      return;
    case TimestampedEvent::Type::kTrackEvent:
      base::ignore_result(token_buffer_.Extract<TrackEventData>(id));
      return;
    case TimestampedEvent::Type::kFuchsiaRecord:
      base::ignore_result(token_buffer_.Extract<FuchsiaRecord>(id));
      return;
    case TimestampedEvent::Type::kJsonValue:
      base::ignore_result(token_buffer_.Extract<JsonEvent>(id));
      return;
    case TimestampedEvent::Type::kSystraceLine:
      base::ignore_result(token_buffer_.Extract<SystraceLine>(id));
      return;
    case TimestampedEvent::Type::kInlineSchedSwitch:
      base::ignore_result(token_buffer_.Extract<InlineSchedSwitch>(id));
      return;
    case TimestampedEvent::Type::kInlineSchedWaking:
      base::ignore_result(token_buffer_.Extract<InlineSchedWaking>(id));
      return;
    case TimestampedEvent::Type::kFtraceEvent:
      base::ignore_result(token_buffer_.Extract<TracePacketData>(id));
      return;
  }
  PERFETTO_FATAL("For GCC");
}

void TraceSorter::MaybeExtractEvent(size_t queue_idx,
                                    const TimestampedEvent& event) {
  int64_t timestamp = event.ts;
  if (timestamp < latest_pushed_event_ts_)
    context_->storage->IncrementStats(stats::sorter_push_event_out_of_order);

  latest_pushed_event_ts_ = std::max(latest_pushed_event_ts_, timestamp);

  if (PERFETTO_UNLIKELY(bypass_next_stage_for_testing_)) {
    // Parse* would extract this event and push it to the next stage. Since we
    // are skipping that, just extract and discard it.
    ExtractAndDiscardTokenizedObject(event);
    return;
  }

  if (queue_idx == 0) {
    ParseTracePacket(event);
  } else {
    // Ftrace queues start at offset 1. So queues_[1] = cpu[0] and so on.
    uint32_t cpu = static_cast<uint32_t>(queue_idx - 1);
    ParseFtracePacket(cpu, event);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
