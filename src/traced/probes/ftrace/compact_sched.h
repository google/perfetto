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

#ifndef SRC_TRACED_PROBES_FTRACE_COMPACT_SCHED_H_
#define SRC_TRACED_PROBES_FTRACE_COMPACT_SCHED_H_

#include <stdint.h>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "src/traced/probes/ftrace/event_info_constants.h"

namespace perfetto {

class FtraceConfig;

// The subset of the sched_switch event's format that is used when parsing &
// encoding into the compact format.
struct CompactSchedSwitchFormat {
  uint32_t event_id;
  uint16_t size;

  uint16_t next_pid_offset;
  FtraceFieldType next_pid_type;
  uint16_t next_prio_offset;
  FtraceFieldType next_prio_type;
  uint16_t prev_state_offset;
  FtraceFieldType prev_state_type;
  uint16_t next_comm_offset;
};

// Pre-parsed format of a subset of scheduling events, for use during ftrace
// parsing if compact encoding is enabled. Holds a flag, |format_valid| to
// state whether the compile-time assumptions about the format held at runtime.
// If they didn't, we cannot use the compact encoding.
struct CompactSchedEventFormat {
  // If false, the rest of the struct is considered invalid.
  const bool format_valid;
  const CompactSchedSwitchFormat sched_switch;
};

CompactSchedEventFormat ValidateFormatForCompactSched(
    const std::vector<Event>& events);

CompactSchedEventFormat InvalidCompactSchedEventFormatForTesting();

// Compact encoding configuration used at ftrace reading & parsing time.
struct CompactSchedConfig {
  CompactSchedConfig(bool _enabled) : enabled(_enabled) {}

  // If true, and sched_switch event is enabled, encode it in a compact format
  // instead of the normal form.
  const bool enabled = false;
};

CompactSchedConfig CreateCompactSchedConfig(
    const FtraceConfig& request,
    const CompactSchedEventFormat& compact_format);

CompactSchedConfig EnabledCompactSchedConfigForTesting();
CompactSchedConfig DisabledCompactSchedConfigForTesting();

// Mutable state for buffering parts of scheduling events, that can later be
// written out in a compact format with |WriteAndReset|. Used by the ftrace
// reader, allocated on the stack.
class CompactSchedBundleState {
 public:
  // Most of the state is stack-allocated, with a compile-time
  // size. We work in batches of pages (see kParsingBufferSizePages in
  // ftrace_controller.cc), and assume a minimum size of a sched event as
  // written by the kernel (validated at runtime). We therefore can calculate
  // the maximum necessary capacity for a given parsing buffer size (as
  // statically asserted in ftrace_controller.cc).
  // Note: be careful not to align the individual buffers at a multiple of the
  // cache size.
  // TODO(rsavitski): this will need a slight rework once we add sched_waking,
  // as it'll be the min size of the two events.
  static constexpr size_t kMaxElements = 2560;
  static constexpr size_t kMinSupportedSchedSwitchSize = 56;
  static constexpr size_t kExpectedCommLength = 16;

  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>*
  switch_timestamp() {
    return &switch_timestamp_;
  }

  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>*
  switch_prev_state() {
    return &switch_prev_state_;
  }

  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>*
  switch_next_pid() {
    return &switch_next_pid_;
  }

  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>*
  switch_next_prio() {
    return &switch_next_prio_;
  }

  size_t interned_switch_comms_size() const {
    return interned_switch_comms_size_;
  }

  inline void AppendSwitchTimestamp(uint64_t timestamp) {
    switch_timestamp_.Append(timestamp - last_switch_timestamp_);
    last_switch_timestamp_ = timestamp;
  }

  // TODO(rsavitski): see if we can use the fact that comms are <16 bytes
  // long when comparing them.
  void InternSwitchNextComm(const char* ptr) {
    // Linearly scan existing string views, ftrace reader will
    // make sure this set doesn't grow too large.
    base::StringView transient_view(ptr);
    for (size_t i = 0; i < interned_switch_comms_size_; i++) {
      if (transient_view == interned_switch_comms_[i]) {
        switch_next_comm_index_.Append(i);
        return;
      }
    }

    // Unique next_comm, intern it. Null byte is not copied over.
    char* start = intern_buf_ + intern_buf_write_pos_;
    size_t size = transient_view.size();
    memcpy(start, ptr, size);
    intern_buf_write_pos_ += size;

    switch_next_comm_index_.Append(interned_switch_comms_size_);
    base::StringView safe_view(start, size);
    interned_switch_comms_[interned_switch_comms_size_++] = safe_view;

    PERFETTO_DCHECK(intern_buf_write_pos_ <= sizeof(intern_buf_));
  }

  // Writes out the currently buffered events, and starts the next batch
  // internally.
  void WriteAndReset(protos::pbzero::FtraceEventBundle* bundle);

 private:
  // First timestamp in a bundle is absolute. The rest are all delta-encoded,
  // each relative to the preceding sched_switch timestamp.
  uint64_t last_switch_timestamp_ = 0;

  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_timestamp_;
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_prev_state_;
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_next_pid_;
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_next_prio_;

  // Storage for interned strings (without null bytes).
  char intern_buf_[kMaxElements * (kExpectedCommLength - 1)];
  size_t intern_buf_write_pos_ = 0;

  // Views into unique interned next_comm strings. Even if every sched_switch
  // carries a unique next_comm, the ftrace reader is expected to flush the
  // compact buffer way before this reaches capacity. This is since the cost of
  // processing each event grows with every unique interned next_comm (as the
  // interning needs to search all existing internings).
  std::array<base::StringView, kMaxElements> interned_switch_comms_;
  uint32_t interned_switch_comms_size_ = 0;

  // One entry per sched_switch event, contains the index of the interned
  // next_comm string view (i.e. array index into |interned_switch_comms|).
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_next_comm_index_;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_COMPACT_SCHED_H_
