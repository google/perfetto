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
  protos::pbzero::FtraceEventBundle::CompactSched::Decoder comp_sched(
      comp_sched_bytes);

  // Pass through all non-switch fields (intern_table, waking_*, etc.)
  protozero::ProtoDecoder decoder(comp_sched_bytes);
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    switch (field.id()) {
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchTimestampFieldNumber:
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchPrevStateFieldNumber:
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchNextPidFieldNumber:
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchNextPrioFieldNumber:
      case protos::pbzero::FtraceEventBundle::CompactSched::
          kSwitchNextCommIndexFieldNumber:
        break;
      default:
        proto_util::AppendField(field, message);
        break;
    }
  }

  std::array<bool, 5> has_switch_fields = {
      comp_sched.has_switch_timestamp(),
      comp_sched.has_switch_prev_state(),
      comp_sched.has_switch_next_pid(),
      comp_sched.has_switch_next_prio(),
      comp_sched.has_switch_next_comm_index(),
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
    RETURN_IF_ERROR(OnCompSchedSwitch(context, cpu, comp_sched, message));
  }

  return base::OkStatus();
}

base::Status MergeSyntheticSched::OnCompSchedSwitch(
    const Context& context,
    int32_t cpu,
    protos::pbzero::FtraceEventBundle::CompactSched::Decoder& comp_sched,
    protos::pbzero::FtraceEventBundle::CompactSched* message) const {
  PERFETTO_DCHECK(message);

  std::array<bool, 5> parse_errors = {false, false, false, false, false};

  auto it_ts = comp_sched.switch_timestamp(&parse_errors.at(0));
  auto it_prev_state = comp_sched.switch_prev_state(&parse_errors.at(1));
  auto it_pid = comp_sched.switch_next_pid(&parse_errors.at(2));
  auto it_prio = comp_sched.switch_next_prio(&parse_errors.at(3));
  auto it_comm = comp_sched.switch_next_comm_index(&parse_errors.at(4));

  if (PERFETTO_UNLIKELY(
          std::any_of(parse_errors.begin(), parse_errors.end(), IsTrue))) {
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

  protozero::PackedVarInt packed_ts;
  protozero::PackedVarInt packed_prev_state;
  protozero::PackedVarInt packed_next_pid;
  protozero::PackedVarInt packed_next_prio;
  protozero::PackedVarInt packed_next_comm_index;

  auto emit_event = [&](const SchedSwitchEvent& evt) {
    packed_ts.Append(evt.timestamp_delta);
    packed_prev_state.Append(evt.prev_state);
    packed_next_pid.Append(evt.next_pid);
    packed_next_prio.Append(evt.next_prio);
    packed_next_comm_index.Append(evt.next_comm_index);
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
  while (it_ts && it_prev_state && it_pid && it_prio && it_comm) {
    SchedSwitchEvent curr;
    curr.timestamp_delta = *it_ts;
    curr.prev_state = *it_prev_state;
    curr.next_pid = *it_pid;
    curr.next_prio = *it_prio;
    curr.next_comm_index = *it_comm;

    bool is_synthetic = curr.next_pid == synth_tid;

    if (in_synthetic_chain) {
      if (is_synthetic) {
        // This is a consecutive synthetic switch event. Add its timestamp to
        // the accumulated delta.
        accumulated_delta += curr.timestamp_delta;
      } else {
        // The chain of synthetic switch events has been broken.
        // Add accumulated delta to the event that broke the chain.
        curr.timestamp_delta += accumulated_delta;

        // By design, we remove information about why a synthetic thread
        // stopped running. This is because that information would be
        // misleading for coalesced events and make it zero also improves
        // compression and maintain consistency across all synthetic-sched
        // slices.
        curr.prev_state = 0;

        in_synthetic_chain = false;
        accumulated_delta = 0;

        emit_event(curr);
      }
    } else {
      // Non-synthetic events and start of synthetic event chains are
      // emitted as-is since they only mark the start of the event, the
      // duration is encoded in the next event's delta time.
      if (is_synthetic) {
        // This is the first synthetic switch event in a potential chain
        in_synthetic_chain = true;
        // Override the synthetic threads priority to 0 as its not useful
        // given we coalesce the switch events plus it makes the data more
        // compressible.
        curr.next_prio = 0;
      }

      emit_event(curr);
    }

    ++it_ts;
    ++it_prev_state;
    ++it_pid;
    ++it_prio;
    ++it_comm;
  }

  if (PERFETTO_UNLIKELY(it_ts || it_prev_state || it_pid || it_prio ||
                        it_comm)) {
    return base::ErrStatus(
        "MergeSyntheticSched: The number of switch events in"
        "FtraceEventBundle::CompactSched switch arrays are not the same.");
  }

  if (packed_ts.size() > 0) {
    message->set_switch_timestamp(packed_ts);
    message->set_switch_prev_state(packed_prev_state);
    message->set_switch_next_pid(packed_next_pid);
    message->set_switch_next_prio(packed_next_prio);
    message->set_switch_next_comm_index(packed_next_comm_index);
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
