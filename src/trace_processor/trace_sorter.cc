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

#include "src/trace_processor/importers/fuchsia/fuchsia_record.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/trace_sorter_queue.h"

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
      // Calling this function without using the packet the same
      // as just calling the destructor for the element.
      EvictVariadic(event);
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
                                     &TimestampedDescriptor::Compare);
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
void TraceSorter::SortAndExtractEventsUntilPacket(uint64_t limit_offset) {
  constexpr int64_t kTsMax = std::numeric_limits<int64_t>::max();
  for (;;) {
    size_t min_queue_idx = 0;  // The index of the queue with the min(ts).

    // The top-2 min(ts) among all queues.
    // queues_[min_queue_idx].events.timestamp == min_queue_ts[0].
    int64_t min_queue_ts[2]{kTsMax, kTsMax};

    // This loop identifies the queue which starts with the earliest event and
    // also remembers the earliest event of the 2nd queue (in min_queue_ts[1]).
    bool has_queues_with_expired_events = false;
    for (size_t i = 0; i < queues_.size(); i++) {
      auto& queue = queues_[i];
      if (queue.events_.empty())
        continue;
      PERFETTO_DCHECK(queue.min_ts_ >= global_min_ts_);
      PERFETTO_DCHECK(queue.max_ts_ <= global_max_ts_);
      if (!has_queues_with_expired_events || queue.min_ts_ < min_queue_ts[0]) {
        min_queue_ts[1] = min_queue_ts[0];
        min_queue_ts[0] = queue.min_ts_;
        min_queue_idx = i;
        has_queues_with_expired_events = true;
      } else if (queue.min_ts_ < min_queue_ts[1]) {
        min_queue_ts[1] = queue.min_ts_;
      }
    }
    if (!has_queues_with_expired_events) {
      // All the queues have events that start after the window (i.e. they are
      // too recent and not eligible to be extracted given the current window).
      break;
    }

    Queue& queue = queues_[min_queue_idx];
    auto& events = queue.events_;
    if (queue.needs_sorting())
      queue.Sort();
    PERFETTO_DCHECK(queue.min_ts_ == events.front().ts);
    PERFETTO_DCHECK(queue.min_ts_ == global_min_ts_);

    // Now that we identified the min-queue, extract all events from it until
    // we hit either: (1) the min-ts of the 2nd queue or (2) the packet index
    // limit, whichever comes first.
    size_t num_extracted = 0;
    for (auto& event : events) {
      if (event.descriptor.offset() >= limit_offset ||
          event.ts > min_queue_ts[1]) {
        break;
      }

      ++num_extracted;
      MaybePushAndEvictEvent(min_queue_idx, event);
    }  // for (event: events)

    if (!num_extracted) {
      // No events can be extracted from any of the queues. This means that
      // either we hit the window or all queues are empty.
      break;
    }

    // Now remove the entries from the event buffer and update the queue-local
    // and global time bounds.
    events.erase_front(num_extracted);

    // After evicting elements we can empty memory in the front of the
    // queue.
    variadic_queue_.FreeMemory();

    // Update the global_{min,max}_ts to reflect the bounds after extraction.
    if (events.empty()) {
      queue.min_ts_ = kTsMax;
      queue.max_ts_ = 0;
      global_min_ts_ = min_queue_ts[1];

      // If we extraced the max entry from a queue (i.e. we emptied the queue)
      // we need to recompute the global max, because it might have been the one
      // just extracted.
      global_max_ts_ = 0;
      for (auto& q : queues_)
        global_max_ts_ = std::max(global_max_ts_, q.max_ts_);
    } else {
      queue.min_ts_ = queue.events_.front().ts;
      global_min_ts_ = std::min(queue.min_ts_, min_queue_ts[1]);
    }
  }  // for(;;)

#if PERFETTO_DCHECK_IS_ON()
  // Check that the global min/max are consistent.
  int64_t dbg_min_ts = kTsMax;
  int64_t dbg_max_ts = 0;
  for (auto& q : queues_) {
    dbg_min_ts = std::min(dbg_min_ts, q.min_ts_);
    dbg_max_ts = std::max(dbg_max_ts, q.max_ts_);
  }
  PERFETTO_DCHECK(global_min_ts_ == dbg_min_ts);
  PERFETTO_DCHECK(global_max_ts_ == dbg_max_ts);
#endif
}

void TraceSorter::EvictVariadic(const TimestampedDescriptor& ts_desc) {
  switch (ts_desc.descriptor.type()) {
    case EventType::kTracePacket:
      EvictTypedVariadic<TracePacketData>(ts_desc);
      return;
    case EventType::kTrackEvent:
      EvictTypedVariadic<TrackEventData>(ts_desc);
      return;
    case EventType::kFuchsiaRecord:
      EvictTypedVariadic<FuchsiaRecord>(ts_desc);
      return;
    case EventType::kJsonValue:
      EvictTypedVariadic<std::string>(ts_desc);
      return;
    case EventType::kSystraceLine:
      EvictTypedVariadic<SystraceLine>(ts_desc);
      return;
    case EventType::kInlineSchedSwitch:
      EvictTypedVariadic<InlineSchedSwitch>(ts_desc);
      return;
    case EventType::kInlineSchedWaking:
      EvictTypedVariadic<InlineSchedWaking>(ts_desc);
      return;
    case EventType::kFtraceEvent:
      EvictTypedVariadic<FtraceEventData>(ts_desc);
      return;
    case EventType::kInvalid:
      PERFETTO_FATAL("Invalid event type");
  }
  PERFETTO_FATAL("For GCC");
}

void TraceSorter::ParseTracePacket(const TimestampedDescriptor& ts_desc) {
  switch (ts_desc.descriptor.type()) {
    case EventType::kTracePacket:
      parser_->ParseTracePacket(ts_desc.ts,
                                EvictTypedVariadic<TracePacketData>(ts_desc));
      return;
    case EventType::kTrackEvent:
      parser_->ParseTrackEvent(ts_desc.ts,
                               EvictTypedVariadic<TrackEventData>(ts_desc));
      return;
    case EventType::kFuchsiaRecord:
      parser_->ParseFuchsiaRecord(ts_desc.ts,
                                  EvictTypedVariadic<FuchsiaRecord>(ts_desc));
      return;
    case EventType::kJsonValue:
      parser_->ParseJsonPacket(ts_desc.ts,
                               EvictTypedVariadic<std::string>(ts_desc));
      return;
    case EventType::kSystraceLine:
      parser_->ParseSystraceLine(ts_desc.ts,
                                 EvictTypedVariadic<SystraceLine>(ts_desc));
      return;
    case EventType::kInlineSchedSwitch:
    case EventType::kInlineSchedWaking:
    case EventType::kFtraceEvent:
    case EventType::kInvalid:
      PERFETTO_FATAL("Invalid event type");
  }
  PERFETTO_FATAL("For GCC");
}

void TraceSorter::ParseFtracePacket(uint32_t cpu,
                                    const TimestampedDescriptor& ts_desc) {
  switch (ts_desc.descriptor.type()) {
    case EventType::kInlineSchedSwitch:
      parser_->ParseInlineSchedSwitch(
          cpu, ts_desc.ts, EvictTypedVariadic<InlineSchedSwitch>(ts_desc));
      return;
    case EventType::kInlineSchedWaking:
      parser_->ParseInlineSchedWaking(
          cpu, ts_desc.ts, EvictTypedVariadic<InlineSchedWaking>(ts_desc));
      return;
    case EventType::kFtraceEvent:
      parser_->ParseFtraceEvent(cpu, ts_desc.ts,
                                EvictTypedVariadic<FtraceEventData>(ts_desc));
      return;
    case EventType::kTrackEvent:
    case EventType::kSystraceLine:
    case EventType::kTracePacket:
    case EventType::kJsonValue:
    case EventType::kFuchsiaRecord:
    case EventType::kInvalid:
      PERFETTO_FATAL("Invalid event type");
  }
  PERFETTO_FATAL("For GCC");
}

void TraceSorter::MaybePushAndEvictEvent(size_t queue_idx,
                                         const TimestampedDescriptor& ts_desc) {
  int64_t timestamp = ts_desc.ts;
  if (timestamp < latest_pushed_event_ts_)
    context_->storage->IncrementStats(stats::sorter_push_event_out_of_order);

  latest_pushed_event_ts_ = std::max(latest_pushed_event_ts_, timestamp);

  if (PERFETTO_UNLIKELY(bypass_next_stage_for_testing_)) {
    // In standard run the object would be evicted by Parsing{F}tracePacket.
    // Without it we need to evict it manually.
    EvictVariadic(ts_desc);
    return;
  }

  if (queue_idx == 0) {
    ParseTracePacket(ts_desc);
  } else {
    // Ftrace queues start at offset 1. So queues_[1] = cpu[0] and so on.
    uint32_t cpu = static_cast<uint32_t>(queue_idx - 1);
    ParseFtracePacket(cpu, ts_desc);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
