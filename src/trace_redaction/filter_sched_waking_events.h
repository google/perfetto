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

#ifndef SRC_TRACE_REDACTION_FILTER_SCHED_WAKING_EVENTS_H_
#define SRC_TRACE_REDACTION_FILTER_SCHED_WAKING_EVENTS_H_

#include "perfetto/protozero/field.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

// Redact sched waking trace events in a ftrace event bundle:
//
//  event {
//    timestamp: 6702093787823849
//    pid: 814                      <-- waker
//    sched_waking {
//      comm: "surfaceflinger"
//      pid: 756                    <-- target
//      prio: 97
//      success: 1
//      target_cpu: 2
//    }
//  }
//
// The three values needed are:
//
//  1. event.pid
//  2. event.timestamp
//  3. event.sched_waking.pid
//
// The two checks that are executed are:
//
//  1. package(event.pid).at(event.timestamp).is(target)
//  2. package(event.sched_waking.pid).at(event.timestamp).is(target)
//
// Both must be true in order to keep an event.
class FilterSchedWakingEvents : public FtraceEventFilter {
 public:
  base::Status VerifyContext(const Context& context) const override;
  bool KeepEvent(const Context& context,
                 protozero::ConstBytes bytes) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_FILTER_SCHED_WAKING_EVENTS_H_
