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

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_redaction/collect_frame_cookies.h"
#include "src/trace_redaction/collect_system_info.h"
#include "src/trace_redaction/collect_timeline_events.h"
#include "src/trace_redaction/filter_ftrace_using_allowlist.h"
#include "src/trace_redaction/filter_packet_using_allowlist.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/populate_allow_lists.h"
#include "src/trace_redaction/prune_package_list.h"
#include "src/trace_redaction/redact_ftrace_event.h"
#include "src/trace_redaction/redact_process_events.h"
#include "src/trace_redaction/redact_process_trees.h"
#include "src/trace_redaction/redact_sched_events.h"
#include "src/trace_redaction/remap_scheduling_events.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/scrub_process_stats.h"
#include "src/trace_redaction/scrub_trace_packet.h"
#include "src/trace_redaction/suspend_resume.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"
#include "src/trace_redaction/verify_integrity.h"

namespace perfetto::trace_redaction {

// Builds and runs a trace redactor.
static base::Status Main(std::string_view input,
                         std::string_view output,
                         std::string_view package_name) {
  TraceRedactor redactor;

  // VerifyIntegrity breaks the CollectPrimitive pattern. Instead of writing to
  // the context, its job is to read trace packets and return errors if any
  // packet does not look "correct". This primitive is added first in an effort
  // to detect and react to bad input before other collectors run.
  redactor.emplace_collect<VerifyIntegrity>();

  // Add all collectors.
  redactor.emplace_collect<FindPackageUid>();
  redactor.emplace_collect<CollectTimelineEvents>();
  redactor.emplace_collect<CollectFrameCookies>();
  redactor.emplace_collect<CollectSystemInfo>();

  // Add all builders.
  redactor.emplace_build<PopulateAllowlists>();
  redactor.emplace_build<AllowSuspendResume>();
  redactor.emplace_build<ReduceFrameCookies>();
  redactor.emplace_build<BuildSyntheticThreads>();

  // Add all transforms.
  auto* scrub_packet = redactor.emplace_transform<ScrubTracePacket>();
  scrub_packet->emplace_back<FilterPacketUsingAllowlist>();
  scrub_packet->emplace_back<FilterFrameEvents>();

  auto* scrub_ftrace_events = redactor.emplace_transform<ScrubFtraceEvents>();
  scrub_ftrace_events->emplace_back<FilterFtraceUsingAllowlist>();
  scrub_ftrace_events->emplace_back<FilterSuspendResume>();

  // Scrub packets and ftrace events first as they will remove the largest
  // chucks of data from the trace. This will reduce the amount of data that the
  // other primitives need to operate on.
  redactor.emplace_transform<PrunePackageList>();
  redactor.emplace_transform<ScrubProcessStats>();

  // Redacts all switch and waking events. This should use the same modifier and
  // filter as the process events (see below).
  auto* redact_sched_events = redactor.emplace_transform<RedactSchedEvents>();
  redact_sched_events->emplace_modifier<ClearComms>();
  redact_sched_events->emplace_filter<ConnectedToPackage>();

  // Redacts all new task, rename task, process free events. This should use the
  // same modifier and filter as the schedule events (see above).
  auto* redact_process_events =
      redactor.emplace_transform<RedactProcessEvents>();
  redact_process_events->emplace_modifier<ClearComms>();
  redact_process_events->emplace_filter<ConnectedToPackage>();

  // TODO(vaage): The primitives used to implement thread merging do not work
  // correctly with other primitives.
  //
  //    - ThreadMergeRemapFtraceEventPid
  //    - ThreadMergeRemapSchedSwitchPid
  //    - ThreadMergeRemapSchedWakingPid
  //    - ThreadMergeDropField(kTaskNewtaskFieldNumber)
  //    - ThreadMergeDropField(kSchedProcessFreeFieldNumber)
  //
  // Add these primitives back one-by-one to find the issue.

  // Configure the primitive to remove processes and threads that don't belong
  // to the target package and adds a process and threads for the synth thread
  // group and threads.
  {
    auto* primitive = redactor.emplace_transform<RedactProcessTrees>();
    primitive->emplace_modifier<ProcessTreeCreateSynthThreads>();
    primitive->emplace_filter<ProcessTreeFilterConnectedToPackage>();
  }

  Context context;
  context.package_name = package_name;

  return redactor.Redact(input, output, &context);
}

}  // namespace perfetto::trace_redaction

int main(int argc, char** argv) {
  constexpr int kSuccess = 0;
  constexpr int kFailure = 1;
  constexpr int kInvalidArgs = 2;

  if (argc != 4) {
    PERFETTO_ELOG(
        "Invalid arguments: %s <input file> <output file> <package name>",
        argv[0]);
    return kInvalidArgs;
  }

  auto result = perfetto::trace_redaction::Main(argv[1], argv[2], argv[3]);

  if (result.ok()) {
    return kSuccess;
  }

  PERFETTO_ELOG("Unexpected error: %s", result.c_message());
  return kFailure;
}
