/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_redaction/merge_synthetic_sched.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

namespace {
bool IsTrue(bool value) {
  return value;
}
}  // namespace

base::Status SyntheticProcessValidator::Validate(const Context& context) const {
  if (!context.synthetic_process) {
    return base::ErrStatus(
        "SyntheticProcessValidator: missing synthetic process.");
  }

  if (context.synthetic_process->tids().empty()) {
    return base::ErrStatus(
        "SyntheticProcessValidator: no synthetic threads in synthetic "
        "process.");
  }

  return base::OkStatus();
}

base::Status MergeSyntheticSched::Transform(const Context& context,
                                            std::string* packet) const {
  PERFETTO_DCHECK(packet);

  protozero::ProtoDecoder decoder(*packet);

  auto ftrace_events_field =
      decoder.FindField(protos::pbzero::TracePacket::kFtraceEventsFieldNumber);
  if (!ftrace_events_field.valid()) {
    return base::OkStatus();
  }

  protozero::ProtoDecoder ftrace_decoder(ftrace_events_field.as_bytes());
  auto compact_sched_field = ftrace_decoder.FindField(
      protos::pbzero::FtraceEventBundle::kCompactSchedFieldNumber);
  if (!compact_sched_field.valid()) {
    return base::OkStatus();
  }

  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
      RETURN_IF_ERROR(
          OnFtraceEvents(context, field, message->set_ftrace_events()));
    } else {
      proto_util::AppendField(field, message.get());
    }
  }

  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

base::Status MergeSyntheticSched::OnFtraceEvents(
    const Context& context,
    protozero::Field ftrace_events,
    protos::pbzero::FtraceEventBundle* message) const {
  PERFETTO_DCHECK(ftrace_events.id() ==
                  protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

  protozero::ProtoDecoder decoder(ftrace_events.as_bytes());

  auto cpu =
      decoder.FindField(protos::pbzero::FtraceEventBundle::kCpuFieldNumber);
  if (PERFETTO_UNLIKELY(!cpu.valid())) {
    return base::ErrStatus(
        "MergeSyntheticSched: missing cpu in ftrace event bundle.");
  }

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() ==
        protos::pbzero::FtraceEventBundle::kCompactSchedFieldNumber) {
      RETURN_IF_ERROR(OnCompSched(context, cpu.as_int32(), field.as_bytes(),
                                  message->set_compact_sched()));
      continue;
    }

    proto_util::AppendField(field, message);
  }

  return base::OkStatus();
}

base::Status MergeSyntheticSched::OnCompSched(
    const Context& context,
    int32_t cpu,
    protozero::ConstBytes comp_sched_bytes,
    protos::pbzero::FtraceEventBundle::CompactSched* message) const {
  protozero::Field switch_timestamp;
  protozero::Field switch_prev_state;
  protozero::Field switch_next_pid;
  protozero::Field switch_next_prio;
  protozero::Field switch_next_comm_index;

  // Pass through all non-switch fields (intern_table, waking_*, etc.)
  protozero::ProtoDecoder decoder(comp_sched_bytes);
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    switch (field.id()) {
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchTimestampFieldNumber:
        switch_timestamp = field;
        break;
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchPrevStateFieldNumber:
        switch_prev_state = field;
        break;
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchNextPidFieldNumber:
        switch_next_pid = field;
        break;
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchNextPrioFieldNumber:
        switch_next_prio = field;
        break;
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchNextCommIndexFieldNumber:
        switch_next_comm_index = field;
        break;
      default:
        proto_util::AppendField(field, message);
        break;
    }
  }

  std::array<bool, 5> has_switch_fields = {
      switch_timestamp.valid(),       switch_prev_state.valid(),
      switch_next_pid.valid(),        switch_next_prio.valid(),
      switch_next_comm_index.valid(),
  };

  // There are comp sched events that have no switch events, so we only
  // process switch events if there are any.
  if (std::any_of(has_switch_fields.begin(), has_switch_fields.end(), IsTrue)) {
    // If there are any switch events, expect every switch field to be present.
    if (PERFETTO_UNLIKELY(!std::all_of(has_switch_fields.begin(),
                                       has_switch_fields.end(), IsTrue))) {
      return base::ErrStatus(
          "MergeSyntheticSched: missing required "
          "FtraceEventBundle::CompactSched "
          "switch field.");
    }
    RETURN_IF_ERROR(OnCompSchedSwitch(
        context, cpu, switch_timestamp, switch_prev_state, switch_next_pid,
        switch_next_prio, switch_next_comm_index, message));
  }

  return base::OkStatus();
}

base::Status MergeSyntheticSched::OnCompSchedSwitch(
    const Context& context,
    int32_t cpu,
    const protozero::Field& switch_timestamp,
    const protozero::Field& switch_prev_state,
    const protozero::Field& switch_next_pid,
    const protozero::Field& switch_next_prio,
    const protozero::Field& switch_next_comm_index,
    protos::pbzero::FtraceEventBundle::CompactSched* message) const {
  PERFETTO_DCHECK(message);

  bool parse_errors = false;

  auto it_ts = ::protozero::PackedRepeatedFieldIterator<
      ::protozero::proto_utils::ProtoWireType::kVarInt, uint64_t>(
      switch_timestamp.data(), switch_timestamp.size(), &parse_errors);
  auto it_prev_state = ::protozero::PackedRepeatedFieldIterator<
      ::protozero::proto_utils::ProtoWireType::kVarInt, int64_t>(
      switch_prev_state.data(), switch_prev_state.size(), &parse_errors);
  auto it_pid = ::protozero::PackedRepeatedFieldIterator<
      ::protozero::proto_utils::ProtoWireType::kVarInt, int32_t>(
      switch_next_pid.data(), switch_next_pid.size(), &parse_errors);
  auto it_prio = ::protozero::PackedRepeatedFieldIterator<
      ::protozero::proto_utils::ProtoWireType::kVarInt, int32_t>(
      switch_next_prio.data(), switch_next_prio.size(), &parse_errors);
  auto it_comm = ::protozero::PackedRepeatedFieldIterator<
      ::protozero::proto_utils::ProtoWireType::kVarInt, uint32_t>(
      switch_next_comm_index.data(), switch_next_comm_index.size(),
      &parse_errors);

  if (PERFETTO_UNLIKELY(parse_errors)) {
    return base::ErrStatus(
        "MergeSyntheticSched: error reading "
        "FtraceEventBundle::CompactSched.");
  }

  struct SchedSwitchEvent {
    uint64_t timestamp_delta = 0;
    int64_t prev_state = 0;
    int32_t next_pid = 0;
    int32_t next_prio = 0;
    uint32_t next_comm_index = 0;
  };

  sched_switch_buffers_.packed_ts.Reset();
  sched_switch_buffers_.packed_prev_state.Reset();
  sched_switch_buffers_.packed_next_pid.Reset();
  sched_switch_buffers_.packed_next_prio.Reset();
  sched_switch_buffers_.packed_next_comm_index.Reset();

  auto emit_event = [&](const SchedSwitchEvent& evt) {
    sched_switch_buffers_.packed_ts.Append(evt.timestamp_delta);
    sched_switch_buffers_.packed_prev_state.Append(evt.prev_state);
    sched_switch_buffers_.packed_next_pid.Append(evt.next_pid);
    sched_switch_buffers_.packed_next_prio.Append(evt.next_prio);
    sched_switch_buffers_.packed_next_comm_index.Append(evt.next_comm_index);
  };

  PERFETTO_DCHECK(context.synthetic_process);
  int32_t synth_tid = 0;
  if (cpu >= 0 &&
      static_cast<size_t>(cpu) < context.synthetic_process->tids().size()) {
    synth_tid = context.synthetic_process->RunningOn(cpu);
  } else {
    return base::ErrStatus(
        "MergeSyntheticSched: cpu index out of bounds for synthetic process.");
  }

  uint64_t accumulated_delta = 0;
  bool in_synthetic_chain = false;
  SchedSwitchEvent curr{};

  uint64_t absolute_timestamp = 0;

  // Perform the consecutive synthetic thread switch events merging algorithm.
  // It will accumulate timestamp deltas of consecutive synthetic thread switch
  // events and merge them into the next non-synthetic thread switch event.
  //
  // Example:
  //
  // TID{N} <-- thread ID for target process
  // STID <-- synthetic thread ID for this CPU
  //
  // sched_switch events for a CPU.
  //
  // Before Merge:
  // TID1 (1 ns) -> TID2 (1 ns) -> STID (1 ns) -> STID (1 ns) -> STID (1 ns)
  // -> STID (1 ns) -> STID (1 ns) -> TID3 (1 ns) -> TID4 (1 ns) -> STID (1 ns)
  //
  // After Merge:
  // TID1 (1 ns) -> TID2 (1 ns) -> STID (1 ns) -> TID3 (5 ns) -> TID4 (1 ns) ->
  // STID (1 ns)
  std::optional<uint64_t> pending_boundary_ts = context.tracing_started_ts;
  while (it_ts && it_prev_state && it_pid && it_prio && it_comm) {
    curr.timestamp_delta = *it_ts;
    curr.prev_state = *it_prev_state;
    curr.next_pid = *it_pid;
    curr.next_prio = *it_prio;
    curr.next_comm_index = *it_comm;

    absolute_timestamp += curr.timestamp_delta;
    bool is_synthetic = curr.next_pid == synth_tid;

    // Trace Processor drops events before `tracing_started_ts` and the first
    // event after it (baseline). To prevent it from dropping the target app's
    // first event, we break synthetic chains at this boundary to provide a
    // sacrificial synthetic event for it to drop instead. We check this
    // top-level to bypass the check once the boundary is crossed.
    bool crossed_trace_started_boundary = false;
    if (PERFETTO_UNLIKELY(pending_boundary_ts.has_value())) {
      if (absolute_timestamp >= *pending_boundary_ts) {
        uint64_t prev_timestamp = absolute_timestamp - curr.timestamp_delta;
        if (prev_timestamp < *pending_boundary_ts) {
          crossed_trace_started_boundary = true;
        }
        pending_boundary_ts.reset();
      }
    }

    if (in_synthetic_chain) {
      bool continue_chain = is_synthetic && !crossed_trace_started_boundary;
      if (continue_chain) {
        // This is a consecutive synthetic switch event. Add its timestamp to
        // the accumulated delta.
        accumulated_delta += curr.timestamp_delta;
      } else {
        // The chain of synthetic switch events has been broken.
        // Add accumulated delta to the event that broke the chain.
        curr.timestamp_delta += accumulated_delta;

        in_synthetic_chain = false;
        accumulated_delta = 0;

        emit_event(curr);

        // If the chain was broken by a synthetic event crossing the
        // tracing_started boundary, we immediately start a new synthetic chain
        // with this event as the seed.
        if (is_synthetic) {
          in_synthetic_chain = true;
        }
      }
    } else {
      // Non-synthetic events and start of synthetic event chains are
      // emitted as-is since they only mark the start of the event, the
      // duration is encoded in the next event's delta time.
      if (is_synthetic) {
        // This is the first synthetic switch event in a potential chain
        in_synthetic_chain = true;
      }

      emit_event(curr);
    }

    ++it_ts;
    ++it_prev_state;
    ++it_pid;
    ++it_prio;
    ++it_comm;
  }

  // If the bundle ended while in a synthetic chain, emit a final event to
  // preserve the end timestamp of the unclosed chain. We use accumulated_delta
  // > 0 to avoid emitting zero-delta tails, which are harmless to drop.
  if (in_synthetic_chain && accumulated_delta > 0) {
    curr.timestamp_delta = accumulated_delta;
    emit_event(curr);
  }

  if (PERFETTO_UNLIKELY(it_ts || it_prev_state || it_pid || it_prio ||
                        it_comm)) {
    return base::ErrStatus(
        "MergeSyntheticSched: The number of switch events in"
        "FtraceEventBundle::CompactSched switch arrays are not the same.");
  }

  if (sched_switch_buffers_.packed_ts.size() > 0) {
    message->set_switch_timestamp(sched_switch_buffers_.packed_ts);
    message->set_switch_prev_state(sched_switch_buffers_.packed_prev_state);
    message->set_switch_next_pid(sched_switch_buffers_.packed_next_pid);
    message->set_switch_next_prio(sched_switch_buffers_.packed_next_prio);
    message->set_switch_next_comm_index(
        sched_switch_buffers_.packed_next_comm_index);
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
