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

#include "src/trace_processor/shell/repl_subcommand.h"

#include <memory>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/interactive.h"
#include "src/trace_processor/shell/metatrace.h"

namespace perfetto::trace_processor::shell {

const char* ReplSubcommand::name() const {
  return "repl";
}

const char* ReplSubcommand::description() const {
  return "Interactive SQL shell.";
}

void ReplSubcommand::PrintUsage(const char* argv0) {
  PERFETTO_ELOG(R"(
Interactive SQL shell.

Usage: %s repl [flags] trace_file

Flags:
  -W, --wide             Double column width for output.
)",
                argv0);
}

static const option kReplLongOptions[] = {
    {"wide", no_argument, nullptr, 'W'},
    GLOBAL_LONG_OPTIONS{nullptr, 0, nullptr, 0}};

const option* ReplSubcommand::GetLongOptions() const {
  return kReplLongOptions;
}

int ReplSubcommand::Run(const SubcommandContext& ctx, int argc, char** argv) {
  GlobalOptions global;
  bool wide = false;

  optind = 1;
  for (;;) {
    int option = getopt_long(argc, argv, "Wm:h", kReplLongOptions, nullptr);
    if (option == -1)
      break;
    if (HandleGlobalOption(option, optarg, global))
      continue;
    if (option == 'W') {
      wide = true;
      continue;
    }
    PrintUsage(argv[0]);
    return option == 'h' ? 0 : 1;
  }

  if (optind == argc - 1 && argv[optind]) {
    global.trace_file = argv[optind];
  } else {
    PERFETTO_ELOG("repl: trace file is required");
    return 1;
  }

  auto config = BuildConfig(global, ctx.platform);
  auto tp_or = SetupTraceProcessor(global, config, ctx.platform);
  if (!tp_or.ok()) {
    PERFETTO_ELOG("%s", tp_or.status().c_message());
    return 1;
  }
  auto tp = std::move(*tp_or);

  auto t_load_or = LoadTraceFile(tp.get(), ctx.platform, global.trace_file);
  if (!t_load_or.ok()) {
    PERFETTO_ELOG("%s", t_load_or.status().c_message());
    return 1;
  }

  auto status = StartInteractiveShell(
      tp.get(),
      InteractiveOptions{
          wide ? 40u : 20u, MetricV1OutputFormat::kNone, {}, {}, nullptr});
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }

  status = MaybeWriteMetatrace(tp.get(), global.metatrace_path);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }
  return 0;
}

}  // namespace perfetto::trace_processor::shell
