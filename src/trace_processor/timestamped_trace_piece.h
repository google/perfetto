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
#include "src/trace_processor/fuchsia_provider_view.h"
#include "src/trace_processor/proto_incremental_state.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
#include <json/value.h>
#else
// Json traces are only supported in standalone and Chromium builds.
namespace Json {
class Value {};
}  // namespace Json
#endif

namespace perfetto {
namespace trace_processor {

struct InlineSchedSwitch {
  int64_t prev_state;
  int32_t next_pid;
  int32_t next_prio;
  StringId next_comm;
};

// Discriminated union of events that are cannot be easily read from the
// mapped trace.
struct InlineEvent {
  enum class Type { kInvalid = 0, kSchedSwitch };

  static InlineEvent SchedSwitch(InlineSchedSwitch content) {
    InlineEvent evt;
    evt.type = Type::kSchedSwitch;
    evt.sched_switch = content;
    return evt;
  }

  Type type = Type::kInvalid;
  union {
    InlineSchedSwitch sched_switch;
  };
};

// A TimestampedTracePiece is (usually a reference to) a piece of a trace that
// is sorted by TraceSorter.
struct TimestampedTracePiece {
  TimestampedTracePiece(
      int64_t ts,
      uint64_t idx,
      TraceBlobView tbv,
      ProtoIncrementalState::PacketSequenceState* sequence_state)
      : TimestampedTracePiece(ts,
                              /*thread_ts=*/0,
                              /*thread_instructions=*/0,
                              idx,
                              std::move(tbv),
                              /*value=*/nullptr,
                              /*fpv=*/nullptr,
                              /*sequence_state=*/sequence_state,
                              InlineEvent{}) {}

  TimestampedTracePiece(int64_t ts, uint64_t idx, TraceBlobView tbv)
      : TimestampedTracePiece(ts,
                              /*thread_ts=*/0,
                              /*thread_instructions=*/0,
                              idx,
                              std::move(tbv),
                              /*value=*/nullptr,
                              /*fpv=*/nullptr,
                              /*sequence_state=*/nullptr,
                              InlineEvent{}) {}

  TimestampedTracePiece(int64_t ts,
                        uint64_t idx,
                        std::unique_ptr<Json::Value> value)
      : TimestampedTracePiece(ts,
                              /*thread_ts=*/0,
                              /*thread_instructions=*/0,
                              idx,
                              // TODO(dproy): Stop requiring TraceBlobView in
                              // TimestampedTracePiece.
                              TraceBlobView(nullptr, 0, 0),
                              std::move(value),
                              /*fpv=*/nullptr,
                              /*sequence_state=*/nullptr,
                              InlineEvent{}) {}

  TimestampedTracePiece(int64_t ts,
                        uint64_t idx,
                        TraceBlobView tbv,
                        std::unique_ptr<FuchsiaProviderView> fpv)
      : TimestampedTracePiece(ts,
                              /*thread_ts=*/0,
                              /*thread_instructions=*/0,
                              idx,
                              std::move(tbv),
                              /*value=*/nullptr,
                              std::move(fpv),
                              /*sequence_state=*/nullptr,
                              InlineEvent{}) {}

  TimestampedTracePiece(
      int64_t ts,
      int64_t thread_ts,
      int64_t thread_instructions,
      uint64_t idx,
      TraceBlobView tbv,
      ProtoIncrementalState::PacketSequenceState* sequence_state)
      : TimestampedTracePiece(ts,
                              thread_ts,
                              thread_instructions,
                              idx,
                              std::move(tbv),
                              /*value=*/nullptr,
                              /*fpv=*/nullptr,
                              sequence_state,
                              InlineEvent{}) {}

  TimestampedTracePiece(int64_t ts, uint64_t idx, InlineEvent inline_evt)
      : TimestampedTracePiece(ts,
                              /*thread_ts=*/0,
                              /*thread_instructions=*/0,
                              idx,
                              /*tbv=*/TraceBlobView(nullptr, 0, 0),
                              /*value=*/nullptr,
                              /*fpv=*/nullptr,
                              /*sequence_state=*/nullptr,
                              inline_evt) {}

  TimestampedTracePiece(
      int64_t ts,
      int64_t thread_ts,
      int64_t thread_instructions,
      uint64_t idx,
      TraceBlobView tbv,
      std::unique_ptr<Json::Value> value,
      std::unique_ptr<FuchsiaProviderView> fpv,
      ProtoIncrementalState::PacketSequenceState* sequence_state,
      InlineEvent inline_evt)
      : json_value(std::move(value)),
        fuchsia_provider_view(std::move(fpv)),
        packet_sequence_state(sequence_state),
        packet_sequence_state_generation(
            sequence_state ? sequence_state->current_generation() : 0),
        timestamp(ts),
        thread_timestamp(thread_ts),
        thread_instruction_count(thread_instructions),
        packet_idx_(idx),
        blob_view(std::move(tbv)),
        inline_event(inline_evt) {}

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
  size_t packet_sequence_state_generation;

  int64_t timestamp;
  int64_t thread_timestamp;
  int64_t thread_instruction_count;
  uint64_t packet_idx_;
  TraceBlobView blob_view;
  InlineEvent inline_event;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TIMESTAMPED_TRACE_PIECE_H_
