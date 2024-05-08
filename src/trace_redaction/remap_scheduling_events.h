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

#ifndef SRC_TRACE_REDACTION_REMAP_SCHEDULING_EVENTS_H_
#define SRC_TRACE_REDACTION_REMAP_SCHEDULING_EVENTS_H_

#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_redaction/redact_ftrace_event.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"

namespace perfetto::trace_redaction {

// Reads the Ftrace event's pid and replaces it with a synthetic thread id (if
// necessary).
class ThreadMergeRemapFtraceEventPid : public FtraceEventRedaction {
 public:
  static constexpr auto kFieldId = protos::pbzero::FtraceEvent::kPidFieldNumber;

  base::Status Redact(
      const Context& context,
      const protos::pbzero::FtraceEventBundle::Decoder& bundle,
      protozero::ProtoDecoder& event,
      protos::pbzero::FtraceEvent* event_message) const override;
};

// Reads the sched switch pid and replaces it with a synthetic thread id (if
// necessary).
//
//  event {
//    timestamp: 6702093743539938
//    pid: 0
//    sched_switch {
//      prev_comm: "swapper/7"
//      prev_pid: 0
//      prev_prio: 120
//      prev_state: 0
//      next_comm: "FMOD stream thr"
//      next_pid: 7174
//      next_prio: 104
//    }
//  }
class ThreadMergeRemapSchedSwitchPid : public FtraceEventRedaction {
 public:
  static constexpr auto kFieldId =
      protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber;

  base::Status Redact(
      const Context& context,
      const protos::pbzero::FtraceEventBundle::Decoder& bundle,
      protozero::ProtoDecoder& event,
      protos::pbzero::FtraceEvent* event_message) const override;
};

// Reads the sched waking pid and replaces it with a synthetic thread id (if
// necessary).
//
//  event {
//    timestamp: 6702093743527386
//    pid: 0
//    sched_waking {
//      comm: "FMOD stream thr"
//      pid: 7174
//      prio: 104
//      success: 1
//      target_cpu: 7
//    }
//  }
class ThreadMergeRemapSchedWakingPid : public FtraceEventRedaction {
 public:
  static constexpr auto kFieldId =
      protos::pbzero::FtraceEvent::kSchedWakingFieldNumber;

  base::Status Redact(
      const Context& context,
      const protos::pbzero::FtraceEventBundle::Decoder& bundle,
      protozero::ProtoDecoder& event,
      protos::pbzero::FtraceEvent* event_message) const override;
};

// Drop "new task" events because it's safe to assume that the threads always
// exist.
//
//  event {
//    timestamp: 6702094133317685
//    pid: 6167
//    task_newtask {
//      pid: 7972                 <-- Pid being started
//      comm: "adbd"
//      clone_flags: 4001536
//      oom_score_adj: -1000
//    }
//  }
//
// Drop "process free" events because it's safe to assume that the threads
// always exist.
//
//  event {
//    timestamp: 6702094703942898
//    pid: 10
//    sched_process_free {
//      comm: "shell svc 7973"
//      pid: 7974                 <-- Pid being freed
//      prio: 120
//    }
//  }
class ThreadMergeDropField : public FtraceEventRedaction {
 public:
  static constexpr auto kTaskNewtaskFieldNumber =
      protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber;
  static constexpr auto kSchedProcessFreeFieldNumber =
      protos::pbzero::FtraceEvent::kSchedProcessFreeFieldNumber;

  base::Status Redact(
      const Context& context,
      const protos::pbzero::FtraceEventBundle::Decoder& bundle,
      protozero::ProtoDecoder& event,
      protos::pbzero::FtraceEvent* event_message) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_REMAP_SCHEDULING_EVENTS_H_
