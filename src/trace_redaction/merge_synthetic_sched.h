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

#ifndef SRC_TRACE_REDACTION_MERGE_SYNTHETIC_SCHED_H_
#define SRC_TRACE_REDACTION_MERGE_SYNTHETIC_SCHED_H_

#include <cstdint>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto::trace_redaction {

// Validates that the context contains a populated synthetic process with
// synthetic threads.
class SyntheticProcessValidator : public ValidatorPrimitive {
 public:
  base::Status Validate(const Context& context) const override;
};

// Coalesces consecutive duplicate synthetic sched switch events in CompactSched
// messages.
//
// Requires `SyntheticProcessValidator` to be registered in the same pass to
// ensure `Context::synthetic_process` is populated before this transform runs.
class MergeSyntheticSched : public TransformPrimitive {
 public:
  base::Status Transform(const Context& context,
                         std::string* packet) const override;

 private:
  base::Status OnFtraceEvents(const Context& context,
                              protozero::Field ftrace_events,
                              protos::pbzero::FtraceEventBundle* message) const;

  base::Status OnCompSched(
      const Context& context,
      int32_t cpu,
      protozero::ConstBytes comp_sched_bytes,
      protos::pbzero::FtraceEventBundle::CompactSched* message) const;

  base::Status OnCompSchedSwitch(
      const Context& context,
      int32_t cpu,
      protos::pbzero::FtraceEventBundle::CompactSched::Decoder& comp_sched,
      protos::pbzero::FtraceEventBundle::CompactSched* message) const;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_MERGE_SYNTHETIC_SCHED_H_
