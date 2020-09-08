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
#include <string>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/status.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/sqlite/db_sqlite_table.h"
#include "src/trace_processor/sqlite/query_cache.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/trace_processor_storage_impl.h"

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

  ~TraceProcessorImpl() override;

  // TraceProcessorStorage implementation:
  util::Status Parse(std::unique_ptr<uint8_t[]>, size_t) override;
  void NotifyEndOfFile() override;

  // TraceProcessor implementation:
  Iterator ExecuteQuery(const std::string& sql,
                        int64_t time_queued = 0) override;

  util::Status RegisterMetric(const std::string& path,
                              const std::string& sql) override;

  util::Status ExtendMetricsProto(const uint8_t* data, size_t size) override;

  util::Status ComputeMetric(const std::vector<std::string>& metric_names,
                             std::vector<uint8_t>* metrics) override;

  std::vector<uint8_t> GetMetricDescriptors() override;

  void InterruptQuery() override;

  size_t RestoreInitialTables() override;

  std::string GetCurrentTraceName() override;
  void SetCurrentTraceName(const std::string&) override;

  void EnableMetatrace() override;

  util::Status DisableAndReadMetatrace(
      std::vector<uint8_t>* trace_proto) override;

 private:
  // Needed for iterators to be able to access the context.
  friend class IteratorImpl;

  template <typename Table>
  void RegisterDbTable(const Table& table) {
    DbSqliteTable::RegisterTable(*db_, query_cache_.get(), Table::Schema(),
                                 &table, table.table_name());
  }

  void RegisterDynamicTable(
      std::unique_ptr<DbSqliteTable::DynamicTableGenerator> generator) {
    DbSqliteTable::RegisterTable(*db_, query_cache_.get(),
                                 std::move(generator));
  }

  bool IsRootMetricField(const std::string& metric_name);
  ScopedDb db_;
  std::unique_ptr<QueryCache> query_cache_;

  DescriptorPool pool_;
  std::vector<metrics::SqlMetricFile> sql_metrics_;

  // This is atomic because it is set by the CTRL-C signal handler and we need
  // to prevent single-flow compiler optimizations in ExecuteQuery().
  std::atomic<bool> query_interrupted_{false};

  // Keeps track of the tables created by the ingestion process. This is used
  // by RestoreInitialTables() to delete all the tables/view that have been
  // created after that point.
  std::vector<std::string> initial_tables_;

  std::string current_trace_name_;
  uint64_t bytes_parsed_ = 0;
};


}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_IMPL_H_
