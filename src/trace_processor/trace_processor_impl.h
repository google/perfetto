/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_IMPL_H_
#define SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_IMPL_H_

#include <sqlite3.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/status.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/create_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/create_view_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/import.h"
#include "src/trace_processor/sqlite/db_sqlite_table.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sqlite_engine.h"
#include "src/trace_processor/trace_processor_storage_impl.h"
#include "src/trace_processor/util/sql_modules.h"

#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto {
namespace trace_processor {

// Coordinates the loading of traces from an arbitrary source and allows
// execution of SQL queries on the events in these traces.
class TraceProcessorImpl : public TraceProcessor,
                           public TraceProcessorStorageImpl {
 public:
  explicit TraceProcessorImpl(const Config&);

  TraceProcessorImpl(const TraceProcessorImpl&) = delete;
  TraceProcessorImpl& operator=(const TraceProcessorImpl&) = delete;

  TraceProcessorImpl(TraceProcessorImpl&&) = delete;
  TraceProcessorImpl& operator=(TraceProcessorImpl&&) = delete;

  ~TraceProcessorImpl() override;

  // TraceProcessorStorage implementation:
  base::Status Parse(TraceBlobView) override;
  void Flush() override;
  void NotifyEndOfFile() override;

  // TraceProcessor implementation:
  Iterator ExecuteQuery(const std::string& sql) override;

  base::Status RegisterMetric(const std::string& path,
                              const std::string& sql) override;

  base::Status RegisterSqlModule(SqlModule sql_module) override;

  base::Status ExtendMetricsProto(const uint8_t* data, size_t size) override;

  base::Status ExtendMetricsProto(
      const uint8_t* data,
      size_t size,
      const std::vector<std::string>& skip_prefixes) override;

  base::Status ComputeMetric(const std::vector<std::string>& metric_names,
                             std::vector<uint8_t>* metrics) override;

  base::Status ComputeMetricText(const std::vector<std::string>& metric_names,
                                 TraceProcessor::MetricResultFormat format,
                                 std::string* metrics_string) override;

  std::vector<uint8_t> GetMetricDescriptors() override;

  void InterruptQuery() override;

  size_t RestoreInitialTables() override;

  std::string GetCurrentTraceName() override;
  void SetCurrentTraceName(const std::string&) override;

  void EnableMetatrace(MetatraceConfig config) override;

  base::Status DisableAndReadMetatrace(
      std::vector<uint8_t>* trace_proto) override;

 private:
  // Needed for iterators to be able to access the context.
  friend class IteratorImpl;

  template <typename Table>
  void RegisterStaticTable(const Table& table) {
    engine_.RegisterStaticTable(table, Table::Name());
  }

  void RegisterStaticTableFunction(std::unique_ptr<StaticTableFunction> fn) {
    engine_.RegisterStaticTableFunction(std::move(fn));
  }

  template <typename View>
  void RegisterView(const View& view);

  bool IsRootMetricField(const std::string& metric_name);

  PerfettoSqlEngine engine_;

  DescriptorPool pool_;

  std::vector<metrics::SqlMetricFile> sql_metrics_;
  std::unordered_map<std::string, std::string> proto_field_to_sql_metric_path_;

  // This is atomic because it is set by the CTRL-C signal handler and we need
  // to prevent single-flow compiler optimizations in ExecuteQuery().
  std::atomic<bool> query_interrupted_{false};

  // Keeps track of the tables created by the ingestion process. This is used
  // by RestoreInitialTables() to delete all the tables/view that have been
  // created after that point.
  std::vector<std::string> initial_tables_;

  std::string current_trace_name_;
  uint64_t bytes_parsed_ = 0;

  // NotifyEndOfFile should only be called once. Set to true whenever it is
  // called.
  bool notify_eof_called_ = false;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_IMPL_H_
