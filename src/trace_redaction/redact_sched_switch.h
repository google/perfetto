/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_REDACT_SCHED_SWITCH_H_
#define SRC_TRACE_REDACTION_REDACT_SCHED_SWITCH_H_

#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"

namespace perfetto::trace_redaction {

class InternTable {
 public:
  int64_t Push(const char* data, size_t size);

  std::string_view Find(size_t index) const;

  const std::vector<std::string_view>& values() const {
    return interned_comms_;
  }

 private:
  constexpr static size_t kExpectedCommLength = 16;
  constexpr static size_t kMaxElements = 4096;

  std::array<char, kMaxElements * kExpectedCommLength> comms_;
  size_t comms_length_ = 0;

  std::vector<std::string_view> interned_comms_;
};

// TODO(vaage): Rename this class. When it was first created, it only handled
// switch events, so having "switch" in the name sense. Now that it is
// expanding to include waking events, a more general name is needed (e.g.
// scheduling covers both switch and waking events).
class RedactSchedSwitchHarness : public TransformPrimitive {
 public:
  class Modifier {
   public:
    virtual ~Modifier();
    virtual base::Status Modify(const Context& context,
                                uint64_t ts,
                                int32_t cpu,
                                int32_t* pid,
                                std::string* comm) const = 0;
  };

  base::Status Transform(const Context& context,
                         std::string* packet) const override;

  template <class Transform>
  void emplace_transform() {
    modifier_ = std::make_unique<Transform>();
  }

 private:
  base::Status TransformFtraceEvents(
      const Context& context,
      protozero::Field ftrace_events,
      protos::pbzero::FtraceEventBundle* message) const;

  base::Status TransformFtraceEvent(const Context& context,
                                    int32_t cpu,
                                    protozero::Field ftrace_event,
                                    protos::pbzero::FtraceEvent* message) const;

  // scratch_str is a reusable string, allowing comm modifications to be done in
  // a shared buffer, avoiding allocations when processing ftrace events.
  base::Status TransformFtraceEventSchedSwitch(
      const Context& context,
      uint64_t ts,
      int32_t cpu,
      protos::pbzero::SchedSwitchFtraceEvent::Decoder& sched_switch,
      std::string* scratch_str,
      protos::pbzero::SchedSwitchFtraceEvent* message) const;

  base::Status TransformFtraceEventSchedWaking(
      const Context& context,
      uint64_t ts,
      int32_t cpu,
      protos::pbzero::SchedWakingFtraceEvent::Decoder& sched_waking,
      std::string* scratch_str,
      protos::pbzero::SchedWakingFtraceEvent* message) const;

  base::Status TransformCompSched(
      const Context& context,
      int32_t cpu,
      protos::pbzero::FtraceEventBundle::CompactSched::Decoder& comp_sched,
      protos::pbzero::FtraceEventBundle::CompactSched* message) const;

  base::Status TransformCompSchedSwitch(
      const Context& context,
      int32_t cpu,
      protos::pbzero::FtraceEventBundle::CompactSched::Decoder& comp_sched,
      InternTable* intern_table,
      protos::pbzero::FtraceEventBundle::CompactSched* message) const;

  std::unique_ptr<Modifier> modifier_;
};

class ClearComms : public RedactSchedSwitchHarness::Modifier {
  base::Status Modify(const Context& context,
                      uint64_t ts,
                      int32_t cpu,
                      int32_t* pid,
                      std::string* comm) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_REDACT_SCHED_SWITCH_H_
