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

#ifndef SRC_TRACE_REDACTION_FILTER_FTRACE_USING_ALLOWLIST_H_
#define SRC_TRACE_REDACTION_FILTER_FTRACE_USING_ALLOWLIST_H_

#include "perfetto/protozero/field.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// event {
//   timestamp: 6702094168934980
//   pid: 7127
//   sched_waking {               <-- event type
//     comm: "Job.worker 1"
//     pid: 7143
//     prio: 120
//     success: 1
//     target_cpu: 7
//   }
// }
//
// Check if the event type appears in the ftrace allow-list. If it doesn't
// appear there, then mark the event as "don't keep".
class FilterFtraceUsingAllowlist : public FtraceEventFilter {
 public:
  base::Status VerifyContext(const Context& context) const override;
  bool KeepEvent(const Context& context,
                 protozero::ConstBytes bytes) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_FILTER_FTRACE_USING_ALLOWLIST_H_
