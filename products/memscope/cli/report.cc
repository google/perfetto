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

#include "products/memscope/cli/report.h"

#include <stdio.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/read_trace.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "products/memscope/sql/amalgamated_memscope_sql.h"
#include "src/trace_processor/util/sql_bundle.h"

namespace perfetto::memscope {
namespace {

using trace_processor::SqlValue;
using trace_processor::TraceProcessor;

constexpr char kPackage[] = "memscope";

struct Category {
  const char* key;
  const char* label;
};
// Same keys, labels and order as the UI's SMAPS_CATEGORIES.
constexpr Category kCategories[] = {
    {"native", "Native"},       {"java", "Java"},
    {"file", "File-backed"},    {"graphics", "Graphics"},
    {"stack", "Thread stacks"}, {"other", "Other"},
};

base::Status RegisterMemscopePackage(TraceProcessor* tp) {
  trace_processor::SqlPackage pkg;
  pkg.name = kPackage;
  trace_processor::SqlBundle bundle(
      trace_processor::memscope::kAmalgamatedMemscopeSql.data(),
      trace_processor::memscope::kAmalgamatedMemscopeSql.size());
  for (const auto& f : bundle) {
    std::string name = f.path;
    PERFETTO_CHECK(base::EndsWith(name, ".sql"));
    name = name.substr(0, name.size() - 4);
    pkg.modules.emplace_back(
        std::string(kPackage) + "." + base::ReplaceAll(name, "/", "."),
        std::string(f.sql));
  }
  return tp->RegisterSqlPackage(std::move(pkg));
}

// A query result cell, detached from the iterator.
struct Cell {
  std::optional<double> num;
  std::string str;
  int64_t i64() const { return num ? static_cast<int64_t>(*num) : 0; }
};
using Rows = std::vector<std::vector<Cell>>;

// Runs |sql| and returns all rows; logs and returns nullopt on error.
std::optional<Rows> Query(TraceProcessor* tp, const std::string& sql) {
  auto it = tp->ExecuteQuery(sql);
  Rows rows;
  while (it.Next()) {
    std::vector<Cell> row;
    for (uint32_t i = 0; i < it.ColumnCount(); ++i) {
      SqlValue v = it.Get(i);
      Cell c;
      if (v.type == SqlValue::kLong)
        c.num = static_cast<double>(v.long_value);
      else if (v.type == SqlValue::kDouble)
        c.num = v.double_value;
      else if (v.type == SqlValue::kString)
        c.str = v.AsString();
      row.push_back(std::move(c));
    }
    rows.push_back(std::move(row));
  }
  if (!it.Status().ok()) {
    PERFETTO_ELOG("Query failed: %s", it.Status().c_message());
    return std::nullopt;
  }
  return rows;
}

// Matches the UI's formatBytes: MiB with one decimal.
std::string Bytes(double b) {
  return base::StackString<64>("%.1f MiB", b / (1024.0 * 1024.0)).ToStdString();
}

// Matches the UI's formatDelta.
std::string Delta(double b) {
  if (std::fpclassify(b) == FP_ZERO)
    return "±" + Bytes(0);
  return (b > 0 ? "+" : "-") + Bytes(std::fabs(b));
}

// Matches the UI's formatDurationShort.
std::string Duration(double ns) {
  double secs = ns / 1e9;
  if (secs < 60)
    return base::StackString<32>("%.0fs", secs).ToStdString();
  double mins = secs / 60;
  if (mins < 60)
    return base::StackString<32>(
               "%dm %ds", static_cast<int>(mins),
               static_cast<int>(std::round(std::fmod(secs, 60))))
        .ToStdString();
  return base::StackString<32>(
             "%dh %dm", static_cast<int>(mins / 60),
             static_cast<int>(std::round(std::fmod(mins, 60))))
      .ToStdString();
}

// Prints "  <label><padding> <value>", padding by code points, not bytes.
void Row(const std::string& label, const std::string& value) {
  size_t width = 0;
  for (char c : label)
    width += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  printf("  %s%*s %s\n", label.c_str(),
         static_cast<int>(24 - std::min<size_t>(width, 24)), "", value.c_str());
}

std::string Opt(const Cell& c, std::string (*fmt)(double)) {
  return c.num ? fmt(*c.num) : "-";
}

}  // namespace

int PrintReport(const ReportArgs& args) {
  std::unique_ptr<TraceProcessor> tp =
      TraceProcessor::CreateInstance(trace_processor::Config());
  base::Status status = RegisterMemscopePackage(tp.get());
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }
  status = trace_processor::ReadTrace(tp.get(), args.trace_path.c_str());
  if (!status.ok()) {
    PERFETTO_ELOG("Could not load %s: %s", args.trace_path.c_str(),
                  status.c_message());
    return 1;
  }

  std::string filter = "upid = memscope_best_process()";
  if (args.pid) {
    filter = "pid = " + std::to_string(*args.pid);
  } else if (!args.app.empty()) {
    filter = "process_name = '" + base::ReplaceAll(args.app, "'", "''") + "'";
  }
  auto procs = Query(tp.get(),
                     "INCLUDE PERFETTO MODULE memscope.processes;"
                     "SELECT upid, pid, process_name, heap_dumps, "
                     "smaps_snapshots, native_dumps "
                     "FROM memscope_process_mem_stats WHERE " +
                         filter + " ORDER BY score DESC, upid LIMIT 1");
  if (!procs)
    return 1;
  if (procs->empty()) {
    PERFETTO_ELOG("No process with memory data matches in %s",
                  args.trace_path.c_str());
    return 1;
  }
  const auto& p = (*procs)[0];
  std::string upid = std::to_string(p[0].i64());
  printf("%s (pid %" PRId64 ")\n", p[2].str.c_str(), p[1].i64());
  printf("  %" PRId64 " Java heap dumps, %" PRId64 " smaps snapshots, %" PRId64
         " native heap dumps\n\n",
         p[3].i64(), p[4].i64(), p[5].i64());

  auto summary = Query(
      tp.get(),
      "INCLUDE PERFETTO MODULE memscope.summary;"
      "SELECT d.uptime, d.oom_score_adj, c.rss_spike_bytes, t.peak_bytes, "
      "  t.delta_bytes, t.trend_bytes_per_hour "
      "FROM (SELECT " +
          upid +
          " AS upid) u "
          "LEFT JOIN memscope_process_last_heap_dump d USING (upid) "
          "LEFT JOIN memscope_process_rss_counters c USING (upid) "
          "LEFT JOIN memscope_process_memory_trend t USING (upid)");
  if (!summary)
    return 1;
  const auto& s = (*summary)[0];
  const std::optional<double>& oom = s[1].num;
  Row("Uptime", Opt(s[0], Duration));
  Row("OOM score", oom ? std::to_string(static_cast<int64_t>(*oom)) : "-");
  Row("Peak RSS (anon + swap)", Opt(s[3], Bytes));
  Row("RSS spike", Opt(s[2], Delta));
  Row("Memory Δ (first → last)", Opt(s[4], Delta));
  const std::optional<double>& trend = s[5].num;
  Row("Trend", trend ? Delta(*trend) + "/h" : "-");

  auto cats = Query(tp.get(),
                    "INCLUDE PERFETTO MODULE memscope.smaps;"
                    "WITH snaps AS (SELECT ts, row_number() OVER "
                    "  (ORDER BY ts) AS n FROM memscope_smaps_snapshot "
                    "  WHERE upid = " +
                        upid +
                        ") "
                        "SELECT n, category, rss_swap_bytes "
                        "FROM memscope_smaps_category_breakdown b JOIN snaps "
                        "USING (ts) WHERE upid = " +
                        upid +
                        " "
                        "AND n IN (1, (SELECT max(n) FROM snaps))");
  if (!cats)
    return 1;
  int64_t last = 0;
  for (const auto& r : *cats)
    last = std::max(last, r[0].i64());
  if (last > 0) {
    printf("\n  Breakdown at snapshot #%" PRId64 " (rss + swap)%s\n", last,
           last > 1 ? "        Δ vs #1" : "");
    for (const Category& c : kCategories) {
      double first_b = 0, last_b = 0;
      bool present = false;
      for (const auto& r : *cats) {
        if (r[1].str != c.key)
          continue;
        present = true;
        if (r[0].i64() == 1)
          first_b = *r[2].num;
        if (r[0].i64() == last)
          last_b = *r[2].num;
      }
      if (!present)
        continue;
      printf("    %-15s %12s", c.label, Bytes(last_b).c_str());
      if (last > 1)
        printf("  %14s", Delta(last_b - first_b).c_str());
      printf("\n");
    }
  }

  auto growth = Query(tp.get(),
                      "INCLUDE PERFETTO MODULE memscope.java;"
                      "SELECT type_name, delta_size_bytes, delta_obj_count "
                      "FROM memscope_java_class_growth WHERE upid = " +
                          upid +
                          " AND delta_size_bytes > 0 "
                          "ORDER BY delta_size_bytes DESC LIMIT 5");
  if (!growth)
    return 1;
  if (!growth->empty()) {
    printf("\n  Top Java growth (first → last heap dump)\n");
    for (const auto& r : *growth) {
      printf("    %-40s %12s  %+" PRId64 " objects\n", r[0].str.c_str(),
             Delta(*r[1].num).c_str(), r[2].i64());
    }
  }
  return 0;
}

}  // namespace perfetto::memscope
