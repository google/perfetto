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
#include "src/trace_redaction/collect_timeline_events.h"
#include "src/trace_redaction/filter_ftrace_using_allowlist.h"
#include "src/trace_redaction/filter_packet_using_allowlist.h"
#include "src/trace_redaction/filter_print_events.h"
#include "src/trace_redaction/filter_sched_waking_events.h"
#include "src/trace_redaction/filter_task_rename.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/optimize_timeline.h"
#include "src/trace_redaction/populate_allow_lists.h"
#include "src/trace_redaction/prune_package_list.h"
#include "src/trace_redaction/redact_ftrace_event.h"
#include "src/trace_redaction/redact_process_free.h"
#include "src/trace_redaction/redact_sched_switch.h"
#include "src/trace_redaction/redact_task_newtask.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/scrub_process_stats.h"
#include "src/trace_redaction/scrub_process_trees.h"
#include "src/trace_redaction/scrub_trace_packet.h"
#include "src/trace_redaction/suspend_resume.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"

namespace perfetto::trace_redaction {

// Builds and runs a trace redactor.
static base::Status Main(std::string_view input,
                         std::string_view output,
                         std::string_view package_name) {
  TraceRedactor redactor;

  // Add all collectors.
  redactor.emplace_collect<FindPackageUid>();
  redactor.emplace_collect<CollectTimelineEvents>();

  // Add all builders.
  redactor.emplace_build<PopulateAllowlists>();
  redactor.emplace_build<AllowSuspendResume>();
  redactor.emplace_build<OptimizeTimeline>();

  // Add all transforms.
  auto* scrub_packet = redactor.emplace_transform<ScrubTracePacket>();
  scrub_packet->emplace_back<FilterPacketUsingAllowlist>();

  auto* scrub_ftrace_events = redactor.emplace_transform<ScrubFtraceEvents>();
  scrub_ftrace_events->emplace_back<FilterFtraceUsingAllowlist>();
  scrub_ftrace_events->emplace_back<FilterPrintEvents>();
  scrub_ftrace_events->emplace_back<FilterSchedWakingEvents>();
  scrub_ftrace_events->emplace_back<FilterTaskRename>();
  scrub_ftrace_events->emplace_back<FilterSuspendResume>();

  // Scrub packets and ftrace events first as they will remove the largest
  // chucks of data from the trace. This will reduce the amount of data that the
  // other primitives need to operate on.
  redactor.emplace_transform<ScrubProcessTrees>();
  redactor.emplace_transform<PrunePackageList>();
  redactor.emplace_transform<ScrubProcessStats>();

  auto* redact_ftrace_events = redactor.emplace_transform<RedactFtraceEvent>();
  redact_ftrace_events->emplace_back<RedactSchedSwitch>();
  redact_ftrace_events->emplace_back<RedactTaskNewTask>();
  redact_ftrace_events->emplace_back<RedactProcessFree>();

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
