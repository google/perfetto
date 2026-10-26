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
#include <functional>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>
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
  for (const Block& block : blocks_) {
    for (size_t i = block.head; i < block.events.size(); ++i) {
      const TimestampedEvent& event = block.events[i];
      sinks_[event.stream]->OnDiscardedEvent(this, GetTokenBufferId(event));
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

void TraceSorter::OpenBlock() {
  // Start small, so few pending events take little memory, unless events
  // just filled a block.
  uint32_t capacity = block_open_ ? kBlockEvents : 4 * 1024;
  if (block_open_) {
    SortBlock(blocks_.back());
  }
  blocks_.emplace_back();
  blocks_.back().events.reserve(capacity);
  block_open_ = true;
}

void TraceSorter::SortBlock(Block& block) {
  if (block.sorted) {
    return;
  }
  block.sorted = true;
  auto begin = block.events.begin() + static_cast<ptrdiff_t>(block.head);
  auto end = block.events.end();
  if (use_slow_sorting_) {
    std::sort(begin, end, TimestampedEvent::SlowOperatorLess{token_buffer_});
    return;
  }
  // Radix sort on the offsets from the earliest timestamp, on only the bits
  // they use. The sort is stable, so equal timestamps keep arrival order.
  auto [min_it, max_it] = std::minmax_element(
      begin, end, [](const TimestampedEvent& a, const TimestampedEvent& b) {
        return a.ts < b.ts;
      });
  int64_t min_ts = min_it->ts;
  uint64_t range =
      static_cast<uint64_t>(max_it->ts) - static_cast<uint64_t>(min_ts);
  uint32_t key_bits = 64 - base::CountLeadZeros64(range);
  auto n = static_cast<size_t>(end - begin);
  radix_scratch_.resize(n);
  radix_counts_.resize(core::RadixSortCountsSize(key_bits, n));
  TimestampedEvent* sorted = core::RadixSort(
      &*begin, &*begin + n, radix_scratch_.data(), radix_counts_.data(),
      key_bits, [min_ts](const TimestampedEvent& e) {
        return static_cast<uint64_t>(e.ts) - static_cast<uint64_t>(min_ts);
      });
  if (sorted != &*begin) {
    memcpy(&*begin, sorted, n * sizeof(TimestampedEvent));
  }
}

// Extracts, in |less| order, the events of all blocks until the first one at
// or past |limit_alloc_id|. This is an "extract min from N sorted blocks",
// with some little cleverness: events tend to arrive in time order, so blocks
// cover mostly disjoint ranges. Upon each iteration this function finds the
// two blocks with the oldest events, and extracts events from the 1st until
// hitting the earliest event of the 2nd. Imagine the blocks are as follows:
//
//  b0           {min_ts: 10  max_ts: 30}
//  b1    {min_ts:5              max_ts: 35}
//  b2              {min_ts: 12    max_ts: 40}
//
// We know that we can extract all events from b1 until we hit ts=10 without
// looking at any other block. After hitting ts=10, the next min-block comes
// from a heap: its top is that block and one of its children the runner-up.
template <typename Less>
void TraceSorter::MergeBlocks(BumpAllocator::AllocId limit_alloc_id,
                              Less less) {
  auto head = [this](uint32_t b) -> const TimestampedEvent& {
    return blocks_[b].events[blocks_[b].head];
  };
  auto later = [&](uint32_t a, uint32_t b) { return less(head(b), head(a)); };
  std::vector<uint32_t>& heap = block_heap_;
  heap.clear();
  for (uint32_t i = 0; i < blocks_.size(); ++i) {
    heap.push_back(i);
  }
  std::make_heap(heap.begin(), heap.end(), later);
  while (!heap.empty()) {
    uint32_t b = heap[0];
    const TimestampedEvent* next = nullptr;
    for (size_t child = 1; child <= 2 && child < heap.size(); ++child) {
      if (!next || less(head(heap[child]), *next)) {
        next = &head(heap[child]);
      }
    }
    Block& block = blocks_[b];
    size_t i = block.head;
    for (; i < block.events.size(); ++i) {
      const TimestampedEvent& event = block.events[i];
      if (event.alloc_id() >= limit_alloc_id) {
        break;
      }
      // Stop at the earliest event of another block, but always extract the
      // first.
      if (i != block.head && next && less(*next, event)) {
        break;
      }
      UntypedSink* sink = sinks_[event.stream].get();
      if (event.ts < latest_pushed_event_ts_) {
        global_stats_tracker_->IncrementGlobalStats(
            stats::sorter_push_event_out_of_order);
        sink->OnDiscardedEvent(this, GetTokenBufferId(event));
        continue;
      }
      latest_pushed_event_ts_ = event.ts;

      if (PERFETTO_UNLIKELY(event_handling_ == EventHandling::kSortAndDrop)) {
        // Parse* would extract this event and push it to the next stage. Since
        // we are skipping that, just extract and discard it.
        sink->OnDiscardedEvent(this, GetTokenBufferId(event));
        continue;
      }
      PERFETTO_DCHECK(event_handling_ == EventHandling::kSortAndPush);

      sink->OnSortedEvent(this, event.ts, GetTokenBufferId(event));
    }
    // The earliest event cannot be extracted without going past the limit.
    if (i == block.head) {
      break;
    }
    block.head = i;
    if (block.head == block.events.size()) {
      // Free it now, not after the merge: one extraction can be the whole
      // trace.
      block.events = std::vector<TimestampedEvent>();
      uint32_t last = heap.back();
      heap.pop_back();
      if (!heap.empty()) {
        core::HeapSiftDown(heap.data(), heap.size(), last, later);
      }
    } else {
      // Shrink it as it drains.
      if (block.events.size() - i <= block.events.capacity() / 2) {
        block.events.erase(block.events.begin(),
                           block.events.begin() + static_cast<ptrdiff_t>(i));
        block.events.shrink_to_fit();
        block.head = 0;
      }
      core::HeapSiftDown(heap.data(), heap.size(), b, later);
    }
    // Since we likely just removed a bunch of events try to reduce the memory
    // usage of the token buffer.
    token_buffer_.FreeMemory();
  }
}

void TraceSorter::SortAndExtractEventsUntilAllocId(
    BumpAllocator::AllocId limit_alloc_id) {
  // Events pushed from here on go to a new block: the merge takes every block
  // as sorted.
  block_open_ = false;
  for (Block& block : blocks_) {
    SortBlock(block);
  }
  // Free them rather than hold them while the parsers' tables grow: blocks
  // are sorted again only as they fill.
  radix_scratch_ = std::vector<TimestampedEvent>();
  radix_counts_ = std::vector<uint32_t>();
  extracting_ = true;
  if (use_slow_sorting_) {
    MergeBlocks(limit_alloc_id,
                TimestampedEvent::SlowOperatorLess{token_buffer_});
  } else {
    MergeBlocks(limit_alloc_id, std::less<>{});
  }
  extracting_ = false;
  blocks_.erase(
      std::remove_if(blocks_.begin(), blocks_.end(),
                     [](const Block& block) { return block.events.empty(); }),
      blocks_.end());
}

TraceSorter::UntypedSink::~UntypedSink() = default;

}  // namespace perfetto::trace_processor
