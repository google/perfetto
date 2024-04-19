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

#ifndef SRC_TRACE_REDACTION_SUSPEND_RESUME_H_
#define SRC_TRACE_REDACTION_SUSPEND_RESUME_H_

#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Updates allowlists to include suspend-resume events and which events to allow
// through.
class AllowSuspendResume : public BuildPrimitive {
 public:
  base::Status Build(Context* context) const override;
};

// Filters ftrace events based on the suspend-resume event.
class FilterSuspendResume : public FtraceEventFilter {
 public:
  base::Status VerifyContext(const Context& context) const override;

  bool KeepEvent(const Context& context,
                 protozero::ConstBytes bytes) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_SUSPEND_RESUME_H_
