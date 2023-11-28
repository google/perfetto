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

#ifndef SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_H_
#define SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_H_

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_record.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"
#include "src/trace_processor/sorter/trace_token_buffer.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/bump_allocator.h"

namespace perfetto {
namespace trace_processor {

// This class takes care of sorting events parsed from the trace stream in
// arbitrary order and pushing them to the next pipeline stages (parsing) in
// order. In order to support streaming use-cases, sorting happens within a
// window.
//
// Events are held in the TraceSorter staging area (events_) until either:
// 1. We can determine that it's safe to extract events by observing
//  TracingServiceEvent Flush and ReadBuffer events
// 2. The trace EOF is reached
//
// Incremental extraction
//
// Incremental extraction happens by using a combination of flush and read
// buffer events from the tracing service. Note that incremental extraction
// is only applicable for write_into_file traces; ring-buffer traces will
// be sorted fully in-memory implicitly because there is only a single read
// buffer call at the end.
//
// The algorithm for incremental extraction is explained in detail at
// go/trace-sorting-is-complicated.
//
// Sorting algorithm
//
// The sorting algorithm is designed around the assumption that:
// - Most events come from ftrace.
// - Ftrace events are sorted within each cpu most of the times.
//
// Due to this, this class is oprerates as a streaming merge-sort of N+1 queues
// (N = num cpus + 1 for non-ftrace events). Each queue in turn gets sorted (if
// necessary) before proceeding with the global merge-sort-extract.
//
// When an event is pushed through, it is just appended to the end of one of
// the N queues. While appending, we keep track of the fact that the queue
// is still ordered or just lost ordering. When an out-of-order event is
// detected on a queue we keep track of: (1) the offset within the queue where
// the chaos begun, (2) the timestamp that broke the ordering.
//
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
  enum class SortingMode {
    kDefault,
    kFullSort,
  };

  TraceSorter(TraceProcessorContext* context,
              std::unique_ptr<TraceParser> parser,
              SortingMode);
  ~TraceSorter();

  inline void PushTraceBlobView(int64_t timestamp, TraceBlobView tbv) {
    TraceTokenBuffer::Id id = token_buffer_.Append(std::move(tbv));
    AppendNonFtraceEvent(timestamp, TimestampedEvent::Type::kTraceBlobView, id);
  }

  inline void PushTracePacket(int64_t timestamp, TracePacketData data) {
    TraceTokenBuffer::Id id = token_buffer_.Append(std::move(data));
    AppendNonFtraceEvent(timestamp, TimestampedEvent::Type::kTracePacket, id);
  }

  inline void PushTracePacket(int64_t timestamp,
                              RefPtr<PacketSequenceStateGeneration> state,
                              TraceBlobView tbv) {
    PushTracePacket(timestamp,
                    TracePacketData{std::move(tbv), std::move(state)});
  }

  inline void PushJsonValue(int64_t timestamp, std::string json_value) {
    TraceTokenBuffer::Id id =
        token_buffer_.Append(JsonEvent{std::move(json_value)});
    AppendNonFtraceEvent(timestamp, TimestampedEvent::Type::kJsonValue, id);
  }

  inline void PushFuchsiaRecord(int64_t timestamp,
                                FuchsiaRecord fuchsia_record) {
    TraceTokenBuffer::Id id = token_buffer_.Append(std::move(fuchsia_record));
    AppendNonFtraceEvent(timestamp, TimestampedEvent::Type::kFuchsiaRecord, id);
  }

  inline void PushSystraceLine(SystraceLine systrace_line) {
    TraceTokenBuffer::Id id = token_buffer_.Append(std::move(systrace_line));
    AppendNonFtraceEvent(systrace_line.ts,
                         TimestampedEvent::Type::kSystraceLine, id);
  }

  inline void PushTrackEventPacket(int64_t timestamp,
                                   TrackEventData track_event) {
    TraceTokenBuffer::Id id = token_buffer_.Append(std::move(track_event));
    AppendNonFtraceEvent(timestamp, TimestampedEvent::Type::kTrackEvent, id);
  }

  inline void PushFtraceEvent(uint32_t cpu,
                              int64_t timestamp,
                              TraceBlobView tbv,
                              RefPtr<PacketSequenceStateGeneration> state) {
    TraceTokenBuffer::Id id =
        token_buffer_.Append(TracePacketData{std::move(tbv), std::move(state)});
    auto* queue = GetQueue(cpu + 1);
    queue->Append(timestamp, TimestampedEvent::Type::kFtraceEvent, id);
    UpdateAppendMaxTs(queue);
  }

  inline void PushInlineFtraceEvent(uint32_t cpu,
                                    int64_t timestamp,
                                    InlineSchedSwitch inline_sched_switch) {
    // TODO(rsavitski): if a trace has a mix of normal & "compact" events
    // (being pushed through this function), the ftrace batches will no longer
    // be fully sorted by timestamp. In such situations, we will have to sort
    // at the end of the batch. We can do better as both sub-sequences are
    // sorted however. Consider adding extra queues, or pushing them in a
    // merge-sort fashion
    // // instead.
    TraceTokenBuffer::Id id =
        token_buffer_.Append(std::move(inline_sched_switch));
    auto* queue = GetQueue(cpu + 1);
    queue->Append(timestamp, TimestampedEvent::Type::kInlineSchedSwitch, id);
    UpdateAppendMaxTs(queue);
  }

  inline void PushInlineFtraceEvent(uint32_t cpu,
                                    int64_t timestamp,
                                    InlineSchedWaking inline_sched_waking) {
    TraceTokenBuffer::Id id =
        token_buffer_.Append(std::move(inline_sched_waking));
    auto* queue = GetQueue(cpu + 1);
    queue->Append(timestamp, TimestampedEvent::Type::kInlineSchedWaking, id);
    UpdateAppendMaxTs(queue);
  }

  void ExtractEventsForced() {
    BumpAllocator::AllocId end_id = token_buffer_.PastTheEndAllocId();
    SortAndExtractEventsUntilAllocId(end_id);
    for (const auto& queue : queues_) {
      PERFETTO_DCHECK(queue.events_.empty());
    }
    queues_.clear();

    alloc_id_for_extraction_ = end_id;
    flushes_since_extraction_ = 0;
  }

  void NotifyFlushEvent() { flushes_since_extraction_++; }

  void NotifyReadBufferEvent() {
    if (sorting_mode_ == SortingMode::kFullSort ||
        flushes_since_extraction_ < 2) {
      return;
    }

    SortAndExtractEventsUntilAllocId(alloc_id_for_extraction_);
    alloc_id_for_extraction_ = token_buffer_.PastTheEndAllocId();
    flushes_since_extraction_ = 0;
  }

  int64_t max_timestamp() const { return append_max_ts_; }

 private:
  struct TimestampedEvent {
    enum class Type : uint8_t {
      kFtraceEvent,
      kTraceBlobView,
      kTracePacket,
      kInlineSchedSwitch,
      kInlineSchedWaking,
      kJsonValue,
      kFuchsiaRecord,
      kTrackEvent,
      kSystraceLine,
      kMax = kSystraceLine,
    };

    // Number of bits required to store the max element in |Type|.
    static constexpr uint32_t kMaxTypeBits = 4;
    static_assert(static_cast<uint8_t>(Type::kMax) <= (1 << kMaxTypeBits),
                  "Max type does not fit inside storage");

    // The timestamp of this event.
    int64_t ts;

    // The fields inside BumpAllocator::AllocId of this tokenized object
    // corresponding to this event.
    uint64_t chunk_index : BumpAllocator::kChunkIndexAllocIdBits;
    uint64_t chunk_offset : BumpAllocator::kChunkOffsetAllocIdBits;

    // The type of this event. GCC7 does not like bit-field enums (see
    // https://stackoverflow.com/questions/36005063/gcc-suppress-warning-too-small-to-hold-all-values-of)
    // so use an uint64_t instead and cast to the enum type.
    uint64_t event_type : kMaxTypeBits;

    BumpAllocator::AllocId alloc_id() const {
      return BumpAllocator::AllocId{chunk_index, chunk_offset};
    }

    // For std::lower_bound().
    static inline bool Compare(const TimestampedEvent& x, int64_t ts) {
      return x.ts < ts;
    }

    // For std::sort().
    inline bool operator<(const TimestampedEvent& evt) const {
      return std::tie(ts, chunk_index, chunk_offset) <
             std::tie(evt.ts, evt.chunk_index, evt.chunk_offset);
    }
  };
  static_assert(sizeof(TimestampedEvent) == 16,
                "TimestampedEvent must be equal to 16 bytes");
  static_assert(std::is_trivially_copyable<TimestampedEvent>::value,
                "TimestampedEvent must be trivially copyable");
  static_assert(std::is_trivially_move_assignable<TimestampedEvent>::value,
                "TimestampedEvent must be trivially move assignable");
  static_assert(std::is_trivially_move_constructible<TimestampedEvent>::value,
                "TimestampedEvent must be trivially move constructible");
  static_assert(std::is_nothrow_swappable<TimestampedEvent>::value,
                "TimestampedEvent must be trivially swappable");

  struct Queue {
    void Append(int64_t ts,
                TimestampedEvent::Type type,
                TraceTokenBuffer::Id id) {
      {
        TimestampedEvent event;
        event.ts = ts;
        event.chunk_index = id.alloc_id.chunk_index;
        event.chunk_offset = id.alloc_id.chunk_offset;
        event.event_type = static_cast<uint8_t>(type);
        events_.emplace_back(std::move(event));
      }

      // Events are often seen in order.
      if (PERFETTO_LIKELY(ts >= max_ts_)) {
        max_ts_ = ts;
      } else {
        // The event is breaking ordering. The first time it happens, keep
        // track of which index we are at. We know that everything before that
        // is sorted (because events were pushed monotonically). Everything
        // after that index, instead, will need a sorting pass before moving
        // events to the next pipeline stage.
        if (sort_start_idx_ == 0) {
          PERFETTO_DCHECK(events_.size() >= 2);
          sort_start_idx_ = events_.size() - 1;
          sort_min_ts_ = ts;
        } else {
          sort_min_ts_ = std::min(sort_min_ts_, ts);
        }
      }

      min_ts_ = std::min(min_ts_, ts);
      PERFETTO_DCHECK(min_ts_ <= max_ts_);
    }

    bool needs_sorting() const { return sort_start_idx_ != 0; }
    void Sort();

    base::CircularQueue<TimestampedEvent> events_;
    int64_t min_ts_ = std::numeric_limits<int64_t>::max();
    int64_t max_ts_ = 0;
    size_t sort_start_idx_ = 0;
    int64_t sort_min_ts_ = std::numeric_limits<int64_t>::max();
  };

  void SortAndExtractEventsUntilAllocId(BumpAllocator::AllocId alloc_id);

  inline Queue* GetQueue(size_t index) {
    if (PERFETTO_UNLIKELY(index >= queues_.size()))
      queues_.resize(index + 1);
    return &queues_[index];
  }

  inline void AppendNonFtraceEvent(int64_t ts,
                                   TimestampedEvent::Type event_type,
                                   TraceTokenBuffer::Id id) {
    Queue* queue = GetQueue(0);
    queue->Append(ts, event_type, id);
    UpdateAppendMaxTs(queue);
  }

  inline void UpdateAppendMaxTs(Queue* queue) {
    append_max_ts_ = std::max(append_max_ts_, queue->max_ts_);
  }

  void ParseTracePacket(const TimestampedEvent&);
  void ParseFtracePacket(uint32_t cpu, const TimestampedEvent&);

  void MaybeExtractEvent(size_t queue_idx, const TimestampedEvent&);
  void ExtractAndDiscardTokenizedObject(const TimestampedEvent& event);

  TraceTokenBuffer::Id GetTokenBufferId(const TimestampedEvent& event) {
    return TraceTokenBuffer::Id{event.alloc_id()};
  }

  TraceProcessorContext* context_ = nullptr;
  std::unique_ptr<TraceParser> parser_;

  // Whether we should ignore incremental extraction and just wait for
  // forced extractionn at the end of the trace.
  SortingMode sorting_mode_ = SortingMode::kDefault;

  // Buffer for storing tokenized objects while the corresponding events are
  // being sorted.
  TraceTokenBuffer token_buffer_;

  // The AllocId until which events should be extracted. Set based
  // on the AllocId in |OnReadBuffer|.
  BumpAllocator::AllocId alloc_id_for_extraction_ =
      token_buffer_.PastTheEndAllocId();

  // The number of flushes which have happened since the last incremental
  // extraction.
  uint32_t flushes_since_extraction_ = 0;

  // queues_[0] is the general (non-ftrace) queue.
  // queues_[1] is the ftrace queue for CPU(0).
  // queues_[x] is the ftrace queue for CPU(x - 1).
  std::vector<Queue> queues_;

  // max(e.ts for e appended to the sorter)
  int64_t append_max_ts_ = 0;

  // Used for performance tests. True when setting
  // TRACE_PROCESSOR_SORT_ONLY=1.
  bool bypass_next_stage_for_testing_ = false;

  // max(e.ts for e pushed to next stage)
  int64_t latest_pushed_event_ts_ = std::numeric_limits<int64_t>::min();
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_H_
