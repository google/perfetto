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

#include "src/trace_processor/shell/query_subcommand.h"

#include <cstdio>
#include <memory>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/interactive.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/query.h"

namespace perfetto::trace_processor::shell {

const char* QuerySubcommand::name() const {
  return "query";
}

const char* QuerySubcommand::description() const {
  return "Run SQL queries against a trace.";
}

void QuerySubcommand::PrintUsage(const char* argv0) {
  PERFETTO_ELOG(R"(
Run SQL queries against a trace.

Usage: %s query [flags] trace_file

Flags:
  -f, --file FILE        Read and execute SQL from a file.
  -c, --sql STRING       Execute the given SQL string.
  -i, --interactive      Drop into REPL after query.
  -W, --wide             Double column width for output.
  -p, --perf-file FILE   Write timing data to FILE.
)",
                argv0);
}

int QuerySubcommand::Run(const SubcommandContext& ctx, int argc, char** argv) {
  GlobalOptions global;
  std::string query_file;
  std::string query_string;
  bool interactive = false;
  bool wide = false;
  std::string perf_file;

  static const option long_options[] = {
      {"file", required_argument, nullptr, 'f'},
      {"sql", required_argument, nullptr, 'c'},
      {"interactive", no_argument, nullptr, 'i'},
      {"wide", no_argument, nullptr, 'W'},
      {"perf-file", required_argument, nullptr, 'p'},
      GLOBAL_LONG_OPTIONS{nullptr, 0, nullptr, 0}};

  optind = 1;
  for (;;) {
    int option = getopt_long(argc, argv, "f:c:iWp:m:h", long_options, nullptr);
    if (option == -1)
      break;
    if (HandleGlobalOption(option, optarg, global))
      continue;

    if (option == 'f') {
      query_file = optarg;
      continue;
    }
    if (option == 'c') {
      query_string = optarg;
      continue;
    }
    if (option == 'i') {
      interactive = true;
      continue;
    }
    if (option == 'W') {
      wide = true;
      continue;
    }
    if (option == 'p') {
      perf_file = optarg;
      continue;
    }

    PrintUsage(argv[0]);
    return option == 'h' ? 0 : 1;
  }

  if (query_file.empty() && query_string.empty()) {
    PERFETTO_ELOG("query: must specify -f FILE or -c STRING");
    PrintUsage(argv[0]);
    return 1;
  }
  if (!perf_file.empty() && interactive) {
    PERFETTO_ELOG("query: -p and -i are mutually exclusive");
    return 1;
  }
  if (optind == argc - 1 && argv[optind]) {
    global.trace_file = argv[optind];
  } else {
    PERFETTO_ELOG("query: trace file is required");
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
  base::TimeNanos t_load = *t_load_or;

  base::TimeNanos t_query_start = base::GetWallTimeNs();

  if (!query_file.empty()) {
    auto status = RunQueriesFromFile(tp.get(), query_file, true);
    if (!status.ok()) {
      MaybeWriteMetatrace(tp.get(), global.metatrace_path);
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }
  if (!query_string.empty()) {
    auto status = RunQueries(tp.get(), query_string, true);
    if (!status.ok()) {
      MaybeWriteMetatrace(tp.get(), global.metatrace_path);
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }

  base::TimeNanos t_query = base::GetWallTimeNs() - t_query_start;

  if (interactive) {
    auto status = StartInteractiveShell(
        tp.get(),
        InteractiveOptions{
            wide ? 40u : 20u, MetricV1OutputFormat::kNone, {}, {}, nullptr});
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  } else if (!perf_file.empty()) {
    auto status = PrintPerfFile(perf_file, t_load, t_query);
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }

  auto status = MaybeWriteMetatrace(tp.get(), global.metatrace_path);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }

  return 0;
}

}  // namespace perfetto::trace_processor::shell
