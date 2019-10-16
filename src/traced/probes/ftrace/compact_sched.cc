/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/traced/probes/ftrace/compact_sched.h"

#include <stdint.h>

#include "perfetto/ext/base/optional.h"
#include "protos/perfetto/config/ftrace/ftrace_config.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "src/traced/probes/ftrace/event_info_constants.h"

namespace perfetto {

namespace {

// Pre-parse the format of sched_switch, checking if our simplifying
// assumptions about possible widths/signedness hold, and record the subset
// of the format that will be used during parsing.
base::Optional<CompactSchedSwitchFormat> ValidateSchedSwitchFormat(
    const Event& event) {
  using protos::pbzero::SchedSwitchFtraceEvent;

  CompactSchedSwitchFormat switch_format;
  switch_format.event_id = event.ftrace_event_id;

  // We make a compile-time buffer capacity decision based on the expected event
  // size per a set of pages. Check that the assumption holds.
  if (event.size < CompactSchedBundleState::kMinSupportedSchedSwitchSize) {
    return base::nullopt;
  }
  switch_format.size = event.size;

  bool prev_state_valid = false;
  bool next_pid_valid = false;
  bool next_prio_valid = false;
  bool next_comm_valid = false;
  for (const auto& field : event.fields) {
    switch (field.proto_field_id) {
      case SchedSwitchFtraceEvent::kPrevStateFieldNumber:
        switch_format.prev_state_offset = field.ftrace_offset;
        switch_format.prev_state_type = field.ftrace_type;

        // kernel type: long
        prev_state_valid = (field.ftrace_type == kFtraceInt32 ||
                            field.ftrace_type == kFtraceInt64);
        break;

      case SchedSwitchFtraceEvent::kNextPidFieldNumber:
        switch_format.next_pid_offset = field.ftrace_offset;
        switch_format.next_pid_type = field.ftrace_type;

        // kernel type: pid_t
        next_pid_valid = (field.ftrace_type == kFtracePid32);
        break;

      case SchedSwitchFtraceEvent::kNextPrioFieldNumber:
        switch_format.next_prio_offset = field.ftrace_offset;
        switch_format.next_prio_type = field.ftrace_type;

        // kernel type: int
        next_prio_valid = (field.ftrace_type == kFtraceInt32);
        break;

      case SchedSwitchFtraceEvent::kNextCommFieldNumber:
        switch_format.next_comm_offset = field.ftrace_offset;

        next_comm_valid =
            (field.ftrace_type == kFtraceFixedCString &&
             field.ftrace_size == CompactSchedBundleState::kExpectedCommLength);
        break;
      default:
        break;
    }
  }

  if (!prev_state_valid || !next_pid_valid || !next_prio_valid ||
      !next_comm_valid) {
    PERFETTO_ELOG("unexpected sched_switch format");
    return base::nullopt;
  }

  return base::make_optional(switch_format);
}

}  // namespace

// TODO(rsavitski): could avoid looping over all events if the caller did the
// work to remember the relevant events (translation table construction already
// loops over them).
CompactSchedEventFormat ValidateFormatForCompactSched(
    const std::vector<Event>& events) {
  using protos::pbzero::FtraceEvent;

  base::Optional<CompactSchedSwitchFormat> switch_format;
  for (const Event& event : events) {
    if (event.proto_field_id == FtraceEvent::kSchedSwitchFieldNumber) {
      switch_format = ValidateSchedSwitchFormat(event);
    }
  }

  if (switch_format.has_value()) {
    return CompactSchedEventFormat{/*format_valid=*/true,
                                   switch_format.value()};
  } else {
    return CompactSchedEventFormat{/*format_valid=*/false,
                                   CompactSchedSwitchFormat{}};
  }
}

CompactSchedEventFormat InvalidCompactSchedEventFormatForTesting() {
  return CompactSchedEventFormat{/*format_valid=*/false,
                                 CompactSchedSwitchFormat{}};
}

// TODO(rsavitski): find the correct place in the trace for, and method of,
// reporting rejection of compact_sched due to compile-time assumptions not
// holding at runtime.
// TODO(rsavitski): consider checking if the ftrace config correctly enables
// sched_switch, for at least an informative print for now?
CompactSchedConfig CreateCompactSchedConfig(
    const FtraceConfig& request,
    const CompactSchedEventFormat& compact_format) {
  if (!request.compact_sched().enabled())
    return CompactSchedConfig{/*enabled=*/false};

  if (!compact_format.format_valid)
    return CompactSchedConfig{/*enabled=*/false};

  return CompactSchedConfig{/*enabled=*/true};
}

CompactSchedConfig EnabledCompactSchedConfigForTesting() {
  return CompactSchedConfig{/*enabled=*/true};
}

CompactSchedConfig DisabledCompactSchedConfigForTesting() {
  return CompactSchedConfig{/*enabled=*/false};
}

// Sanity check size of stack-allocated bundle state.
static_assert(sizeof(CompactSchedBundleState) <= 1 << 20,
              "CompactSchedBundleState excessively large (used on the stack).");

void CompactSchedBundleState::WriteAndReset(
    protos::pbzero::FtraceEventBundle* bundle) {
  // If we buffered at least one event (using the interner as a proxy),
  // write the state out.
  if (interned_switch_comms_size_ > 0) {
    auto compact_out = bundle->set_compact_sched();

    compact_out->set_switch_timestamp(switch_timestamp_);
    compact_out->set_switch_next_pid(switch_next_pid_);
    compact_out->set_switch_prev_state(switch_prev_state_);
    compact_out->set_switch_next_prio(switch_next_prio_);

    for (size_t i = 0; i < interned_switch_comms_size_; i++) {
      compact_out->add_switch_next_comm_table(interned_switch_comms_[i].data(),
                                              interned_switch_comms_[i].size());
    }
    compact_out->set_switch_next_comm_index(switch_next_comm_index_);
  }

  // Reset internal state.
  last_switch_timestamp_ = 0;
  switch_timestamp_.Reset();
  switch_next_pid_.Reset();
  switch_prev_state_.Reset();
  switch_next_prio_.Reset();
  switch_next_comm_index_.Reset();
  intern_buf_write_pos_ = 0;
  interned_switch_comms_size_ = 0;
}

}  // namespace perfetto
