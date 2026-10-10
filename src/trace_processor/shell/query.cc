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

#include "src/trace_processor/shell/query.h"

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/result_formatter.h"
#include "src/trace_processor/shell/shell_utils.h"

namespace perfetto::trace_processor {

base::Status RunQueriesWithoutOutput(TraceProcessor* trace_processor,
                                     const std::string& sql_query) {
  auto it = trace_processor->ExecuteQuery(sql_query);
  if (it.StatementWithOutputCount() > 0)
    return base::ErrStatus("Unexpected result from a query.");

  RETURN_IF_ERROR(it.Status());
  return it.Next() ? base::ErrStatus("Unexpected result from a query.")
                   : it.Status();
}

base::Status RunQueriesAndPrintResult(TraceProcessor* trace_processor,
                                      const std::string& sql_query,
                                      const ResultFormatOptions& format,
                                      FILE* output,
                                      bool quiet) {
  PERFETTO_DLOG("Executing query: %s", sql_query.c_str());

  // Statements are executed one at a time and every statement's result set
  // is printed, with consecutive result sets separated by a single blank
  // line. Since our CSV writer quotes all strings, a blank line is
  // unambiguously a boundary between result sets. Statements with no output
  // print nothing, matching the sqlite3/duckdb shells.
  std::chrono::nanoseconds exec_dur{0};
  uint32_t offset = 0;
  bool executed_any_statement = false;
  bool printed_any_result = false;
  for (;;) {
    auto query_start = std::chrono::steady_clock::now();
    std::optional<Iterator> it =
        trace_processor->ExecuteNextStatement(sql_query, &offset);
    if (!it.has_value()) {
      break;
    }
    RETURN_IF_ERROR(it->Status());
    executed_any_statement = true;

    bool has_more = it->Next();
    RETURN_IF_ERROR(it->Status());

    // Statements without a result set (e.g. CREATE TABLE) print nothing.
    if (it->ColumnCount() == 0) {
      PERFETTO_DCHECK(!has_more);
      exec_dur += std::chrono::steady_clock::now() - query_start;
      continue;
    }

    // Statements with rows which nonetheless count as having no output are
    // those whose output is explicitly ignored (a single column named
    // `suppress_query_output`, void functions): step through them for their
    // side effects but print nothing.
    if (has_more && it->StatementWithOutputCount() == 0) {
      for (; has_more; has_more = it->Next()) {
      }
      RETURN_IF_ERROR(it->Status());
      exec_dur += std::chrono::steady_clock::now() - query_start;
      continue;
    }

    // A zero-row result set still prints its header, with one exception: the
    // `suppress_query_output` escape hatch must stay silent whether or not
    // any row matched. (A zero-row void-function statement can't be detected
    // here: its VOID marker lives on a row's value and there is no row.)
    if (!has_more && it->ColumnCount() == 1 &&
        it->GetColumnName(0) == "suppress_query_output") {
      exec_dur += std::chrono::steady_clock::now() - query_start;
      continue;
    }

    std::vector<std::string> column_names;
    for (uint32_t c = 0; c < it->ColumnCount(); c++) {
      column_names.push_back(it->GetColumnName(c));
    }
    auto formatter =
        CreateResultFormatter(format, std::move(column_names), output);
    std::vector<SqlValue> cells(it->ColumnCount());
    for (; has_more; has_more = it->Next()) {
      for (uint32_t c = 0; c < it->ColumnCount(); c++) {
        cells[c] = it->Get(c);
      }
      // The formatter may have seen enough rows: abandon the rest.
      // TODO(lalitm): over --remote, abandoning the iterator does not stop
      // the statement: the server still computes every row and the client
      // drains them (an unbounded query never finishes). Let the caller tell
      // the RPC server to stop a stream (e.g. from RemoteIteratorImpl's
      // destructor or InterruptQuery()). This needs the server to yield
      // between batches rather than sending them all in one go, so that it
      // can see the request.
      if (!formatter->AddRow(cells.data())) {
        break;
      }
    }
    RETURN_IF_ERROR(it->Status());

    // We want to include the query iteration time (as it's a part of
    // executing SQL and can be non-trivial), and we want to exclude the time
    // spent printing the result (which can be significant for large results),
    // so formatters buffer rows until Finish().
    exec_dur += std::chrono::steady_clock::now() - query_start;

    if (printed_any_result) {
      fprintf(output, "\n");
    }
    printed_any_result = true;
    formatter->Finish();
  }
  if (!executed_any_statement) {
    return base::ErrStatus("No valid SQL to run");
  }

  if (!quiet) {
    int64_t ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(exec_dur).count();
    PrintStatusLine(StatusLabelStyle::kMuted,
                    "Query executed in " + std::to_string(ms) + " ms", "");
  }
  return base::OkStatus();
}

base::Status PrintPerfFile(const std::string& perf_file_path,
                           base::TimeNanos t_load,
                           base::TimeNanos t_run) {
  char buf[128];
  size_t count = base::SprintfTrunc(buf, sizeof(buf), "%" PRId64 ",%" PRId64,
                                    static_cast<int64_t>(t_load.count()),
                                    static_cast<int64_t>(t_run.count()));
  if (count == 0) {
    return base::ErrStatus("Failed to write perf data");
  }

  auto fd(base::OpenFile(perf_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666));
  if (!fd) {
    return base::ErrStatus("Failed to open perf file");
  }
  base::WriteAll(fd.get(), buf, count);
  return base::OkStatus();
}

base::Status RunQueries(TraceProcessor* trace_processor,
                        const std::string& queries,
                        bool expect_output,
                        bool quiet) {
  if (expect_output) {
    return RunQueriesAndPrintResult(trace_processor, queries,
                                    ResultFormatOptions(), stdout, quiet);
  }
  return RunQueriesWithoutOutput(trace_processor, queries);
}

base::Status RunQueriesFromFile(TraceProcessor* trace_processor,
                                const std::string& query_file_path,
                                bool expect_output,
                                bool quiet) {
  std::string queries;
  if (!base::ReadFile(query_file_path, &queries)) {
    return base::ErrStatus(
        "Unable to read file %s. If you're passing an SQL query, did you mean "
        "to use the -Q flag instead?",
        query_file_path.c_str());
  }
  return RunQueries(trace_processor, queries, expect_output, quiet);
}

}  // namespace perfetto::trace_processor
