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
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/summarizer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/core/plugin/plugin.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_DUCKDB)
#include "src/trace_processor/duckdb/duckdb_engine.h"
#include "src/trace_processor/plugins/args/args.h"
#endif
#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/trace_processor_storage_impl.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor {

class SqliteIteratorImpl;

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

  // =================================================================
  // |        TraceProcessorStorage implementation starts here       |
  // =================================================================

  base::Status Parse(TraceBlobView) override;
  void Flush() override;
  base::Status NotifyEndOfFile() override;

  // =================================================================
  // |        PerfettoSQL related functionality starts here          |
  // =================================================================

  Iterator ExecuteQuery(const std::string& sql) override;

  base::Status RegisterSqlPackage(SqlPackage) override;

  // =================================================================
  // |  Trace-based metrics (v2) related functionality starts here   |
  // =================================================================

  base::Status Summarize(const TraceSummaryComputationSpec& computation,
                         const std::vector<TraceSummarySpecBytes>& specs,
                         std::vector<uint8_t>* output,
                         const TraceSummaryOutputSpec& output_spec) override;

  // =================================================================
  // |        Metatracing related functionality starts here          |
  // =================================================================

  void EnableMetatrace(MetatraceConfig config) override;

  base::Status DisableAndReadMetatrace(
      std::vector<uint8_t>* trace_proto) override;

  // =================================================================
  // |              Advanced functionality starts here               |
  // =================================================================

  std::string GetCurrentTraceName() override;
  void SetCurrentTraceName(const std::string&) override;

  base::Status RegisterFileContent(const std::string& path,
                                   TraceBlob content) override;

  void InterruptQuery() override;

  size_t RestoreInitialTables() override;

  // =================================================================
  // |  Trace-based metrics (v1) related functionality starts here   |
  // =================================================================

  base::Status RegisterMetric(const std::string& path,
                              const std::string& sql) override;

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

  // ===================
  // |   Summarizer    |
  // ===================

  base::Status CreateSummarizer(std::unique_ptr<Summarizer>* out) override;

  // Fallback-honesty counters (EXPERIMENTAL, SQLite -> DuckDB migration).
  // Exposed for tests/measurement: how many ExecuteQuery calls actually ran
  // inside DuckDB vs fell back to SQLite while the DuckDB engine was enabled.
  // Always present (cheap two-uint64 counters) so test code links in any build.
  uint64_t queries_executed_in_duckdb() const {
    return queries_executed_in_duckdb_;
  }
  uint64_t queries_fell_back_to_sqlite() const {
    return queries_fell_back_to_sqlite_;
  }

 private:
  // Needed for iterators to be able to access the context.
  friend class SqliteIteratorImpl;

  // By-value RegisterMetric body. External callers go through the
  // |RegisterMetric| override (which copies its const-ref args into our
  // parameters); the constructor's amalgamated-metrics loop calls this
  // directly so it can move the temporaries through without extra copies.
  base::Status RegisterMetricImpl(std::string path, std::string sql);

  bool IsRootMetricField(const std::string& metric_name);

  void CacheBoundsAndBuildTable();

  struct InitPerfettoSqlConnectionArgs {
    TraceProcessorContext* context;
    TraceStorage* storage;
    const Config& config;
    const std::list<SqlPackage>& packages;
    std::vector<metrics::SqlMetricFile>& sql_metrics;
    const DescriptorPool* metrics_descriptor_pool;
    std::unordered_map<std::string, std::string>* proto_fn_name_to_path;
    TraceProcessor* trace_processor;
    bool notify_eof_called;
    std::pair<int64_t, int64_t> cached_trace_bounds;
    std::vector<std::unique_ptr<PluginBase>>& plugins;
    const std::vector<PluginDataframe>& plugin_dataframes;
  };

  static std::unique_ptr<PerfettoSqlConnection> InitPerfettoSqlConnection(
      const InitPerfettoSqlConnectionArgs& args);

  static void IncludeAfterEofPrelude(PerfettoSqlConnection*);

  const Config config_;

  // Registered plugins, topologically sorted by dependency order.
  std::vector<std::unique_ptr<PluginBase>> plugins_;

  // Dataframes contributed by plugins, collected once after plugin
  // construction. Both InitPerfettoSqlConnection and NotifyEndOfFile read
  // from this list, so RegisterDataframes runs exactly once per plugin.
  std::vector<PluginDataframe> plugin_dataframes_;

  std::unique_ptr<PerfettoSqlConnection> engine_;

#if PERFETTO_BUILDFLAG(PERFETTO_TP_DUCKDB)
  // Lazily created on the first DuckDB-eligible query (only when
  // config_.enable_duckdb_query_engine is set). Owns the DuckDB
  // database/connection + table provider.
  std::unique_ptr<duckdb_integration::DuckDbEngine> duckdb_engine_;
  // Cache for the DuckDB table-function mirror: maps a stdlib RETURNS TABLE
  // function name to {raw authored body, pipelined body}. The pipeline (interval
  // /graph rewrites + macro expansion) is run once per (name, raw-body); a
  // CREATE OR REPLACE with a changed body re-pipelines. Avoids re-parsing every
  // function body on every query.
  std::unordered_map<std::string, std::pair<std::string, std::string>>
      duckdb_mirror_body_cache_;
  // Backs the DuckDB-lane __intrinsic_arg_set_to_json UDF: the args plugin's
  // converter (reused for byte-exact JSON) + a mutex serializing its mutable
  // scratch (DuckDB may call the scalar from worker threads).
  std::unique_ptr<ArgSetToJson::Context> duckdb_arg_set_json_ctx_;
  std::mutex duckdb_arg_set_json_mu_;
  // Runs the DuckDB rewrite->expand pipeline over an authored RETURNS TABLE
  // function body (interval/graph macro rewrites, then macro expansion), so the
  // function mirrors into DuckDB using native functions. Returns the pipelined
  // body, or the rewritten-but-unexpanded body if expansion fails.
  std::string PipelineDuckDbMirrorBody(const std::string& raw_body);
#endif

  // Fallback-honesty counters. See accessors above. Defined unconditionally so
  // the (test-only) accessors are always available, regardless of build config.
  uint64_t queries_executed_in_duckdb_ = 0;
  uint64_t queries_fell_back_to_sqlite_ = 0;

  DescriptorPool metrics_descriptor_pool_;

  std::vector<metrics::SqlMetricFile> sql_metrics_;
  // list (not vector) for stable element addresses: RegisteredPackage holds
  // string_views into these std::strings.
  std::list<SqlPackage> registered_sql_packages_;

  std::unordered_map<std::string, std::string> proto_field_to_sql_metric_path_;
  std::unordered_map<std::string, std::string> proto_fn_name_to_path_;

  // This is atomic because it is set by the CTRL-C signal handler and we need
  // to prevent single-flow compiler optimizations in ExecuteQuery().
  std::atomic<bool> query_interrupted_{false};

  // Track the number of objects registered with SQLite post prelude.
  uint64_t sqlite_objects_post_prelude_ = 0;

  std::string current_trace_name_;
  uint64_t bytes_parsed_ = 0;

  // NotifyEndOfFile should only be called once. Set to true whenever it is
  // called.
  bool notify_eof_called_ = false;

  // Cached trace timestamp bounds. This is set in NotifyEndOfFile before
  // tables are finalized and reused in RestoreInitialTables to avoid
  // iterating over finalized dataframes.
  std::pair<int64_t, int64_t> cached_trace_bounds_ = {0, 0};

  // Tracks the sum of mutations across all tables used by
  // CacheBoundsAndBuildTable to avoid recomputing bounds when unchanged.
  uint64_t bounds_tables_mutations_ = 0;

  // Auto-incrementing counter for generating unique summarizer ids.
  uint32_t next_summarizer_id_ = 0;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_TRACE_PROCESSOR_IMPL_H_
