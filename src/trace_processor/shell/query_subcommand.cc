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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/summarizer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/protozero/text_to_proto/text_to_proto.h"
#include "src/trace_processor/shell/agent_mode.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/interactive.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/query.h"
#include "src/trace_processor/shell/result_formatter.h"
#include "src/trace_processor/shell/shell_utils.h"
#include "src/trace_processor/shell/subcommand.h"
#include "src/trace_processor/trace_summary/trace_summary.descriptor.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <unistd.h>  // For STDIN_FILENO.
#endif
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) && !defined(STDIN_FILENO)
#define STDIN_FILENO 0
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FREEBSD) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#define PERFETTO_HAS_SIGNAL_H() 1
#include <signal.h>
#else
#define PERFETTO_HAS_SIGNAL_H() 0
#endif

namespace perfetto::trace_processor::shell {
namespace {

// Returns true if the file at |path| with |content| should be treated as
// textproto rather than binary proto. Uses the same heuristic as the
// classic codepath: .pb → binary, .textproto → text, otherwise
// content-sniff the first 128 bytes (all printable/whitespace → text).
bool IsTextproto(const std::string& path, const std::string& content) {
  if (base::EndsWith(path, ".pb")) {
    return false;
  }
  if (base::EndsWith(path, ".textproto")) {
    return true;
  }
  std::string_view prefix(content.c_str(),
                          std::min<size_t>(content.size(), 128));
  return std::all_of(prefix.begin(), prefix.end(),
                     [](char c) { return std::isspace(c) || std::isprint(c); });
}

// Reads a TraceSummarySpec file and passes it to |summarizer|, converting
// textproto to binary proto when necessary.
base::Status LoadSpecIntoSummarizer(Summarizer* summarizer,
                                    const std::string& path) {
  std::string content;
  if (!base::ReadFile(path, &content)) {
    return base::ErrStatus("Unable to read spec file %s", path.c_str());
  }

  const uint8_t* spec_data;
  size_t spec_size;
  std::vector<uint8_t> binary_proto;
  if (IsTextproto(path, content)) {
    ASSIGN_OR_RETURN(binary_proto,
                     protozero::TextToProto(kTraceSummaryDescriptor.data(),
                                            kTraceSummaryDescriptor.size(),
                                            ".perfetto.protos.TraceSummarySpec",
                                            "-", std::string_view(content)));
    spec_data = binary_proto.data();
    spec_size = binary_proto.size();
  } else {
    spec_data = reinterpret_cast<const uint8_t*>(content.data());
    spec_size = content.size();
  }

  SummarizerUpdateSpecResult update_result;
  RETURN_IF_ERROR(summarizer->UpdateSpec(spec_data, spec_size, &update_result));
  for (const auto& q : update_result.queries) {
    if (q.error.has_value()) {
      return base::ErrStatus("Error in query '%s' from spec '%s': %s",
                             q.query_id.c_str(), path.c_str(),
                             q.error->c_str());
    }
  }
  return base::OkStatus();
}

// Runs |sql|, printing every result set. On error, records which statement
// failed so that it can be reported (see kFailedStatementPayload).
base::Status RunAndPrint(TraceProcessor* tp,
                         const std::string& sql,
                         const ResultFormatOptions& format,
                         const GlobalOptions& global) {
  uint32_t statement = 0;
  base::Status status = RunQueriesAndPrintResult(tp, sql, format, stdout,
                                                 global.quiet, &statement);
  if (!status.ok() && statement > 0) {
    status.SetPayload(kFailedStatementPayload, std::to_string(statement));
  }
  return status;
}

}  // namespace

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

Multiple semicolon-separated statements are supported: every statement's
result set is printed, with consecutive result sets separated by a single
blank line. Use -i to drop into an interactive shell after the queries
complete.

Output is CSV by default. --format markdown prints compact markdown tables:
results over --max-rows rows or --max-bytes bytes show only their first and
last rows plus a footer with the row count, and cells are
cut at --max-cell-width characters.

Agent mode is on when run by a coding agent (detected from its environment,
e.g. CLAUDECODE or AI_AGENT) with stdout not a terminal and no --format;
--agent / --no-agent force it on or off. In agent mode, results default to
markdown capped at 1000 rows / 10KB with cells cut at 500 characters, and
errors are printed as JSON.

Advanced (for debugging/testing structured queries):
  --structured-query-id ID --summary-spec FILE [...]
  Executes a single structured query by ID from the given summary spec
  files. The spec files replace -f/stdin/positional SQL. Output is the
  query result table.)";
}

std::vector<FlagSpec> QuerySubcommand::GetFlags() {
  return {
      StringFlag("query-file", 'f', "FILE",
                 "Read SQL from FILE (use '-' for stdin).", &query_file_),
      StringFlag("structured-query-id", '\0', "ID",
                 "[Advanced] Run a single structured query by ID.",
                 &structured_query_id_),
      {"summary-spec", '\0', true, "FILE",
       "[Advanced] Summary spec file for structured queries (repeatable).",
       [this](const char* v) { structured_query_specs_.emplace_back(v); }},
      BoolFlag("interactive", 'i', "Start interactive shell after query.",
               &interactive_),
      BoolFlag("wide", 'W', "Double column width for output.", &wide_),
      StringFlag("perf-file", '\0', "FILE", "Write perf timing data to FILE.",
                 &perf_file_),
      StringFlag("format", '\0', "FMT",
                 "Output format: csv (default) or markdown.", &format_),
      BoolFlag("agent", '\0', "Force agent mode on (see above).", &agent_),
      BoolFlag("no-agent", '\0', "Force agent mode off.", &no_agent_),
      StringFlag("max-rows", '\0', "N",
                 "[markdown] Rows printed in full; larger results show only "
                 "their first and last rows. 0 for no limit.",
                 &max_rows_),
      StringFlag("max-bytes", '\0', "N",
                 "[markdown] Like --max-rows but for bytes of output.",
                 &max_bytes_),
      StringFlag("max-cell-width", '\0', "N",
                 "[markdown] Characters printed per cell. 0 for no limit.",
                 &max_cell_width_),
  };
}

base::StatusOr<ResultFormatOptions> QuerySubcommand::ResolveFormat(
    const AgentMode& agent_mode) {
  ResultFormatOptions options =
      agent_mode.enabled ? AgentResultFormatOptions() : ResultFormatOptions();
  if (format_ == "csv") {
    options.format = ResultFormat::kCsv;
  } else if (format_ == "markdown") {
    options.format = ResultFormat::kMarkdown;
  } else if (!format_.empty()) {
    return base::ErrStatus("query: unknown --format '%s' (csv or markdown)",
                           format_.c_str());
  }
  struct {
    const char* flag;
    const std::string& value;
    uint64_t* target;
  } limits[] = {
      {"--max-rows", max_rows_, &options.max_rows},
      {"--max-bytes", max_bytes_, &options.max_bytes},
      {"--max-cell-width", max_cell_width_, &options.max_cell_chars},
  };
  for (const auto& limit : limits) {
    if (limit.value.empty()) {
      continue;
    }
    std::optional<uint64_t> v = base::StringToUInt64(limit.value);
    if (!v) {
      return base::ErrStatus("query: %s expects a number, got '%s'", limit.flag,
                             limit.value.c_str());
    }
    *limit.target = *v;
  }
  return options;
}

base::Status QuerySubcommand::Run(const SubcommandContext& ctx) {
  RETURN_IF_ERROR(RejectExtraPositionals(ctx, "query", 2));
  if (agent_ && no_agent_) {
    return base::ErrStatus("query: --agent and --no-agent are exclusive");
  }
  std::optional<bool> force_agent;
  if (agent_ || no_agent_) {
    force_agent = agent_;
  }
  const bool stdout_is_tty = base::IsTty(stdout);
  AgentMode agent_mode =
      DecideAgentMode(force_agent, !format_.empty(), stdout_is_tty);
  const bool quiet = ctx.global->quiet;
  if (agent_mode.enabled && !quiet) {
    PrintAgentModeNotice(agent_mode);
  }

  base::Status status = ExecuteAndPrint(ctx, agent_mode);
  if (status.ok()) {
    return status;
  }
  // Results printed before the error (e.g. by earlier statements) must come
  // first when stdout and stderr go to the same place.
  fflush(stdout);
  if (agent_mode.enabled) {
    fprintf(stderr, "%s\n", FormatErrorAsJson(status).c_str());
  } else if (!force_agent && format_.empty() && !stdout_is_tty && !quiet) {
    // Agents which don't set a known environment variable never get agent
    // mode automatically. Their stdout is redirected, so when a query fails
    // in that situation, point them at it.
    fprintf(stderr, "%s\n", status.c_message());
    PrintAgentModeHint();
  } else {
    return status;
  }
  status.SetPayload("perfetto.dev/has_printed_error", "1");
  return status;
}

base::Status QuerySubcommand::ExecuteAndPrint(const SubcommandContext& ctx,
                                              const AgentMode& agent_mode) {
  ASSIGN_OR_RETURN(ResultFormatOptions format, ResolveFormat(agent_mode));
  // With --remote, the trace is already loaded server-side, so there is no
  // trace-file positional: the first positional (if any) is the SQL.
  std::string trace_file;
  size_t sql_pos = 0;
  RETURN_IF_ERROR(ResolveTraceFileArg(ctx, "query", &trace_file, &sql_pos));

  // Advanced: structured query mode.
  if (!structured_query_id_.empty()) {
    return RunStructuredQuery(ctx, trace_file, format);
  }

  // Determine SQL source:
  //   1. Positional:  query trace.pb "SELECT ..."
  //   2. File:        query -f file.sql trace.pb
  //   3. Stdin flag:  query -f - trace.pb
  //   4. Stdin pipe:  query trace.pb < file.sql
  std::string sql;
  bool read_stdin =
      query_file_ == "-" || (query_file_.empty() && !base::IsTty(STDIN_FILENO));
  if (ctx.positional_args.size() > sql_pos) {
    sql = ctx.positional_args[sql_pos];
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

  base::TimeNanos t_load{};
  ASSIGN_OR_RETURN(auto tp, CreateTraceProcessor(*ctx.global, ctx.platform,
                                                 trace_file, &t_load));
  // One-shot `query TRACE` calls re-parse the trace every time. Scripts and
  // coding agents rarely read --help but do read stderr, so when parsing
  // was slow enough to matter tell them, once per call, how to keep the
  // trace loaded across queries. Measured on agents: this line alone moved
  // warm-session use from 0% to >80% of runs; longer guides were not read.
  constexpr double kSlowParseSeconds = 1.0;
  double load_s = static_cast<double>(t_load.count()) / 1e9;
  if (!ctx.global->quiet && ctx.global->remote_addr.empty() && !interactive_ &&
      load_s >= kSlowParseSeconds) {
    PrintStatusLine(
        StatusLabelStyle::kHint, "Tip",
        base::StackString<64>("parsing took %.1fs", load_s).ToStdString() +
            " and `query TRACE` re-parses on every call. To run many "
            "queries, load once and reuse the session:\n"
            "  trace_processor server unix --name S --daemonize " +
            trace_file +
            "\n"
            "  trace_processor query --remote S \"SELECT ...\"\n"
            "  trace_processor server kill S\n"
            "Scripts and AI agents: `trace_processor help agent` covers "
            "this, schema discovery and PerfettoSQL tips.");
  }

  if (!query_file_.empty()) {
    if (!base::ReadFile(query_file_, &sql)) {
      return base::ErrStatus("query: unable to read file '%s'",
                             query_file_.c_str());
    }
  }
  PERFETTO_CHECK(!sql.empty());

#if PERFETTO_HAS_SIGNAL_H()
  static TraceProcessor* g_tp_for_signal_handler = tp.get();
  signal(SIGINT, [](int) { g_tp_for_signal_handler->InterruptQuery(); });
#endif

  base::TimeNanos t_query_start = base::GetWallTimeNs();
  auto status = RunAndPrint(tp.get(), sql, format, *ctx.global);
  if (!status.ok()) {
    MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path);
    return status;
  }
  base::TimeNanos t_query = base::GetWallTimeNs() - t_query_start;

  if (!perf_file_.empty()) {
    RETURN_IF_ERROR(PrintPerfFile(perf_file_, t_load, t_query));
  }

  if (interactive_) {
    RETURN_IF_ERROR(StartInteractiveShell(
        tp.get(), InteractiveOptions{wide_ ? 40u : 20u,
                                     MetricV1OutputFormat::kNone,
                                     {},
                                     {},
                                     nullptr,
                                     ctx.global->quiet}));
  }

  RETURN_IF_ERROR(MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path));
  return base::OkStatus();
}

base::Status QuerySubcommand::RunStructuredQuery(
    const SubcommandContext& ctx,
    const std::string& trace_file,
    const ResultFormatOptions& format) {
  if (structured_query_specs_.empty()) {
    return base::ErrStatus(
        "query: --structured-query-id requires at least one --summary-spec");
  }

  base::TimeNanos t_load{};
  ASSIGN_OR_RETURN(auto tp, CreateTraceProcessor(*ctx.global, ctx.platform,
                                                 trace_file, &t_load));

  std::unique_ptr<Summarizer> summarizer;
  RETURN_IF_ERROR(tp->CreateSummarizer(&summarizer));
  for (const auto& path : structured_query_specs_) {
    RETURN_IF_ERROR(LoadSpecIntoSummarizer(summarizer.get(), path));
  }

  base::TimeNanos t_query_start = base::GetWallTimeNs();
  SummarizerQueryResult query_result;
  RETURN_IF_ERROR(summarizer->Query(structured_query_id_, &query_result));
  if (!query_result.exists) {
    return base::ErrStatus(
        "Structured query ID '%s' not found in the provided spec files",
        structured_query_id_.c_str());
  }

  RETURN_IF_ERROR(RunAndPrint(tp.get(),
                              "SELECT * FROM " + query_result.table_name,
                              format, *ctx.global));
  base::TimeNanos t_query = base::GetWallTimeNs() - t_query_start;

  if (!perf_file_.empty()) {
    RETURN_IF_ERROR(PrintPerfFile(perf_file_, t_load, t_query));
  }
  RETURN_IF_ERROR(MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
