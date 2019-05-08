/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/metrics/metrics.h"

#include <regex>
#include <unordered_map>
#include <vector>

#include "perfetto/base/string_utils.h"
#include "perfetto/metrics/android/mem_metric.pbzero.h"
#include "perfetto/metrics/metrics.pbzero.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/metrics/sql_metrics.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

namespace {
// TODO(lalitm): delete this and use sqlite_utils when that is cleaned up of
// trace processor dependencies.
const char* ExtractSqliteValue(sqlite3_value* value) {
  auto type = sqlite3_value_type(value);
  PERFETTO_DCHECK(type == SQLITE_TEXT);
  return reinterpret_cast<const char*>(sqlite3_value_text(value));
}
}  // namespace

int TemplateReplace(
    const std::string& raw_text,
    const std::unordered_map<std::string, std::string>& substitutions,
    std::string* out) {
  std::regex re(R"(\{\{\s*(\w*)\s*\}\})", std::regex_constants::ECMAScript);

  auto it = std::sregex_iterator(raw_text.begin(), raw_text.end(), re);
  auto regex_end = std::sregex_iterator();
  auto start = raw_text.begin();
  for (; it != regex_end; ++it) {
    out->insert(out->end(), start, raw_text.begin() + it->position(0));

    auto value_it = substitutions.find(it->str(1));
    if (value_it == substitutions.end())
      return 1;

    const auto& value = value_it->second;
    std::copy(value.begin(), value.end(), std::back_inserter(*out));
    start = raw_text.begin() + it->position(0) + it->length(0);
  }
  out->insert(out->end(), start, raw_text.end());
  return 0;
}

void RunMetric(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  auto* tp = static_cast<TraceProcessor*>(sqlite3_user_data(ctx));
  if (argc == 0 || sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
    sqlite3_result_error(ctx, "RUN_METRIC: Invalid arguments", -1);
    return;
  }

  const char* filename =
      reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
  const char* sql = sql_metrics::GetBundledMetric(filename);
  if (!sql) {
    sqlite3_result_error(ctx, "RUN_METRIC: Unknown filename provided", -1);
    return;
  }

  std::unordered_map<std::string, std::string> substitutions;
  for (int i = 1; i < argc; i += 2) {
    if (sqlite3_value_type(argv[i]) != SQLITE_TEXT) {
      sqlite3_result_error(ctx, "RUN_METRIC: Invalid args", -1);
      return;
    }

    auto* key_str = ExtractSqliteValue(argv[i]);
    auto* value_str = ExtractSqliteValue(argv[i + 1]);
    substitutions[key_str] = value_str;
  }

  for (const auto& query : base::SplitString(sql, ";\n")) {
    std::string buffer;
    int ret = TemplateReplace(query, substitutions, &buffer);
    if (ret) {
      sqlite3_result_error(
          ctx, "RUN_METRIC: Error when performing substitution", -1);
      return;
    }

    PERFETTO_DLOG("RUN_METRIC: Executing query: %s", buffer.c_str());
    auto it = tp->ExecuteQuery(buffer);
    if (auto opt_error = it.GetLastError()) {
      char* error =
          sqlite3_mprintf("RUN_METRIC: Error when running file %s: %s",
                          filename, opt_error->c_str());
      sqlite3_result_error(ctx, error, -1);
      sqlite3_free(error);
      return;
    } else if (it.Next()) {
      sqlite3_result_error(
          ctx, "RUN_METRIC: functions should not produce any output", -1);
      return;
    }
  }
}

int ComputeMetrics(TraceProcessor* tp,
                   const std::vector<std::string>& metric_names,
                   std::vector<uint8_t>* metrics_proto) {
  // TODO(lalitm): stop hardcoding android.mem metric and read the proto
  // descriptor for this logic instead.
  if (metric_names.size() != 1 || metric_names[0] != "android.mem") {
    PERFETTO_ELOG("Only android.mem metric is currently supported");
    return 1;
  }

  auto queries = base::SplitString(sql_metrics::kAndroidMem, ";\n");
  for (const auto& query : queries) {
    PERFETTO_DLOG("Executing query: %s", query.c_str());
    auto prep_it = tp->ExecuteQuery(query);
    prep_it.Next();

    if (auto opt_error = prep_it.GetLastError()) {
      PERFETTO_ELOG("SQLite error: %s", opt_error->c_str());
      return 1;
    }
  }

  protozero::ScatteredHeapBuffer delegate;
  protozero::ScatteredStreamWriter writer(&delegate);
  delegate.set_writer(&writer);

  protos::pbzero::TraceMetrics metrics;
  metrics.Reset(&writer);

  // TODO(lalitm): all the below is temporary hardcoded queries and proto
  // filling to ensure that the code above works.
  auto it = tp->ExecuteQuery("SELECT COUNT(*) from lmk_by_score;");
  auto has_next = it.Next();
  if (auto opt_error = it.GetLastError()) {
    PERFETTO_ELOG("SQLite error: %s", opt_error->c_str());
    return 1;
  }
  PERFETTO_CHECK(has_next);
  PERFETTO_CHECK(it.Get(0).type == SqlValue::Type::kLong);

  has_next = it.Next();
  PERFETTO_DCHECK(!has_next);

  auto* memory = metrics.set_android_mem();
  memory->set_system_metrics()->set_lmks()->set_total_count(
      static_cast<int32_t>(it.Get(0).long_value));

  it = tp->ExecuteQuery("SELECT * from anon_rss;");
  while (it.Next()) {
    const char* name = it.Get(0).string_value;

    auto* process = memory->add_process_metrics();
    process->set_process_name(name);

    auto* anon = process->set_overall_counters()->set_anon_rss();
    anon->set_min(it.Get(1).AsDouble());
    anon->set_max(it.Get(2).AsDouble());
    anon->set_avg(it.Get(3).AsDouble());
  }
  if (auto opt_error = it.GetLastError()) {
    PERFETTO_ELOG("SQLite error: %s", opt_error->c_str());
    return 1;
  }

  metrics.Finalize();
  *metrics_proto = delegate.StitchSlices();
  return 0;
}

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto
