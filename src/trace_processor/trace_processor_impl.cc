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

#include "src/trace_processor/trace_processor_impl.h"

#include <inttypes.h>
#include <algorithm>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/android_logs_table.h"
#include "src/trace_processor/args_table.h"
#include "src/trace_processor/counter_values_table.h"
#include "src/trace_processor/cpu_profile_stack_sample_table.h"
#include "src/trace_processor/heap_profile_allocation_table.h"
#include "src/trace_processor/instants_table.h"
#include "src/trace_processor/metadata_table.h"
#include "src/trace_processor/process_table.h"
#include "src/trace_processor/raw_table.h"
#include "src/trace_processor/sched_slice_table.h"
#include "src/trace_processor/slice_table.h"
#include "src/trace_processor/span_join_operator_table.h"
#include "src/trace_processor/sql_stats_table.h"
#include "src/trace_processor/sqlite/db_sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite3_str_split.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/stack_profile_frame_table.h"
#include "src/trace_processor/stack_profile_mapping_table.h"
#include "src/trace_processor/stats_table.h"
#include "src/trace_processor/thread_table.h"
#include "src/trace_processor/window_operator_table.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_METRICS)
#include "src/trace_processor/metrics/metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/metrics/sql_metrics.h"
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_METRICS)

#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
#include "src/trace_processor/export_json.h"
#endif

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <cxxabi.h>
#endif

// In Android and Chromium tree builds, we don't have the percentile module.
// Just don't include it.
#if PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)
// defined in sqlite_src/ext/misc/percentile.c
extern "C" int sqlite3_percentile_init(sqlite3* db,
                                       char** error,
                                       const sqlite3_api_routines* api);
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)

namespace perfetto {
namespace trace_processor {
namespace {

const char kAllTablesQuery[] =
    "SELECT tbl_name, type FROM (SELECT * FROM sqlite_master UNION ALL SELECT "
    "* FROM sqlite_temp_master)";

void InitializeSqlite(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db, "PRAGMA temp_store=2", 0, 0, &error);
  if (error) {
    PERFETTO_FATAL("Error setting pragma temp_store: %s", error);
  }
  sqlite3_str_split_init(db);
// In Android tree builds, we don't have the percentile module.
// Just don't include it.
#if PERFETTO_BUILDFLAG(PERFETTO_TP_PERCENTILE)
  sqlite3_percentile_init(db, &error, nullptr);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
#endif
}

void BuildBoundsTable(sqlite3* db, std::pair<int64_t, int64_t> bounds) {
  char* error = nullptr;
  sqlite3_exec(db, "DELETE FROM trace_bounds", nullptr, nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error deleting from bounds table: %s", error);
    sqlite3_free(error);
    return;
  }

  char* insert_sql = sqlite3_mprintf("INSERT INTO trace_bounds VALUES(%" PRId64
                                     ", %" PRId64 ")",
                                     bounds.first, bounds.second);

  sqlite3_exec(db, insert_sql, 0, 0, &error);
  sqlite3_free(insert_sql);
  if (error) {
    PERFETTO_ELOG("Error inserting bounds table: %s", error);
    sqlite3_free(error);
  }
}

void CreateBuiltinTables(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db, "CREATE TABLE perfetto_tables(name STRING)", 0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
  sqlite3_exec(db,
               "CREATE TABLE trace_bounds(start_ts BIG INT, end_ts BIG INT)", 0,
               0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  // Initialize the bounds table with some data so even before parsing any data,
  // we still have a valid table.
  BuildBoundsTable(db, std::make_pair(0, 0));
}

void CreateBuiltinViews(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db,
               "CREATE VIEW counter_definitions AS "
               "SELECT "
               "  *, "
               "  id AS counter_id "
               "FROM counter_track",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  sqlite3_exec(db,
               "CREATE VIEW counter_values AS "
               "SELECT "
               "  *, "
               "  track_id as counter_id "
               "FROM counter",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  sqlite3_exec(db,
               "CREATE VIEW counters AS "
               "SELECT * "
               "FROM counter_values v "
               "INNER JOIN counter_track t "
               "ON v.track_id = t.id "
               "ORDER BY ts;",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  sqlite3_exec(db,
               "CREATE VIEW slice AS "
               "SELECT "
               "  *, "
               "  category AS cat, "
               "  CASE ref_type "
               "    WHEN 'utid' THEN ref "
               "    ELSE NULL "
               "  END AS utid "
               "FROM internal_slice;",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  sqlite3_exec(db,
               "CREATE VIEW gpu_slice AS "
               "SELECT "
               "* "
               "FROM internal_gpu_slice join internal_slice using(slice_id);",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  // Legacy view for "slice" table with a deprecated table name.
  // TODO(eseckler): Remove this view when all users have switched to "slice".
  sqlite3_exec(db,
               "CREATE VIEW slices AS "
               "SELECT * FROM slice;",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
}

#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
void ExportJson(sqlite3_context* ctx, int /*argc*/, sqlite3_value** argv) {
  TraceStorage* storage = static_cast<TraceStorage*>(sqlite3_user_data(ctx));
  FILE* output;
  if (sqlite3_value_type(argv[0]) == SQLITE_INTEGER) {
    // Assume input is an FD.
    output = fdopen(sqlite3_value_int(argv[0]), "w");
    if (!output) {
      sqlite3_result_error(ctx, "Couldn't open output file from given FD", -1);
      return;
    }
  } else {
    const char* filename =
        reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    output = fopen(filename, "w");
    if (!output) {
      sqlite3_result_error(ctx, "Couldn't open output file", -1);
      return;
    }
  }

  util::Status result = json::ExportJson(storage, output);
  if (!result.ok()) {
    sqlite3_result_error(ctx, result.message().c_str(), -1);
    return;
  }
}

void CreateJsonExportFunction(TraceStorage* ts, sqlite3* db) {
  auto ret = sqlite3_create_function_v2(db, "EXPORT_JSON", 1, SQLITE_UTF8, ts,
                                        ExportJson, nullptr, nullptr,
                                        sqlite_utils::kSqliteStatic);
  if (ret) {
    PERFETTO_ELOG("Error initializing EXPORT_JSON");
  }
}
#endif

void Hash(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  base::Hash hash;
  for (int i = 0; i < argc; ++i) {
    sqlite3_value* value = argv[i];
    switch (sqlite3_value_type(value)) {
      case SQLITE_INTEGER:
        hash.Update(sqlite3_value_int64(value));
        break;
      case SQLITE_TEXT: {
        const char* ptr =
            reinterpret_cast<const char*>(sqlite3_value_text(value));
        hash.Update(ptr, strlen(ptr));
        break;
      }
      default:
        sqlite3_result_error(ctx, "Unsupported type of arg passed to HASH", -1);
        return;
    }
  }
  sqlite3_result_int64(ctx, static_cast<int64_t>(hash.digest()));
}

void Demangle(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != 1) {
    sqlite3_result_error(ctx, "Unsupported number of arg passed to DEMANGLE",
                         -1);
    return;
  }
  sqlite3_value* value = argv[0];
  if (sqlite3_value_type(value) != SQLITE_TEXT) {
    sqlite3_result_error(ctx, "Unsupported type of arg passed to DEMANGLE", -1);
    return;
  }
  const char* ptr = reinterpret_cast<const char*>(sqlite3_value_text(value));
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  int ignored = 0;
  // This memory was allocated by malloc and will be passed to SQLite to free.
  char* demangled_name = abi::__cxa_demangle(ptr, nullptr, nullptr, &ignored);
  if (!demangled_name) {
    sqlite3_result_null(ctx);
    return;
  }
  sqlite3_result_text(ctx, demangled_name, -1, free);
#else
  sqlite3_result_text(ctx, ptr, -1, sqlite_utils::kSqliteTransient);
#endif
}

void CreateHashFunction(sqlite3* db) {
  auto ret = sqlite3_create_function_v2(
      db, "HASH", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, &Hash,
      nullptr, nullptr, nullptr);
  if (ret) {
    PERFETTO_ELOG("Error initializing HASH");
  }
}

void CreateDemangledNameFunction(sqlite3* db) {
  auto ret = sqlite3_create_function_v2(
      db, "DEMANGLE", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, &Demangle,
      nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    PERFETTO_ELOG("Error initializing DEMANGLE: %s", sqlite3_errmsg(db));
  }
}

#if PERFETTO_BUILDFLAG(PERFETTO_TP_METRICS)
void SetupMetrics(TraceProcessor* tp,
                  sqlite3* db,
                  std::vector<metrics::SqlMetricFile>* sql_metrics) {
  tp->ExtendMetricsProto(kMetricsDescriptor.data(), kMetricsDescriptor.size());

  for (const auto& file_to_sql : metrics::sql_metrics::kFileToSql) {
    tp->RegisterMetric(file_to_sql.path, file_to_sql.sql);
  }

  {
    std::unique_ptr<metrics::RunMetricContext> ctx(
        new metrics::RunMetricContext());
    ctx->tp = tp;
    ctx->metrics = sql_metrics;
    auto ret = sqlite3_create_function_v2(
        db, "RUN_METRIC", -1, SQLITE_UTF8, ctx.release(), metrics::RunMetric,
        nullptr, nullptr,
        [](void* ptr) { delete static_cast<metrics::RunMetricContext*>(ptr); });
    if (ret)
      PERFETTO_ELOG("Error initializing RUN_METRIC");
  }

  {
    auto ret = sqlite3_create_function_v2(
        db, "RepeatedField", 1, SQLITE_UTF8, nullptr, nullptr,
        metrics::RepeatedFieldStep, metrics::RepeatedFieldFinal, nullptr);
    if (ret)
      PERFETTO_ELOG("Error initializing RepeatedField");
  }
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_METRICS)

}  // namespace

TraceProcessorImpl::TraceProcessorImpl(const Config& cfg)
    : TraceProcessorStorageImpl(cfg) {
  sqlite3* db = nullptr;
  PERFETTO_CHECK(sqlite3_initialize() == SQLITE_OK);
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  InitializeSqlite(db);
  CreateBuiltinTables(db);
  CreateBuiltinViews(db);
  db_.reset(std::move(db));

#if PERFETTO_BUILDFLAG(PERFETTO_TP_JSON)
  CreateJsonExportFunction(this->context_.storage.get(), db);
#endif
  CreateHashFunction(db);
  CreateDemangledNameFunction(db);

#if PERFETTO_BUILDFLAG(PERFETTO_TP_METRICS)
  SetupMetrics(this, *db_, &sql_metrics_);
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_METRICS)

  ArgsTable::RegisterTable(*db_, context_.storage.get());
  ProcessTable::RegisterTable(*db_, context_.storage.get());
#if PERFETTO_BUILDFLAG(PERFETTO_TP_FTRACE)
  SchedSliceTable::RegisterTable(*db_, context_.storage.get());
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_FTRACE)
  SliceTable::RegisterTable(*db_, context_.storage.get());
  SqlStatsTable::RegisterTable(*db_, context_.storage.get());
  ThreadTable::RegisterTable(*db_, context_.storage.get());
  CounterValuesTable::RegisterTable(*db_, context_.storage.get());
  SpanJoinOperatorTable::RegisterTable(*db_, context_.storage.get());
  WindowOperatorTable::RegisterTable(*db_, context_.storage.get());
  InstantsTable::RegisterTable(*db_, context_.storage.get());
  StatsTable::RegisterTable(*db_, context_.storage.get());
  AndroidLogsTable::RegisterTable(*db_, context_.storage.get());
  RawTable::RegisterTable(*db_, context_.storage.get());
  HeapProfileAllocationTable::RegisterTable(*db_, context_.storage.get());
  CpuProfileStackSampleTable::RegisterTable(*db_, context_.storage.get());
  StackProfileFrameTable::RegisterTable(*db_, context_.storage.get());
  StackProfileMappingTable::RegisterTable(*db_, context_.storage.get());
  MetadataTable::RegisterTable(*db_, context_.storage.get());

  // New style db-backed tables.
  const TraceStorage* storage = context_.storage.get();

  DbSqliteTable::RegisterTable(*db_, &storage->track_table(),
                               storage->track_table().table_name());
  DbSqliteTable::RegisterTable(*db_, &storage->thread_track_table(),
                               storage->thread_track_table().table_name());
  DbSqliteTable::RegisterTable(*db_, &storage->process_track_table(),
                               storage->process_track_table().table_name());
  DbSqliteTable::RegisterTable(*db_, &storage->gpu_slice_table(),
                               storage->gpu_slice_table().table_name());
  DbSqliteTable::RegisterTable(*db_, &storage->gpu_track_table(),
                               storage->gpu_track_table().table_name());

  DbSqliteTable::RegisterTable(*db_, &storage->counter_track_table(),
                               storage->counter_track_table().table_name());
  DbSqliteTable::RegisterTable(
      *db_, &storage->process_counter_track_table(),
      storage->process_counter_track_table().table_name());
  DbSqliteTable::RegisterTable(
      *db_, &storage->thread_counter_track_table(),
      storage->thread_counter_track_table().table_name());
  DbSqliteTable::RegisterTable(*db_, &storage->cpu_counter_track_table(),
                               storage->cpu_counter_track_table().table_name());
  DbSqliteTable::RegisterTable(*db_, &storage->irq_counter_track_table(),
                               storage->irq_counter_track_table().table_name());
  DbSqliteTable::RegisterTable(
      *db_, &storage->softirq_counter_track_table(),
      storage->softirq_counter_track_table().table_name());
  DbSqliteTable::RegisterTable(*db_, &storage->gpu_counter_track_table(),
                               storage->gpu_counter_track_table().table_name());

  DbSqliteTable::RegisterTable(*db_, &storage->heap_graph_object_table(),
                               storage->heap_graph_object_table().table_name());
  DbSqliteTable::RegisterTable(
      *db_, &storage->heap_graph_reference_table(),
      storage->heap_graph_reference_table().table_name());

  DbSqliteTable::RegisterTable(*db_, &storage->symbol_table(),
                               storage->symbol_table().table_name());
  DbSqliteTable::RegisterTable(
      *db_, &storage->stack_profile_callsite_table(),
      storage->stack_profile_callsite_table().table_name());

  DbSqliteTable::RegisterTable(
      *db_, &storage->vulkan_memory_allocations_table(),
      storage->vulkan_memory_allocations_table().table_name());
}

TraceProcessorImpl::~TraceProcessorImpl() {
  for (auto* it : iterators_)
    it->Reset();
}

util::Status TraceProcessorImpl::Parse(std::unique_ptr<uint8_t[]> data,
                                       size_t size) {
  bytes_parsed_ += size;
  return TraceProcessorStorageImpl::Parse(std::move(data), size);
}

std::string TraceProcessorImpl::GetCurrentTraceName() {
  if (current_trace_name_.empty())
    return "";
  auto size = " (" + std::to_string(bytes_parsed_ / 1024 / 1024) + " MB)";
  return current_trace_name_ + size;
}

void TraceProcessorImpl::SetCurrentTraceName(const std::string& name) {
  current_trace_name_ = name;
}

void TraceProcessorImpl::NotifyEndOfFile() {
  if (current_trace_name_.empty())
    current_trace_name_ = "Unnamed trace";

  TraceProcessorStorageImpl::NotifyEndOfFile();

  BuildBoundsTable(*db_, context_.storage->GetTraceTimestampBoundsNs());

  // Create a snapshot of all tables and views created so far. This is so later
  // we can drop all extra tables created by the UI and reset to the original
  // state (see RestoreInitialTables).
  initial_tables_.clear();
  auto it = ExecuteQuery(kAllTablesQuery);
  while (it.Next()) {
    auto value = it.Get(0);
    PERFETTO_CHECK(value.type == SqlValue::Type::kString);
    initial_tables_.push_back(value.string_value);
  }
}

size_t TraceProcessorImpl::RestoreInitialTables() {
  std::vector<std::pair<std::string, std::string>> deletion_list;
  std::string msg = "Resetting DB to initial state, deleting table/views:";
  for (auto it = ExecuteQuery(kAllTablesQuery); it.Next();) {
    std::string name(it.Get(0).string_value);
    std::string type(it.Get(1).string_value);
    if (std::find(initial_tables_.begin(), initial_tables_.end(), name) ==
        initial_tables_.end()) {
      msg += " " + name;
      deletion_list.push_back(std::make_pair(type, name));
    }
  }

  PERFETTO_LOG("%s", msg.c_str());
  for (const auto& tn : deletion_list) {
    std::string query = "DROP " + tn.first + " " + tn.second;
    auto it = ExecuteQuery(query);
    while (it.Next()) {
    }
    PERFETTO_CHECK(it.Status().ok());
  }
  return deletion_list.size();
}

TraceProcessor::Iterator TraceProcessorImpl::ExecuteQuery(
    const std::string& sql,
    int64_t time_queued) {
  sqlite3_stmt* raw_stmt;
  int err = sqlite3_prepare_v2(*db_, sql.c_str(), static_cast<int>(sql.size()),
                               &raw_stmt, nullptr);
  util::Status status;
  uint32_t col_count = 0;
  if (err != SQLITE_OK) {
    status = util::ErrStatus("%s", sqlite3_errmsg(*db_));
  } else {
    col_count = static_cast<uint32_t>(sqlite3_column_count(raw_stmt));
  }

  base::TimeNanos t_start = base::GetWallTimeNs();
  uint32_t sql_stats_row =
      context_.storage->mutable_sql_stats()->RecordQueryBegin(sql, time_queued,
                                                              t_start.count());

  std::unique_ptr<IteratorImpl> impl(new IteratorImpl(
      this, *db_, ScopedStmt(raw_stmt), col_count, status, sql_stats_row));
  iterators_.emplace_back(impl.get());
  return TraceProcessor::Iterator(std::move(impl));
}

void TraceProcessorImpl::InterruptQuery() {
  if (!db_)
    return;
  query_interrupted_.store(true);
  sqlite3_interrupt(db_.get());
}

#if PERFETTO_BUILDFLAG(PERFETTO_TP_METRICS)
util::Status TraceProcessorImpl::RegisterMetric(const std::string& path,
                                                const std::string& sql) {
  std::string stripped_sql;
  for (base::StringSplitter sp(sql, '\n'); sp.Next();) {
    if (strncmp(sp.cur_token(), "--", 2) != 0) {
      stripped_sql.append(sp.cur_token());
      stripped_sql.push_back('\n');
    }
  }

  // Check if the metric with the given path already exists and if it does, just
  // update the SQL associated with it.
  auto it = std::find_if(
      sql_metrics_.begin(), sql_metrics_.end(),
      [&path](const metrics::SqlMetricFile& m) { return m.path == path; });
  if (it != sql_metrics_.end()) {
    it->sql = stripped_sql;
    return util::OkStatus();
  }

  auto sep_idx = path.rfind("/");
  std::string basename =
      sep_idx == std::string::npos ? path : path.substr(sep_idx + 1);

  auto sql_idx = basename.rfind(".sql");
  if (sql_idx == std::string::npos) {
    return util::ErrStatus("Unable to find .sql extension for metric");
  }
  auto no_ext_name = basename.substr(0, sql_idx);

  metrics::SqlMetricFile metric;
  metric.path = path;
  metric.proto_field_name = no_ext_name;
  metric.output_table_name = no_ext_name + "_output";
  metric.sql = stripped_sql;
  sql_metrics_.emplace_back(metric);
  return util::OkStatus();
}

util::Status TraceProcessorImpl::ExtendMetricsProto(const uint8_t* data,
                                                    size_t size) {
  util::Status status = pool_.AddFromFileDescriptorSet(data, size);
  if (!status.ok())
    return status;

  for (const auto& desc : pool_.descriptors()) {
    // Convert the full name (e.g. .perfetto.protos.TraceMetrics.SubMetric)
    // into a function name of the form (TraceMetrics_SubMetric).
    auto fn_name = desc.full_name().substr(desc.package_name().size() + 1);
    std::replace(fn_name.begin(), fn_name.end(), '.', '_');

    std::unique_ptr<metrics::BuildProtoContext> ctx(
        new metrics::BuildProtoContext());
    ctx->tp = this;
    ctx->pool = &pool_;
    ctx->desc = &desc;

    auto ret = sqlite3_create_function_v2(
        *db_, fn_name.c_str(), -1, SQLITE_UTF8, ctx.release(),
        metrics::BuildProto, nullptr, nullptr, [](void* ptr) {
          delete static_cast<metrics::BuildProtoContext*>(ptr);
        });
    if (ret != SQLITE_OK)
      return util::ErrStatus("%s", sqlite3_errmsg(*db_));
  }
  return util::OkStatus();
}

util::Status TraceProcessorImpl::ComputeMetric(
    const std::vector<std::string>& metric_names,
    std::vector<uint8_t>* metrics_proto) {
  auto opt_idx = pool_.FindDescriptorIdx(".perfetto.protos.TraceMetrics");
  if (!opt_idx.has_value())
    return util::Status("Root metrics proto descriptor not found");

  const auto& root_descriptor = pool_.descriptors()[opt_idx.value()];
  return metrics::ComputeMetrics(this, metric_names, sql_metrics_,
                                 root_descriptor, metrics_proto);
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_TP_METRICS)

TraceProcessor::IteratorImpl::IteratorImpl(TraceProcessorImpl* trace_processor,
                                           sqlite3* db,
                                           ScopedStmt stmt,
                                           uint32_t column_count,
                                           util::Status status,
                                           uint32_t sql_stats_row)
    : trace_processor_(trace_processor),
      db_(db),
      stmt_(std::move(stmt)),
      column_count_(column_count),
      status_(status),
      sql_stats_row_(sql_stats_row) {}

TraceProcessor::IteratorImpl::~IteratorImpl() {
  if (trace_processor_) {
    auto* its = &trace_processor_->iterators_;
    auto it = std::find(its->begin(), its->end(), this);
    PERFETTO_CHECK(it != its->end());
    its->erase(it);

    base::TimeNanos t_end = base::GetWallTimeNs();
    auto* sql_stats = trace_processor_->context_.storage->mutable_sql_stats();
    sql_stats->RecordQueryEnd(sql_stats_row_, t_end.count());
  }
}

void TraceProcessor::IteratorImpl::Reset() {
  *this = IteratorImpl(nullptr, nullptr, ScopedStmt(), 0,
                       util::ErrStatus("Trace processor was deleted"), 0);
}

void TraceProcessor::IteratorImpl::RecordFirstNextInSqlStats() {
  base::TimeNanos t_first_next = base::GetWallTimeNs();
  auto* sql_stats = trace_processor_->context_.storage->mutable_sql_stats();
  sql_stats->RecordQueryFirstNext(sql_stats_row_, t_first_next.count());
}

}  // namespace trace_processor
}  // namespace perfetto
