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

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

class SchedSwitchTransform {
 public:
  virtual ~SchedSwitchTransform();
  virtual base::Status Transform(const Context& context,
                                 uint64_t ts,
                                 int32_t cpu,
                                 int32_t* pid,
                                 std::string* comm) const = 0;
};

// Goes through all sched switch events are modifies them.
class RedactSchedSwitchHarness : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

  template <class Transform>
  void emplace_transform() {
    transforms_.emplace_back(new Transform());
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

  std::vector<std::unique_ptr<SchedSwitchTransform>> transforms_;
};

class ClearComms : public SchedSwitchTransform {
  base::Status Transform(const Context& context,
                         uint64_t ts,
                         int32_t cpu,
                         int32_t* pid,
                         std::string* comm) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_REDACT_SCHED_SWITCH_H_
