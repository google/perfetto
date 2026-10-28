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
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/sorter/trace_token_buffer.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/bump_allocator.h"

namespace perfetto::trace_processor {

// This class takes care of sorting events parsed from the trace stream in
// arbitrary order and pushing them to the next pipeline stages (parsing) in
// order. In order to support streaming use-cases, sorting happens within a
// window.
//
// Events are held in the TraceSorter staging area (blocks_) until either:
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
// The events of all streams are appended, in arrival order, to shared blocks
// of up to kBlockEvents events. Arrival order is token buffer (alloc id)
// order, so sorting by (timestamp, alloc id) gives the order events must be
// extracted in, whichever stream they came from.
//
// A block is sorted as soon as it fills, while its events are still in cache,
// with a stable radix sort on timestamps. Extraction merges the sorted blocks
// through a heap. Arrival is roughly in time order, so blocks cover mostly
// disjoint ranges and the merge takes long runs from each. Blocks grow as
// events arrive and shrink as they drain, so they hold memory in proportion
// to the pending events.
class TraceSorter {
 public:
  template <typename T>
  class Stream;
  template <typename T, typename Derived>
  class Sink;

  enum class SortingMode : uint8_t {
    kDefault,
    kFullSort,
  };
  enum class EventHandling : uint8_t {
    // Indicates that events should be sorted and pushed to the parsing stage.
    kSortAndPush,

    // Indicates that events should be sorted but then dropped before pushing
    // to the parsing stage.
    // Used for performance analysis of the sorter.
    kSortAndDrop,

    // Indicates that the events should be dropped as soon as they enter the
    // sorter.
    // Used in cases where we only want to perform tokenization: dropping data
    // when it hits the sorter is much cleaner than trying to handle this
    // at every different tokenizer.
    kDrop,
  };

  TraceSorter(TraceProcessorContext*,
              SortingMode,
              EventHandling = EventHandling::kSortAndPush);

  ~TraceSorter();

  template <typename SinkType>
  std::unique_ptr<TraceSorter::Stream<typename SinkType::type>> CreateStream(
      std::unique_ptr<SinkType> sink);

  SortingMode sorting_mode() const { return sorting_mode_; }
  bool SetSortingMode(SortingMode sorting_mode);

  void ExtractEventsForced() {
    BumpAllocator::AllocId end_id = token_buffer_.PastTheEndAllocId();
    SortAndExtractEventsUntilAllocId(end_id);
    PERFETTO_CHECK(blocks_.empty());

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

  // The most events in a block. 64K events (1MB): small enough to sort in
  // cache, large enough that blocks are few to merge.
  static constexpr uint32_t kBlockEvents = 64 * 1024;

 private:
  class UntypedSink;

  // The bits of TimestampedEvent left for the stream index.
  static constexpr uint32_t kStreamBits = 64 - BumpAllocator::kMaxIdBits - 1;

  struct TimestampedEvent {
    // The timestamp of this event.
    int64_t ts;

    // The fields inside BumpAllocator::AllocId of this tokenized object
    // corresponding to this event.
    uint64_t chunk_index : BumpAllocator::kChunkIndexAllocIdBits;
    uint64_t chunk_offset : BumpAllocator::kChunkOffsetAllocIdBits;

    // Indicates whether the event is a JSON event: may have special rules based
    // on the event type and duration.
    // TODO(sashwinbalaji): Update to bool.
    uint64_t is_json_event : 1;

    // The index into |sinks_| of the stream the event was pushed to.
    uint64_t stream : kStreamBits;

    BumpAllocator::AllocId alloc_id() const {
      return BumpAllocator::AllocId{chunk_index, chunk_offset};
    }

    // For std::sort().
    bool operator<(const TimestampedEvent& evt) const {
      return std::tie(ts, chunk_index, chunk_offset) <
             std::tie(evt.ts, evt.chunk_index, evt.chunk_offset);
    }

    struct SlowOperatorLess {
      // For std::sort() in slow mode.
      bool operator()(const TimestampedEvent& a,
                      const TimestampedEvent& b) const {
        int64_t a_key =
            a.is_json_event
                ? KeyForType(*buffer.Get<JsonEvent>(GetTokenBufferId(a)))
                : std::numeric_limits<int64_t>::max();
        int64_t b_key =
            b.is_json_event
                ? KeyForType(*buffer.Get<JsonEvent>(GetTokenBufferId(b)))
                : std::numeric_limits<int64_t>::max();
        return std::tie(a.ts, a_key, a.chunk_index, a.chunk_offset) <
               std::tie(b.ts, b_key, b.chunk_index, b.chunk_offset);
      }

      static int64_t KeyForType(const JsonEvent& event) {
        switch (event.phase) {
          case 'E':
            return std::numeric_limits<int64_t>::min();
          case 'X':
            return std::numeric_limits<int64_t>::max() - event.dur;
          default:
            return std::numeric_limits<int64_t>::max();
        }
        PERFETTO_FATAL("For GCC");
      }

      TraceTokenBuffer& buffer;
    };
  };

  static_assert(sizeof(TimestampedEvent) == 16,
                "TimestampedEvent must be equal to 16 bytes");
  static_assert(std::is_trivially_copyable_v<TimestampedEvent>,
                "TimestampedEvent must be trivially copyable");
  static_assert(std::is_trivially_move_assignable_v<TimestampedEvent>,
                "TimestampedEvent must be trivially move assignable");
  static_assert(std::is_trivially_move_constructible_v<TimestampedEvent>,
                "TimestampedEvent must be trivially move constructible");
  static_assert(std::is_nothrow_swappable_v<TimestampedEvent>,
                "TimestampedEvent must be trivially swappable");

  // Events in arrival order until the block is sorted; |head| is the first
  // not yet extracted.
  struct Block {
    std::vector<TimestampedEvent> events;
    size_t head = 0;
    int64_t max_ts = std::numeric_limits<int64_t>::min();
    // Whether |events| are in extraction order.
    bool sorted = true;
  };

  PERFETTO_ALWAYS_INLINE void Append(int64_t ts,
                                     TraceTokenBuffer::Id id,
                                     uint32_t stream,
                                     bool is_json_event) {
    if (PERFETTO_UNLIKELY(!block_open_ ||
                          blocks_.back().events.size() == kBlockEvents)) {
      OpenBlock();
    }
    Block& block = blocks_.back();
    block.events.push_back({ts, id.alloc_id.chunk_index,
                            id.alloc_id.chunk_offset, is_json_event, stream});
    // Events are often seen in order.
    if (PERFETTO_LIKELY(ts > block.max_ts ||
                        (!use_slow_sorting_ && ts == block.max_ts))) {
      block.max_ts = ts;
    } else {
      block.sorted = false;
    }
    append_max_ts_ = std::max(append_max_ts_, ts);
  }

  // Sorts the open block, if any, and starts a new one.
  void OpenBlock();
  void SortBlock(Block&);
  template <typename Less>
  void MergeBlocks(BumpAllocator::AllocId limit_alloc_id, Less less);

  void SortAndExtractEventsUntilAllocId(BumpAllocator::AllocId alloc_id);

  static TraceTokenBuffer::Id GetTokenBufferId(const TimestampedEvent& event) {
    return TraceTokenBuffer::Id{event.alloc_id()};
  }

  // The sink of each stream, by stream index.
  std::vector<std::unique_ptr<UntypedSink>> sinks_;

  // The pending events of all streams, in arrival order.
  std::vector<Block> blocks_;
  // Whether new events go to the last of |blocks_|.
  bool block_open_ = false;
  // MergeBlocks()' heap of block indices, kept to reuse its memory.
  std::vector<uint32_t> block_heap_;
  // SortBlock()'s radix sort scratch, kept to reuse its memory until the next
  // extraction.
  std::vector<TimestampedEvent> radix_scratch_;
  std::vector<uint32_t> radix_counts_;

  // Whether events are being extracted: the merge assumes none are pushed
  // meanwhile.
  bool extracting_ = false;

  // Whether we should ignore incremental extraction and just wait for
  // forced extractionn at the end of the trace.
  SortingMode sorting_mode_ = SortingMode::kDefault;

  TraceStorage* storage_;
  GlobalStatsTracker* global_stats_tracker_;

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

  // max(e.ts for e appended to the sorter)
  int64_t append_max_ts_ = 0;

  // How events pushed into the sorter should be handled.
  EventHandling event_handling_ = EventHandling::kSortAndPush;

  // max(e.ts for e pushed to next stage)
  int64_t latest_pushed_event_ts_ = std::numeric_limits<int64_t>::min();

  // Whether blocks are sorted and merged in SlowOperatorLess order, needed
  // once a JSON 'X' event is pushed.
  bool use_slow_sorting_ = false;
};

// The non-templated base class for polymorphism.
class TraceSorter::UntypedSink {
 public:
  virtual ~UntypedSink();

  // The generic dispatch method called by the sorter.
  virtual void OnSortedEvent(TraceSorter* sorter,
                             int64_t ts,
                             TraceTokenBuffer::Id id) = 0;
  virtual void OnDiscardedEvent(TraceSorter* sorter,
                                TraceTokenBuffer::Id id) = 0;
};

// The type-safe interface that parsers implement.
template <typename T, typename Derived>
class TraceSorter::Sink : public TraceSorter::UntypedSink {
 public:
  using type = T;

  ~Sink() override = default;

  // Implements the generic dispatch method from the base class.
  void OnSortedEvent(TraceSorter* sorter,
                     int64_t ts,
                     TraceTokenBuffer::Id id) final {
    // Safely extracts the data of the expected type T...
    T data = sorter->token_buffer_.Extract<T>(id);
    // ...and calls the type-safe method on the derived class.
    static_cast<Derived*>(this)->Parse(ts, std::move(data));
  }

  void OnDiscardedEvent(TraceSorter* sorter, TraceTokenBuffer::Id id) final {
    // Safely extracts and destroys the data of the expected type T.
    T res = sorter->token_buffer_.Extract<T>(id);
    base::ignore_result(res);
  }
};

// This is the handle a tokenizer uses to push data.
template <typename T>
class TraceSorter::Stream {
 public:
  // Public API for tokenizers.
  void Push(int64_t ts, T data) {
    // Record the negative timestamp drop even when events are dropped
    // (tokenize-only mode): the UI's merge dialog dry-runs a tokenize-only
    // pass and relies on this stat to warn about misconfigured offsets.
    if (PERFETTO_UNLIKELY(ts < 0)) {
      sorter_->global_stats_tracker_->IncrementGlobalStats(
          stats::trace_sorter_negative_timestamp_dropped);
      return;
    }
    if (PERFETTO_UNLIKELY(sorter_->event_handling_ == EventHandling::kDrop)) {
      return;
    }
    if constexpr (std::is_same_v<T, JsonEvent>) {
      if (PERFETTO_UNLIKELY(data.phase == 'X' && !sorter_->use_slow_sorting_)) {
        sorter_->use_slow_sorting_ = true;
        // Sorted blocks are not in the slow order.
        for (Block& block : sorter_->blocks_) {
          block.sorted = false;
        }
      }
    }
    PERFETTO_DCHECK(!sorter_->extracting_);
    TraceTokenBuffer::Id id = sorter_->token_buffer_.Append(std::move(data));
    sorter_->Append(ts, id, static_cast<uint32_t>(stream_idx_),
                    std::is_same_v<T, JsonEvent>);
  }

 private:
  // Only the TraceSorter can create streams.
  friend class TraceSorter;
  Stream(TraceSorter* sorter, size_t stream_idx)
      : sorter_(sorter), stream_idx_(stream_idx) {}

  TraceSorter* sorter_;
  size_t stream_idx_;
};

// The factory method implementation (in the header due to templates).
template <typename SinkType>
std::unique_ptr<TraceSorter::Stream<typename SinkType::type>>
TraceSorter::CreateStream(std::unique_ptr<SinkType> sink) {
  using T = typename SinkType::type;
  static_assert(
      std::is_base_of_v<Sink<T, SinkType>, SinkType>,
      "CreateStream sink must inherit from TraceSorter::Sink<T, Derived>");

  // The sorter owns the sink, upcast to the base class.
  size_t stream_idx = sinks_.size();
  PERFETTO_CHECK(stream_idx < (size_t{1} << kStreamBits));
  sinks_.emplace_back(std::move(sink));

  // Create and return the type-safe input handle.
  return std::unique_ptr<Stream<T>>(new Stream<T>(this, stream_idx));
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_SORTER_TRACE_SORTER_H_
