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
#include "src/trace_redaction/build_timeline.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/optimize_timeline.h"
#include "src/trace_redaction/populate_allow_lists.h"
#include "src/trace_redaction/prune_package_list.h"
#include "src/trace_redaction/redact_sched_switch.h"
#include "src/trace_redaction/redact_sched_waking.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/scrub_process_trees.h"
#include "src/trace_redaction/scrub_task_rename.h"
#include "src/trace_redaction/scrub_trace_packet.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"

namespace perfetto::trace_redaction {

// Builds and runs a trace redactor.
static base::Status Main(std::string_view input,
                         std::string_view output,
                         std::string_view package_name) {
  TraceRedactor redactor;

  // Add all collectors.
  redactor.collectors()->emplace_back(new FindPackageUid());
  redactor.collectors()->emplace_back(new BuildTimeline());

  // Add all builders.
  redactor.builders()->emplace_back(new PopulateAllowlists());
  redactor.builders()->emplace_back(new OptimizeTimeline());

  // Add all transforms.
  redactor.transformers()->emplace_back(new PrunePackageList());
  redactor.transformers()->emplace_back(new ScrubTracePacket());
  redactor.transformers()->emplace_back(new ScrubFtraceEvents());
  redactor.transformers()->emplace_back(new ScrubProcessTrees());
  redactor.transformers()->emplace_back(new ScrubTaskRename());
  redactor.transformers()->emplace_back(new RedactSchedSwitch());
  redactor.transformers()->emplace_back(new RedactSchedWaking());

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
