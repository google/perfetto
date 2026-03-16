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

#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/interactive.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/query.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace perfetto::trace_processor::shell {

const char* QuerySubcommand::name() const {
  return "query";
}

const char* QuerySubcommand::description() const {
  return "Run SQL queries against a trace.";
}

std::vector<FlagSpec> QuerySubcommand::GetFlags() {
  return {
      StringFlag("file", 'f', "FILE",
                 "Read and execute SQL from a file. Use '-' for stdin.",
                 &query_file_),
      BoolFlag("interactive", 'i', "Drop into REPL after query.",
               &interactive_),
      BoolFlag("wide", 'W', "Double column width for output.", &wide_),
      StringFlag("perf-file", 'p', "FILE", "Write timing data to FILE.",
                 &perf_file_),
  };
}

base::Status QuerySubcommand::Run(const SubcommandContext& ctx) {
  // First positional arg is the trace file.
  if (ctx.positional_args.empty()) {
    return base::ErrStatus("query: trace file is required");
  }
  ctx.global->trace_file = ctx.positional_args[0];

  // SQL source priority:
  //   1. Positional inline SQL:  query trace.pb "SELECT ..."
  //   2. File flag:              query -f file.sql trace.pb
  //   3. Explicit stdin:         query -f - trace.pb
  //   4. Auto-detect stdin pipe: query trace.pb < file.sql
  std::string sql;

  if (ctx.positional_args.size() >= 2) {
    // Inline SQL string after the trace file.
    sql = ctx.positional_args[1];
  } else if (query_file_ == "-") {
    // -f - : read SQL from stdin.
    if (!base::ReadFileDescriptor(STDIN_FILENO, &sql)) {
      return base::ErrStatus("query: failed to read SQL from stdin");
    }
    query_file_.clear();
  } else if (!query_file_.empty()) {
    // -f FILE: handled below via RunQueriesFromFile.
  } else if (!isatty(STDIN_FILENO)) {
    // Stdin is a pipe — read SQL from it.
    if (!base::ReadFileDescriptor(STDIN_FILENO, &sql) || sql.empty()) {
      return base::ErrStatus("query: no SQL provided on stdin");
    }
  } else {
    return base::ErrStatus(
        "query: must specify SQL via positional argument, "
        "-f FILE, or stdin pipe");
  }

  if (!perf_file_.empty() && interactive_) {
    return base::ErrStatus("query: -p and -i are mutually exclusive");
  }

  auto config = BuildConfig(*ctx.global, ctx.platform);
  ASSIGN_OR_RETURN(auto tp,
                   SetupTraceProcessor(*ctx.global, config, ctx.platform));
  ASSIGN_OR_RETURN(auto t_load, LoadTraceFile(tp.get(), ctx.platform,
                                              ctx.global->trace_file));

  base::TimeNanos t_query_start = base::GetWallTimeNs();

  if (!query_file_.empty()) {
    auto status = RunQueriesFromFile(tp.get(), query_file_, true);
    if (!status.ok()) {
      MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path);
      return status;
    }
  }
  if (!sql.empty()) {
    auto status = RunQueries(tp.get(), sql, true);
    if (!status.ok()) {
      MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path);
      return status;
    }
  }

  base::TimeNanos t_query = base::GetWallTimeNs() - t_query_start;

  if (interactive_) {
    RETURN_IF_ERROR(StartInteractiveShell(
        tp.get(),
        InteractiveOptions{
            wide_ ? 40u : 20u, MetricV1OutputFormat::kNone, {}, {}, nullptr}));
  } else if (!perf_file_.empty()) {
    RETURN_IF_ERROR(PrintPerfFile(perf_file_, t_load, t_query));
  }

  RETURN_IF_ERROR(MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
