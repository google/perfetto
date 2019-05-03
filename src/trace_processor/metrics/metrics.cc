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

#include "perfetto/base/string_utils.h"
#include "perfetto/metrics/android/mem_metric.pbzero.h"
#include "perfetto/metrics/metrics.pbzero.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/metrics/sql_metrics.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

void RunMetric(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  auto* tp = static_cast<TraceProcessor*>(sqlite3_user_data(ctx));
  if (argc == 0 || sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
    sqlite3_result_error(ctx, "Invalid call to RUN_METRIC", -1);
    return;
  }

  const char* filename =
      reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
  const char* sql = sql_metrics::GetBundledMetric(filename);
  if (!sql) {
    sqlite3_result_error(ctx, "Unknown filename provided to RUN_METRIC", -1);
    return;
  }

  for (const auto& query : base::SplitString(sql, ";\n\n")) {
    PERFETTO_DLOG("Executing query in RUN_METRIC: %s", query.c_str());

    auto it = tp->ExecuteQuery(query);
    if (auto opt_error = it.GetLastError()) {
      sqlite3_result_error(ctx, "Error when running RUN_METRIC file", -1);
      return;
    } else if (it.Next()) {
      sqlite3_result_error(
          ctx, "RUN_METRIC functions should not produce any output", -1);
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

  auto queries = base::SplitString(sql_metrics::kAndroidMem, ";\n\n");
  for (const auto& query : queries) {
    PERFETTO_DLOG("Executing query: %s", query.c_str());
    auto prep_it = tp->ExecuteQuery(query);
    auto prep_has_next = prep_it.Next();
    if (auto opt_error = prep_it.GetLastError()) {
      PERFETTO_ELOG("SQLite error: %s", opt_error->c_str());
      return 1;
    }
    PERFETTO_DCHECK(!prep_has_next);
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

  auto* memory = metrics.set_android_mem();
  memory->set_system_metrics()->set_lmks()->set_total_count(
      static_cast<int32_t>(it.Get(0).long_value));
  metrics.Finalize();

  *metrics_proto = delegate.StitchSlices();

  has_next = it.Next();
  PERFETTO_DCHECK(!has_next);
  return 0;
}

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto
