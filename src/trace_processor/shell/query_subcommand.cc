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
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/trace_processor/summarizer.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/interactive.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/query.h"
#include "src/trace_processor/shell/subcommand.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <io.h>
#else
#include <unistd.h>
#endif
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) && !defined(STDIN_FILENO)
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#endif

namespace perfetto::trace_processor::shell {

const char* QuerySubcommand::name() const {
  return "query";
}

const char* QuerySubcommand::description() const {
  return "Load a trace and run a SQL query.";
}

const char* QuerySubcommand::usage_args() const {
  return "<trace_file> [SQL]";
}

const char* QuerySubcommand::detailed_help() const {
  return R"(Run one or more SQL queries against a loaded trace file and print results.

SQL can be provided in three ways:
  1. Positional argument:  tp query trace.pb "SELECT ts FROM slice LIMIT 10"
  2. From a file:          tp query -f queries.sql trace.pb
  3. From stdin:           cat q.sql | tp query trace.pb

Multiple semicolon-separated statements are supported. Use -i to drop into
an interactive shell after the queries complete.

Advanced (for debugging/testing structured queries):
  --structured-query-id ID --structured-query-spec FILE [...]
  Executes a single structured query by ID from the given spec files. The spec
  files replace -f/stdin/positional SQL. Output is the query result table.)";
}

std::vector<FlagSpec> QuerySubcommand::GetFlags() {
  return {
      StringFlag("query-file", 'f', "FILE",
                 "Read SQL from FILE (use '-' for stdin).", &query_file_),
      StringFlag("structured-query-id", '\0', "ID",
                 "[Advanced] Run a single structured query by ID.",
                 &structured_query_id_),
      {"structured-query-spec", '\0', true, "FILE",
       "[Advanced] Summary spec file for structured queries (repeatable).",
       [this](const char* v) { structured_query_specs_.emplace_back(v); }},
      BoolFlag("interactive", 'i', "Start interactive shell after query.",
               &interactive_),
      BoolFlag("wide", 'W', "Double column width for output.", &wide_),
      StringFlag("perf-file", '\0', "FILE", "Write perf timing data to FILE.",
                 &perf_file_),
  };
}

base::Status QuerySubcommand::Run(const SubcommandContext& ctx) {
  if (ctx.positional_args.empty()) {
    return base::ErrStatus("query: trace file is required");
  }
  std::string trace_file = ctx.positional_args[0];

  // Structured query mode: load specs via Summarizer and query by ID.
  if (!structured_query_id_.empty()) {
    if (structured_query_specs_.empty()) {
      return base::ErrStatus(
          "query: --structured-query-id requires at least one "
          "--structured-query-spec");
    }
    auto config = BuildConfig(*ctx.global, ctx.platform);
    ASSIGN_OR_RETURN(auto tp,
                     SetupTraceProcessor(*ctx.global, config, ctx.platform));
    ASSIGN_OR_RETURN(auto t_load,
                     LoadTraceFile(tp.get(), ctx.platform, trace_file));

    // Create a Summarizer and load specs.
    std::unique_ptr<Summarizer> summarizer;
    RETURN_IF_ERROR(tp->CreateSummarizer(&summarizer));

    for (const auto& path : structured_query_specs_) {
      std::string content;
      if (!base::ReadFile(path, &content)) {
        return base::ErrStatus("Unable to read spec file %s", path.c_str());
      }
      SummarizerUpdateSpecResult update_result;
      RETURN_IF_ERROR(summarizer->UpdateSpec(
          reinterpret_cast<const uint8_t*>(content.data()), content.size(),
          &update_result));
      for (const auto& q : update_result.queries) {
        if (q.error.has_value()) {
          return base::ErrStatus("Error in query '%s' from spec '%s': %s",
                                 q.query_id.c_str(), path.c_str(),
                                 q.error->c_str());
        }
      }
    }

    // Materialize and fetch the requested query.
    base::TimeNanos t_query_start = base::GetWallTimeNs();
    SummarizerQueryResult query_result;
    RETURN_IF_ERROR(
        summarizer->Query(structured_query_id_, &query_result));
    if (!query_result.exists) {
      return base::ErrStatus(
          "Structured query ID '%s' not found in the provided spec files",
          structured_query_id_.c_str());
    }

    // Run a SELECT * on the materialized table to print results.
    std::string sql = "SELECT * FROM " + query_result.table_name;
    RETURN_IF_ERROR(RunQueries(tp.get(), sql, true));
    base::TimeNanos t_query = base::GetWallTimeNs() - t_query_start;

    if (!perf_file_.empty())
      RETURN_IF_ERROR(PrintPerfFile(perf_file_, t_load, t_query));

    RETURN_IF_ERROR(MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path));
    return base::OkStatus();
  }

  // Regular SQL query mode.

  // Determine SQL source:
  //   1. Positional:  query trace.pb "SELECT ..."
  //   2. File:        query -f file.sql trace.pb
  //   3. Stdin flag:  query -f - trace.pb
  //   4. Stdin pipe:  query trace.pb < file.sql
  std::string sql;
  bool read_stdin =
      query_file_ == "-" || (query_file_.empty() && !isatty(STDIN_FILENO));
  if (ctx.positional_args.size() >= 2) {
    sql = ctx.positional_args[1];
  } else if (read_stdin) {
    if (!base::ReadFileDescriptor(STDIN_FILENO, &sql))
      return base::ErrStatus("query: failed to read SQL from stdin");
    query_file_.clear();
  }

  if (sql.empty() && query_file_.empty()) {
    return base::ErrStatus(
        "query: no SQL provided. Use positional arg, -f FILE, or pipe to "
        "stdin.");
  }

  auto config = BuildConfig(*ctx.global, ctx.platform);
  ASSIGN_OR_RETURN(auto tp,
                   SetupTraceProcessor(*ctx.global, config, ctx.platform));
  ASSIGN_OR_RETURN(auto t_load,
                   LoadTraceFile(tp.get(), ctx.platform, trace_file));

  // If we have a file, read it into sql. After this point, sql always has
  // the SQL to execute.
  if (!query_file_.empty()) {
    if (!base::ReadFile(query_file_, &sql)) {
      return base::ErrStatus("query: unable to read file '%s'",
                             query_file_.c_str());
    }
  }
  PERFETTO_CHECK(!sql.empty());

  base::TimeNanos t_query_start = base::GetWallTimeNs();
  auto status = RunQueries(tp.get(), sql, true);
  if (!status.ok()) {
    MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path);
    return status;
  }

  base::TimeNanos t_query = base::GetWallTimeNs() - t_query_start;

  if (!perf_file_.empty())
    RETURN_IF_ERROR(PrintPerfFile(perf_file_, t_load, t_query));

  if (interactive_) {
    RETURN_IF_ERROR(StartInteractiveShell(
        tp.get(),
        InteractiveOptions{
            wide_ ? 40u : 20u, MetricV1OutputFormat::kNone, {}, {}, nullptr}));
  }

  RETURN_IF_ERROR(MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
