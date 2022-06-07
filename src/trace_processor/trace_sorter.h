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

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/timestamped_trace_piece.h"
#include "src/trace_processor/trace_sorter_queue.h"

namespace perfetto {
namespace trace_processor {

namespace trace_sorter_internal {
class VariadicQueue;
}  // namespace trace_sorter_internal

class PacketSequenceState;
struct SystraceLine;

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
 private:
  using VariadicQueue = trace_sorter_internal::VariadicQueue;

 public:
  enum class SortingMode {
    kDefault,
    kFullSort,
  };

  TraceSorter(TraceProcessorContext* context,
              std::unique_ptr<TraceParser> parser,
              SortingMode);

  inline void PushTracePacket(int64_t timestamp,
                              PacketSequenceState* state,
                              TraceBlobView event) {
    uint32_t offset = variadic_queue_.Append(
        TracePacketData{std::move(event), state->current_generation()});
    AppendNonFtraceEvent(timestamp, offset, Type::kTracePacket);
  }

  inline void PushJsonValue(int64_t timestamp, std::string json_value) {
    uint32_t offset = variadic_queue_.Append(std::move(json_value));
    AppendNonFtraceEvent(timestamp, offset, Type::kJsonValue);
  }

  inline void PushFuchsiaRecord(int64_t timestamp,
                                std::unique_ptr<FuchsiaRecord> fuchsia_record) {
    uint32_t offset = variadic_queue_.Append(std::move(fuchsia_record));
    AppendNonFtraceEvent(timestamp, offset, Type::kFuchsiaRecord);
  }

  inline void PushSystraceLine(std::unique_ptr<SystraceLine> systrace_line) {
    auto ts = systrace_line->ts;
    auto offset = variadic_queue_.Append(std::move(systrace_line));
    AppendNonFtraceEvent(ts, offset, Type::kSystraceLine);
  }

  inline void PushTrackEventPacket(
      int64_t timestamp,
      std::unique_ptr<TrackEventData> track_event) {
    uint32_t offset = variadic_queue_.Append(std::move(track_event));
    AppendNonFtraceEvent(timestamp, offset, Type::kTrackEvent);
  }

  inline void PushFtraceEvent(uint32_t cpu,
                              int64_t timestamp,
                              TraceBlobView event,
                              PacketSequenceState* state) {
    auto* queue = GetQueue(cpu + 1);
    uint32_t offset = variadic_queue_.Append(
        FtraceEventData{std::move(event), state->current_generation()});
    queue->Append(TimestampedDescriptor{
        timestamp, Descriptor(offset, Type::kFtraceEvent)});
    UpdateGlobalTs(queue);
  }
  inline void PushInlineFtraceEvent(uint32_t cpu,
                                    int64_t timestamp,
                                    InlineSchedSwitch inline_sched_switch) {
    // TODO(rsavitski): if a trace has a mix of normal & "compact" events (being
    // pushed through this function), the ftrace batches will no longer be fully
    // sorted by timestamp. In such situations, we will have to sort at the end
    // of the batch. We can do better as both sub-sequences are sorted however.
    // Consider adding extra queues, or pushing them in a merge-sort fashion
    // // instead.
    auto* queue = GetQueue(cpu + 1);
    uint32_t offset = variadic_queue_.Append(inline_sched_switch);
    queue->Append(TimestampedDescriptor{
        timestamp, Descriptor(offset, Type::kInlineSchedSwitch)});
    UpdateGlobalTs(queue);
  }
  inline void PushInlineFtraceEvent(uint32_t cpu,
                                    int64_t timestamp,
                                    InlineSchedWaking inline_sched_waking) {
    auto* queue = GetQueue(cpu + 1);

    uint32_t offset = variadic_queue_.Append(inline_sched_waking);
    queue->Append(TimestampedDescriptor{
        timestamp,
        Descriptor(offset, TimestampedTracePiece::Type::kInlineSchedWaking)});
    UpdateGlobalTs(queue);
  }

  void ExtractEventsForced() {
    uint32_t cur_mem_block_offset = variadic_queue_.NextOffset();
    SortAndExtractEventsUntilPacket(cur_mem_block_offset);
    queues_.resize(0);

    offset_for_extraction_ = cur_mem_block_offset;
    flushes_since_extraction_ = 0;
  }

  void NotifyFlushEvent() { flushes_since_extraction_++; }

  void NotifyReadBufferEvent() {
    if (sorting_mode_ == SortingMode::kFullSort ||
        flushes_since_extraction_ < 2) {
      return;
    }

    SortAndExtractEventsUntilPacket(offset_for_extraction_);
    offset_for_extraction_ = variadic_queue_.NextOffset();
    flushes_since_extraction_ = 0;
  }

  int64_t max_timestamp() const { return global_max_ts_; }

 private:
  using Type = TimestampedTracePiece::Type;
  // Stores offset and type of metadat.
  struct Descriptor {
   public:
    static constexpr uint8_t kTypeBits = 4;
    static constexpr uint64_t kTypeMask = (1 << kTypeBits) - 1;
    static constexpr uint64_t kOffsetShift = kTypeBits;
    static constexpr uint64_t kMaxType = kTypeMask;

    static_assert(static_cast<uint8_t>(TimestampedTracePiece::Type::kSize) <=
                      kTypeMask,
                  "Too many bits for type");

    Descriptor(uint32_t offset, TimestampedTracePiece::Type type)
        : packed_value_((static_cast<uint64_t>(offset) << kOffsetShift) |
                        static_cast<uint64_t>(type)) {}

    uint32_t offset() const {
      return static_cast<uint32_t>(packed_value_ >> kOffsetShift);
    }

    TimestampedTracePiece::Type type() const {
      return static_cast<TimestampedTracePiece::Type>(packed_value_ &
                                                      kTypeMask);
    }

   private:
    uint64_t packed_value_ = 0;
  };

  struct TimestampedDescriptor {
    int64_t ts;
    Descriptor descriptor;

    // For std::lower_bound().
    static inline bool Compare(const TimestampedDescriptor& x, int64_t ts) {
      return x.ts < ts;
    }

    // For std::sort().
    inline bool operator<(const TimestampedDescriptor& desc) const {
      return ts < desc.ts ||
             (ts == desc.ts && descriptor.offset() < desc.descriptor.offset());
    }

    // For std::sort(). Without this the compiler will fall back on invoking
    // move operators on temporary objects.
    friend void swap(TimestampedDescriptor& a, TimestampedDescriptor& b) {
      // TimestampedDescriptor is 16 bytes + trivially swappable so it can be
      // done without doing any moving.
      using AS =
          typename std::aligned_storage<sizeof(TimestampedDescriptor),
                                        alignof(TimestampedDescriptor)>::type;
      using std::swap;
      swap(reinterpret_cast<AS&>(a), reinterpret_cast<AS&>(b));
    }
  };

  static_assert(sizeof(TimestampedDescriptor) == 16,
                "TimestampeDescriptor cannot grow beyond 16 bytes");

  static constexpr uint32_t kNoBatch = std::numeric_limits<uint32_t>::max();

  struct Queue {
    inline void Append(TimestampedDescriptor ts_desc) {
      auto ts = ts_desc.ts;
      events_.emplace_back(std::move(ts_desc));
      min_ts_ = std::min(min_ts_, ts);

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

      PERFETTO_DCHECK(min_ts_ <= max_ts_);
    }

    bool needs_sorting() const { return sort_start_idx_ != 0; }
    void Sort();

    base::CircularQueue<TimestampedDescriptor> events_;
    int64_t min_ts_ = std::numeric_limits<int64_t>::max();
    int64_t max_ts_ = 0;
    size_t sort_start_idx_ = 0;
    int64_t sort_min_ts_ = std::numeric_limits<int64_t>::max();
  };

  void SortAndExtractEventsUntilPacket(uint64_t limit_packet_idx);

  inline Queue* GetQueue(size_t index) {
    if (PERFETTO_UNLIKELY(index >= queues_.size()))
      queues_.resize(index + 1);
    return &queues_[index];
  }

  inline void AppendNonFtraceEvent(int64_t ts, uint32_t offset, Type type) {
    Queue* queue = GetQueue(0);
    queue->Append(TimestampedDescriptor{ts, Descriptor{offset, type}});
    UpdateGlobalTs(queue);
  }

  inline void UpdateGlobalTs(Queue* queue) {
    global_min_ts_ = std::min(global_min_ts_, queue->min_ts_);
    global_max_ts_ = std::max(global_max_ts_, queue->max_ts_);
  }

  template <typename T>
  void ParseTracePacket(size_t queue_idx,
                        const TimestampedDescriptor& ts_desc) {
    if (queue_idx == 0) {
      // queues_[0] is for non-ftrace packets.
      parser_->ParseTracePacket(
          ts_desc.ts,
          TimestampedTracePiece(ts_desc.ts, variadic_queue_.Evict<T>(
                                                ts_desc.descriptor.offset())));
    } else {
      // Ftrace queues start at offset 1. So queues_[1] = cpu[0] and so on.
      uint32_t cpu = static_cast<uint32_t>(queue_idx - 1);
      parser_->ParseFtracePacket(
          cpu, ts_desc.ts,
          TimestampedTracePiece(ts_desc.ts, variadic_queue_.Evict<T>(
                                                ts_desc.descriptor.offset())));
    }
  }

  void MaybePushEvent(size_t queue_idx, const TimestampedDescriptor& ts_desc)
      PERFETTO_ALWAYS_INLINE;

  TraceProcessorContext* context_;
  std::unique_ptr<TraceParser> parser_;

  // Whether we should ignore incremental extraction and just wait for
  // forced extractionn at the end of the trace.
  SortingMode sorting_mode_ = SortingMode::kDefault;

  // The packet offset until which events should be extracted. Set based
  // on the packet offset in |OnReadBuffer|.
  uint32_t offset_for_extraction_ = 0;

  // The number of flushes which have happened since the last incremental
  // extraction.
  uint32_t flushes_since_extraction_ = 0;

  // Stores the metadata for each event type in a memory efficient manner.
  VariadicQueue variadic_queue_;

  // queues_[0] is the general (non-ftrace) queue.
  // queues_[1] is the ftrace queue for CPU(0).
  // queues_[x] is the ftrace queue for CPU(x - 1).
  std::vector<Queue> queues_;

  // max(e.timestamp for e in queues_).
  int64_t global_max_ts_ = 0;

  // min(e.timestamp for e in queues_).
  int64_t global_min_ts_ = std::numeric_limits<int64_t>::max();

  // Used for performance tests. True when setting
  // TRACE_PROCESSOR_SORT_ONLY=1.
  bool bypass_next_stage_for_testing_ = false;

  // max(e.ts for e pushed to next stage)
  int64_t latest_pushed_event_ts_ = std::numeric_limits<int64_t>::min();
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_SORTER_H_
