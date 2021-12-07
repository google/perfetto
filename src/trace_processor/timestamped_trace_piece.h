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

#ifndef SRC_TRACE_PROCESSOR_TIMESTAMPED_TRACE_PIECE_H_
#define SRC_TRACE_PROCESSOR_TIMESTAMPED_TRACE_PIECE_H_

#include "perfetto/base/build_config.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_record.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

// GCC can't figure out the relationship between TimestampedTracePiece's type
// and the union, and thus thinks that we may be moving or destroying
// uninitialized data in the move constructors / destructors. Disable those
// warnings for TimestampedTracePiece and the types it contains.
#if PERFETTO_BUILDFLAG(PERFETTO_COMPILER_GCC)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace perfetto {
namespace trace_processor {

struct InlineSchedSwitch {
  int64_t prev_state;
  int32_t next_pid;
  int32_t next_prio;
  StringId next_comm;
};

struct InlineSchedWaking {
  int32_t pid;
  int32_t target_cpu;
  int32_t prio;
  StringId comm;
};

struct TracePacketData {
  TraceBlobView packet;
  RefPtr<PacketSequenceStateGeneration> sequence_state;
};

struct FtraceEventData {
  TraceBlobView event;
  RefPtr<PacketSequenceStateGeneration> sequence_state;
};

struct TrackEventData : public TracePacketData {
  TrackEventData(TraceBlobView pv,
                 RefPtr<PacketSequenceStateGeneration> generation)
      : TracePacketData{std::move(pv), std::move(generation)} {}

  static constexpr size_t kMaxNumExtraCounters = 8;

  base::Optional<int64_t> thread_timestamp;
  base::Optional<int64_t> thread_instruction_count;
  double counter_value = 0;
  std::array<double, kMaxNumExtraCounters> extra_counter_values = {};
};

// On Windows std::aligned_storage was broken before VS 2017 15.8 and the
// compiler (even clang-cl) requires -D_ENABLE_EXTENDED_ALIGNED_STORAGE. Given
// the alignment here is purely a performance enhancment with no other
// functional requirement, disable it on Win.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#define PERFETTO_TTS_ALIGNMENT alignas(64)
#else
#define PERFETTO_TTS_ALIGNMENT
#endif

// A TimestampedTracePiece is (usually a reference to) a piece of a trace that
// is sorted by TraceSorter.
struct PERFETTO_TTS_ALIGNMENT TimestampedTracePiece {
  enum class Type {
    kInvalid = 0,
    kFtraceEvent,
    kTracePacket,
    kInlineSchedSwitch,
    kInlineSchedWaking,
    kJsonValue,
    kFuchsiaRecord,
    kTrackEvent,
    kSystraceLine,
  };

  TimestampedTracePiece(int64_t ts,
                        uint64_t idx,
                        TraceBlobView tbv,
                        RefPtr<PacketSequenceStateGeneration> sequence_state)
      : packet_data{std::move(tbv), std::move(sequence_state)},
        timestamp(ts),
        packet_idx(idx),
        type(Type::kTracePacket) {}

  TimestampedTracePiece(int64_t ts, uint64_t idx, FtraceEventData fed)
      : ftrace_event(std::move(fed)),
        timestamp(ts),
        packet_idx(idx),
        type(Type::kFtraceEvent) {}

  TimestampedTracePiece(int64_t ts, uint64_t idx, std::string value)
      : json_value(std::move(value)),
        timestamp(ts),
        packet_idx(idx),
        type(Type::kJsonValue) {}

  TimestampedTracePiece(int64_t ts,
                        uint64_t idx,
                        std::unique_ptr<FuchsiaRecord> fr)
      : fuchsia_record(std::move(fr)),
        timestamp(ts),
        packet_idx(idx),
        type(Type::kFuchsiaRecord) {}

  TimestampedTracePiece(int64_t ts,
                        uint64_t idx,
                        std::unique_ptr<TrackEventData> ted)
      : track_event_data(std::move(ted)),
        timestamp(ts),
        packet_idx(idx),
        type(Type::kTrackEvent) {}

  TimestampedTracePiece(int64_t ts,
                        uint64_t idx,
                        std::unique_ptr<SystraceLine> ted)
      : systrace_line(std::move(ted)),
        timestamp(ts),
        packet_idx(idx),
        type(Type::kSystraceLine) {}

  TimestampedTracePiece(int64_t ts, uint64_t idx, InlineSchedSwitch iss)
      : sched_switch(std::move(iss)),
        timestamp(ts),
        packet_idx(idx),
        type(Type::kInlineSchedSwitch) {}

  TimestampedTracePiece(int64_t ts, uint64_t idx, InlineSchedWaking isw)
      : sched_waking(std::move(isw)),
        timestamp(ts),
        packet_idx(idx),
        type(Type::kInlineSchedWaking) {}

  TimestampedTracePiece(TimestampedTracePiece&& ttp) noexcept {
    // Adopt |ttp|'s data. We have to use placement-new to fill the fields
    // because their original values may be uninitialized and thus
    // move-assignment won't work correctly.
    switch (ttp.type) {
      case Type::kInvalid:
        break;
      case Type::kFtraceEvent:
        new (&ftrace_event) FtraceEventData(std::move(ttp.ftrace_event));
        break;
      case Type::kTracePacket:
        new (&packet_data) TracePacketData(std::move(ttp.packet_data));
        break;
      case Type::kInlineSchedSwitch:
        new (&sched_switch) InlineSchedSwitch(std::move(ttp.sched_switch));
        break;
      case Type::kInlineSchedWaking:
        new (&sched_waking) InlineSchedWaking(std::move(ttp.sched_waking));
        break;
      case Type::kJsonValue:
        new (&json_value) std::string(std::move(ttp.json_value));
        break;
      case Type::kFuchsiaRecord:
        new (&fuchsia_record)
            std::unique_ptr<FuchsiaRecord>(std::move(ttp.fuchsia_record));
        break;
      case Type::kTrackEvent:
        new (&track_event_data)
            std::unique_ptr<TrackEventData>(std::move(ttp.track_event_data));
        break;
      case Type::kSystraceLine:
        new (&systrace_line)
            std::unique_ptr<SystraceLine>(std::move(ttp.systrace_line));
    }
    timestamp = ttp.timestamp;
    packet_idx = ttp.packet_idx;
    type = ttp.type;

    // Invalidate |ttp|.
    ttp.type = Type::kInvalid;
  }

  TimestampedTracePiece& operator=(TimestampedTracePiece&& ttp) {
    if (this != &ttp) {
      // First invoke the destructor and then invoke the move constructor
      // inline via placement-new to implement move-assignment.
      this->~TimestampedTracePiece();
      new (this) TimestampedTracePiece(std::move(ttp));
    }
    return *this;
  }

  TimestampedTracePiece(const TimestampedTracePiece&) = delete;
  TimestampedTracePiece& operator=(const TimestampedTracePiece&) = delete;

  ~TimestampedTracePiece() {
    switch (type) {
      case Type::kInvalid:
      case Type::kInlineSchedSwitch:
      case Type::kInlineSchedWaking:
        break;
      case Type::kFtraceEvent:
        ftrace_event.~FtraceEventData();
        break;
      case Type::kTracePacket:
        packet_data.~TracePacketData();
        break;
      case Type::kJsonValue:
        json_value.~basic_string();
        break;
      case Type::kFuchsiaRecord:
        fuchsia_record.~unique_ptr();
        break;
      case Type::kTrackEvent:
        track_event_data.~unique_ptr();
        break;
      case Type::kSystraceLine:
        systrace_line.~unique_ptr();
        break;
    }
  }

  // For std::lower_bound().
  static inline bool Compare(const TimestampedTracePiece& x, int64_t ts) {
    return x.timestamp < ts;
  }

  // For std::sort().
  inline bool operator<(const TimestampedTracePiece& o) const {
    return timestamp < o.timestamp ||
           (timestamp == o.timestamp && packet_idx < o.packet_idx);
  }

  // For std::sort(). Without this the compiler will fall back on invoking
  // move operators on temporary objects.
  friend void swap(TimestampedTracePiece& a, TimestampedTracePiece& b) {
    // We know that TimestampedTracePiece is 64-byte aligned (because of the
    // alignas(64) in the declaration above). We also know that swapping it is
    // trivial and we can just swap the memory without invoking move operators.
    // The cast to aligned_storage below allows the compiler to turn this into
    // a bunch of movaps with large XMM registers (128/256/512 bit depending on
    // -mavx).
    using AS =
        typename std::aligned_storage<sizeof(TimestampedTracePiece),
                                      alignof(TimestampedTracePiece)>::type;
    using std::swap;
    swap(reinterpret_cast<AS&>(a), reinterpret_cast<AS&>(b));
  }

  // Fields ordered for packing.

  // Data for different types of TimestampedTracePiece.
  union {
    FtraceEventData ftrace_event;
    TracePacketData packet_data;
    InlineSchedSwitch sched_switch;
    InlineSchedWaking sched_waking;
    std::string json_value;
    std::unique_ptr<FuchsiaRecord> fuchsia_record;
    std::unique_ptr<TrackEventData> track_event_data;
    std::unique_ptr<SystraceLine> systrace_line;
  };

  int64_t timestamp;
  uint64_t packet_idx;
  Type type;
};

// std::sort<TTS> is an extremely hot path in TraceProcessor (in trace_sorter.h)
// When TTS is 512-bit wide, we can leverage SIMD instructions to swap it by
// declaring it aligned at its own size, without losing any space in the
// CircularQueue due to fragmentation. This makes a 6% difference in the
// ingestion time of a large trace. See the comments above in the swap() above.
static_assert(sizeof(TimestampedTracePiece) <= 64,
              "TimestampedTracePiece cannot grow beyond 64 bytes");

}  // namespace trace_processor
}  // namespace perfetto

#if PERFETTO_BUILDFLAG(PERFETTO_COMPILER_GCC)
#pragma GCC diagnostic pop
#endif

#endif  // SRC_TRACE_PROCESSOR_TIMESTAMPED_TRACE_PIECE_H_
