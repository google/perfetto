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

#ifndef SRC_TRACE_PROCESSOR_TRACE_SORTER_H_
#define SRC_TRACE_PROCESSOR_TRACE_SORTER_H_

#include <vector>

#include "perfetto/base/circular_queue.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/fuchsia_provider_view.h"
#include "src/trace_processor/proto_incremental_state.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
#include <json/value.h>
#else
// Json traces are only supported in standalone build.
namespace Json {
class Value {};
}  // namespace Json
#endif

namespace perfetto {
namespace trace_processor {

// This class takes care of sorting events parsed from the trace stream in
// arbitrary order and pushing them to the next pipeline stages (parsing) in
// order. In order to support streaming use-cases, sorting happens within a
// max window. Events are held in the TraceSorter staging area (events_) until
// either (1) the (max - min) timestamp > window_size; (2) trace EOF.
//
// This class is designed around the assumption that:
// - Most events come from ftrace.
// - Ftrace events are sorted within each cpu most of the times.
//
// Due to this, this class is oprerates as a streaming merge-sort of N+1 queues
// (N = num cpus + 1 for non-ftrace events). Each queue in turn gets sorted (if
// necessary) before proceeding with the global merge-sort-extract.
// When an event is pushed through, it is just appeneded to the end of one of
// the N queues. While appending, we keep track of the fact that the queue
// is still ordered or just lost ordering. When an out-of-order event is
// detected on a queue we keep track of: (1) the offset within the queue where
// the chaos begun, (2) the timestamp that broke the ordering.
// When we decide to extract events from the queues into the next stages of
// the trace processor, we re-sort the events in the queue. Rather than
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
    TimestampedTracePiece(int64_t ts, uint64_t idx, TraceBlobView tbv)
        : TimestampedTracePiece(ts,
                                /*thread_ts=*/0,
                                idx,
                                std::move(tbv),
                                /*value=*/nullptr,
                                /*fpv=*/nullptr,
                                /*sequence_state=*/nullptr) {}

    TimestampedTracePiece(int64_t ts,
                          uint64_t idx,
                          std::unique_ptr<Json::Value> value)
        : TimestampedTracePiece(ts,
                                /*thread_ts=*/0,
                                idx,
                                // TODO(dproy): Stop requiring TraceBlobView in
                                // TimestampedTracePiece.
                                TraceBlobView(nullptr, 0, 0),
                                std::move(value),
                                /*fpv=*/nullptr,
                                /*sequence_state=*/nullptr) {}

    TimestampedTracePiece(int64_t ts,
                          uint64_t idx,
                          TraceBlobView tbv,
                          std::unique_ptr<FuchsiaProviderView> fpv)
        : TimestampedTracePiece(ts,
                                /*thread_ts=*/0,
                                idx,
                                std::move(tbv),
                                /*value=*/nullptr,
                                std::move(fpv),
                                /*sequence_state=*/nullptr) {}

    TimestampedTracePiece(
        int64_t ts,
        int64_t thread_ts,
        uint64_t idx,
        TraceBlobView tbv,
        ProtoIncrementalState::PacketSequenceState* sequence_state)
        : TimestampedTracePiece(ts,
                                thread_ts,
                                idx,
                                std::move(tbv),
                                /*value=*/nullptr,
                                /*fpv=*/nullptr,
                                sequence_state) {}

    TimestampedTracePiece(
        int64_t ts,
        int64_t thread_ts,
        uint64_t idx,
        TraceBlobView tbv,
        std::unique_ptr<Json::Value> value,
        std::unique_ptr<FuchsiaProviderView> fpv,
        ProtoIncrementalState::PacketSequenceState* sequence_state)
        : json_value(std::move(value)),
          fuchsia_provider_view(std::move(fpv)),
          packet_sequence_state(sequence_state),
          timestamp(ts),
          thread_timestamp(thread_ts),
          packet_idx_(idx),
          blob_view(std::move(tbv)) {}

    TimestampedTracePiece(TimestampedTracePiece&&) noexcept = default;
    TimestampedTracePiece& operator=(TimestampedTracePiece&&) = default;

    // For std::lower_bound().
    static inline bool Compare(const TimestampedTracePiece& x, int64_t ts) {
      return x.timestamp < ts;
    }

    // For std::sort().
    inline bool operator<(const TimestampedTracePiece& o) const {
      return timestamp < o.timestamp ||
             (timestamp == o.timestamp && packet_idx_ < o.packet_idx_);
    }

    std::unique_ptr<Json::Value> json_value;
    std::unique_ptr<FuchsiaProviderView> fuchsia_provider_view;
    ProtoIncrementalState::PacketSequenceState* packet_sequence_state;

    int64_t timestamp;
    int64_t thread_timestamp;
    uint64_t packet_idx_;
    TraceBlobView blob_view;
  };

  TraceSorter(TraceProcessorContext*, int64_t window_size_ns);

  inline void PushTracePacket(int64_t timestamp, TraceBlobView packet) {
    DCHECK_ftrace_batch_cpu(kNoBatch);
    auto* queue = GetQueue(0);
    queue->Append(
        TimestampedTracePiece(timestamp, packet_idx_++, std::move(packet)));
    MaybeExtractEvents(queue);
  }

  inline void PushJsonValue(int64_t timestamp,
                            std::unique_ptr<Json::Value> json_value) {
    auto* queue = GetQueue(0);
    queue->Append(
        TimestampedTracePiece(timestamp, packet_idx_++, std::move(json_value)));
    MaybeExtractEvents(queue);
  }

  inline void PushFuchsiaRecord(
      int64_t timestamp,
      TraceBlobView record,
      std::unique_ptr<FuchsiaProviderView> provider_view) {
    DCHECK_ftrace_batch_cpu(kNoBatch);
    auto* queue = GetQueue(0);
    queue->Append(TimestampedTracePiece(
        timestamp, packet_idx_++, std::move(record), std::move(provider_view)));
    MaybeExtractEvents(queue);
  }

  inline void PushFtraceEvent(uint32_t cpu,
                              int64_t timestamp,
                              TraceBlobView event) {
    set_ftrace_batch_cpu_for_DCHECK(cpu);
    GetQueue(cpu + 1)->Append(
        TimestampedTracePiece(timestamp, packet_idx_++, std::move(event)));

    // The caller must call FinalizeFtraceEventBatch() after having pushed a
    // batch of ftrace events. This is to amortize the overhead of handling
    // global ordering and doing that in batches only after all ftrace events
    // for a bundle are pushed.
  }

  inline void PushTrackEventPacket(
      int64_t timestamp,
      int64_t thread_time,
      ProtoIncrementalState::PacketSequenceState* state,
      TraceBlobView packet) {
    auto* queue = GetQueue(0);
    queue->Append(TimestampedTracePiece(timestamp, thread_time, packet_idx_++,
                                        std::move(packet), state));
    MaybeExtractEvents(queue);
  }

  inline void FinalizeFtraceEventBatch(uint32_t cpu) {
    DCHECK_ftrace_batch_cpu(cpu);
    set_ftrace_batch_cpu_for_DCHECK(kNoBatch);
    MaybeExtractEvents(GetQueue(cpu + 1));
  }

  // Extract all events ignoring the window.
  void ExtractEventsForced() {
    SortAndExtractEventsBeyondWindow(/*window_size_ns=*/0);
  }

  void set_window_ns_for_testing(int64_t window_size_ns) {
    window_size_ns_ = window_size_ns;
  }

 private:
  static constexpr uint32_t kNoBatch = std::numeric_limits<uint32_t>::max();

  struct Queue {
    inline void Append(TimestampedTracePiece ttp) {
      const int64_t timestamp = ttp.timestamp;
      events_.emplace_back(std::move(ttp));
      min_ts_ = std::min(min_ts_, timestamp);

      // Events are often seen in order.
      if (PERFETTO_LIKELY(timestamp >= max_ts_)) {
        max_ts_ = timestamp;
      } else {
        // The event is breaking ordering. The first time it happens, keep
        // track of which index we are at. We know that everything before that
        // is sorted (because events were pushed monotonically). Everything
        // after that index, instead, will need a sorting pass before moving
        // events to the next pipeline stage.
        if (sort_start_idx_ == 0) {
          PERFETTO_DCHECK(events_.size() >= 2);
          sort_start_idx_ = events_.size() - 1;
          sort_min_ts_ = timestamp;
        } else {
          sort_min_ts_ = std::min(sort_min_ts_, timestamp);
        }
      }

      PERFETTO_DCHECK(min_ts_ <= max_ts_);
    }

    bool needs_sorting() const { return sort_start_idx_ != 0; }
    void Sort();

    base::CircularQueue<TimestampedTracePiece> events_;
    int64_t min_ts_ = std::numeric_limits<int64_t>::max();
    int64_t max_ts_ = 0;
    size_t sort_start_idx_ = 0;
    int64_t sort_min_ts_ = std::numeric_limits<int64_t>::max();
  };

  // This method passes any events older than window_size_ns to the
  // parser to be parsed and then stored.
  void SortAndExtractEventsBeyondWindow(int64_t windows_size_ns);

  inline Queue* GetQueue(size_t index) {
    if (PERFETTO_UNLIKELY(index >= queues_.size()))
      queues_.resize(index + 1);
    return &queues_[index];
  }

  inline void MaybeExtractEvents(Queue* queue) {
    DCHECK_ftrace_batch_cpu(kNoBatch);
    global_max_ts_ = std::max(global_max_ts_, queue->max_ts_);
    global_min_ts_ = std::min(global_min_ts_, queue->min_ts_);

    if (global_max_ts_ - global_min_ts_ < window_size_ns_)
      return;

    SortAndExtractEventsBeyondWindow(window_size_ns_);
  }

  TraceProcessorContext* const context_;

  // queues_[0] is the general (non-ftrace) queue.
  // queues_[1] is the ftrace queue for CPU(0).
  // queues_[x] is the ftrace queue for CPU(x - 1).
  std::vector<Queue> queues_;

  // Events are propagated to the next stage only after (max - min) timestamp
  // is larger than this value.
  int64_t window_size_ns_;

  // max(e.timestamp for e in queues_).
  int64_t global_max_ts_ = 0;

  // min(e.timestamp for e in queues_).
  int64_t global_min_ts_ = std::numeric_limits<int64_t>::max();

  // Monotonic increasing value used to index timestamped trace pieces.
  uint64_t packet_idx_ = 0;

  // Used for performance tests. True when setting TRACE_PROCESSOR_SORT_ONLY=1.
  bool bypass_next_stage_for_testing_ = false;

#if PERFETTO_DCHECK_IS_ON()
  // Used only for DCHECK-ing that FinalizeFtraceEventBatch() is called.
  uint32_t ftrace_batch_cpu_ = kNoBatch;

  inline void DCHECK_ftrace_batch_cpu(uint32_t cpu) {
    PERFETTO_DCHECK(ftrace_batch_cpu_ == kNoBatch || ftrace_batch_cpu_ == cpu);
  }

  inline void set_ftrace_batch_cpu_for_DCHECK(uint32_t cpu) {
    PERFETTO_DCHECK(ftrace_batch_cpu_ == cpu || ftrace_batch_cpu_ == kNoBatch ||
                    cpu == kNoBatch);
    ftrace_batch_cpu_ = cpu;
  }
#else
  inline void DCHECK_ftrace_batch_cpu(uint32_t) {}
  inline void set_ftrace_batch_cpu_for_DCHECK(uint32_t) {}
#endif
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_SORTER_H_
