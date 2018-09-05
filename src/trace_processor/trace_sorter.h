/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TRACE_SORTER_H_
#define SRC_TRACE_PROCESSOR_TRACE_SORTER_H_

#include <vector>

#include "src/trace_processor/basic_types.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// This class takes care of sorting events parsed from the trace stream in
// arbitrary order and pushing them to the next pipeline stages (parsing) in
// order. In order to support streaming use-cases, sorting happens within a
// max window. Events are held in the TraceSorter staging area (events_) until
// either (1) the (max - min) timestamp > window_size; (2) trace EOF.
//
// Performance considerations:
// This class is designed assuming that events are mostly ordered and lack of
// ordering tends to happen towards the end of |events_|. In practice, in fact,
// lack of ordering comes from the fact that the ftrace buffers from differnt
// CPUs are independent and are flushed into the trace in blocks. So, when
// taking a trace file, events that are near (w.r.t. file offset) are likely to
// be out-of-order, but events that are ~10MB+ apart from each other are often
// in-order.
//
// Operation:
// When a bunch of events is pushed they are just appeneded to the end of the
// |events_| staging area. While appending, we keep track of the fact that the
// staging area is ordered or not. When an out-of-order event is detected we
// keep track of: (1) the offset within the staging area where the chaos begun,
// (2) the timestamp that broke the ordering.
// When we decide to flush events from the staging area into the next stages of
// the trace processor, we re-sort the events in the staging area. Rather than
// re-sorting everything all the times, we use the above knowledge to restrict
// sorting to the (hopefully smaller) tail of the |events_| staging area.
// At any time, the first partition of |events_| [0 .. sort_start_idx_) is
// ordered, and the second partition [sort_start_idx_.. end] is not.
// We use a logarithmic bound search operation to figure out what is the index
// within the first partition where sorting should start, and sort all events
// from there to the end.

class TraceSorter {
 public:
  struct TimestampedTracePiece {
    static constexpr uint32_t kNoCpu = std::numeric_limits<uint32_t>::max();

    TimestampedTracePiece(uint64_t a, TraceBlobView b, uint32_t c)
        : timestamp(a), blob_view(std::move(b)), cpu(c) {}

    TimestampedTracePiece(TimestampedTracePiece&&) noexcept = default;
    TimestampedTracePiece& operator=(TimestampedTracePiece&&) = default;

    // For std::lower_bound().
    static inline bool Compare(const TimestampedTracePiece& x, uint64_t ts) {
      return x.timestamp < ts;
    }

    // For std::sort().
    inline bool operator<(const TimestampedTracePiece& o) const {
      return timestamp < o.timestamp;
    }

    bool is_ftrace() const { return cpu != kNoCpu; }

    uint64_t timestamp;
    TraceBlobView blob_view;
    uint32_t cpu;
  };

  TraceSorter(TraceProcessorContext*,
              OptimizationMode,
              uint64_t window_size_ns);

  inline void PushTracePacket(uint64_t timestamp, TraceBlobView packet) {
    AppendAndMaybeFlushEvents(TimestampedTracePiece(
        timestamp, std::move(packet), TimestampedTracePiece::kNoCpu));
  }

  inline void PushFtracePacket(uint32_t cpu,
                               uint64_t timestamp,
                               TraceBlobView packet) {
    AppendAndMaybeFlushEvents(
        TimestampedTracePiece(timestamp, std::move(packet), cpu));
  }

  // This method passes any events older than window_size_ns to the
  // parser to be parsed and then stored.
  void SortAndFlushEventsBeyondWindow(uint64_t windows_size_ns);

  // Flush all events ignorinig the window.
  void FlushEventsForced() {
    SortAndFlushEventsBeyondWindow(/*window_size_ns=*/0);
  }

  void set_window_ns_for_testing(uint64_t window_size_ns) {
    window_size_ns_ = window_size_ns;
  }

 private:
  inline void AppendAndMaybeFlushEvents(TimestampedTracePiece ttp) {
    const uint64_t timestamp = ttp.timestamp;
    events_.emplace_back(std::move(ttp));
    earliest_timestamp_ = std::min(earliest_timestamp_, timestamp);

    // Events are often seen in order.
    if (PERFETTO_LIKELY(timestamp >= latest_timestamp_)) {
      latest_timestamp_ = timestamp;
    } else {
      // The event is breaking ordering. The first time it happens, keep
      // track of which index we are at. We know that everything before that
      // is sorted (because events were pushed monotonically). Everything after
      // that index, instead, will need a sorting pass before moving events to
      // the next pipeline stage.
      if (PERFETTO_UNLIKELY(sort_start_idx_ == 0)) {
        PERFETTO_DCHECK(events_.size() >= 2);
        sort_start_idx_ = events_.size() - 1;
        sort_min_ts_ = timestamp;
      } else {
        sort_min_ts_ = std::min(sort_min_ts_, timestamp);
      }
    }

    PERFETTO_DCHECK(earliest_timestamp_ <= latest_timestamp_);

    if (latest_timestamp_ - earliest_timestamp_ < window_size_ns_)
      return;

    // If we are optimizing for high-bandwidth, wait before we accumulate a
    // bunch of events before processing them. There are two cpu-intensive
    // things happening here: (1) Sorting the tail of |events_|; (2) Erasing the
    // head of |events_| and shifting them left. Both operations become way
    // faster if done in large batches (~1M events), where we end up erasing
    // 90% or more of |events_| and the erase-front becomes mainly a memmove of
    // the remaining tail elements. Capping at 1M objectis to avoid holding
    // too many events in the staging area.
    if (optimization_ == OptimizationMode::kMaxBandwidth &&
        latest_timestamp_ - earliest_timestamp_ < window_size_ns_ * 10 &&
        events_.size() < 1e6) {
      return;
    }

    SortAndFlushEventsBeyondWindow(window_size_ns_);
  }

  // std::deque makes erase-front potentially faster but std::sort slower.
  // Overall seems slower than a vector (350 MB/s vs 400 MB/s) without counting
  // next pipeline stages.
  std::vector<TimestampedTracePiece> events_;
  TraceProcessorContext* const context_;
  OptimizationMode optimization_;

  // Events are propagated to the next stage only after (max - min) timestamp
  // is larger than this value.
  uint64_t window_size_ns_;

  // max(e.timestamp for e in events_).
  uint64_t latest_timestamp_ = 0;

  // min(e.timestamp for e in events_).
  uint64_t earliest_timestamp_ = std::numeric_limits<uint64_t>::max();

  // Contains the index (< events_.size()) of the last sorted event. In essence,
  // events_[0..sort_start_idx_] are guaranteed to be in-order, while
  // events_[(sort_start_idx_ + 1)..end] are in random order.
  size_t sort_start_idx_ = 0;

  // The smallest timestamp that breaks the ordering in the range
  // events_[0..sort_start_idx_]. In order to re-establish a total order within
  // |events_| we need to sort entries from (the index corresponding to) that
  // timestamp.
  uint64_t sort_min_ts_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_SORTER_H_
