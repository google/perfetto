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
#include <fstream>
#include <functional>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/android_logs_table.h"
#include "src/trace_processor/args_table.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/clock_tracker.h"
#include "src/trace_processor/counter_definitions_table.h"
#include "src/trace_processor/counter_values_table.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/heap_profile_allocation_table.h"
#include "src/trace_processor/heap_profile_tracker.h"
#include "src/trace_processor/instants_table.h"
#include "src/trace_processor/metadata_table.h"
#include "src/trace_processor/metrics/descriptors.h"
#include "src/trace_processor/metrics/metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/metrics/sql_metrics.h"
#include "src/trace_processor/process_table.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/proto_trace_tokenizer.h"
#include "src/trace_processor/raw_table.h"
#include "src/trace_processor/sched_slice_table.h"
#include "src/trace_processor/slice_table.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/span_join_operator_table.h"
#include "src/trace_processor/sql_stats_table.h"
#include "src/trace_processor/sqlite/sqlite3_str_split.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/stack_profile_callsite_table.h"
#include "src/trace_processor/stack_profile_frame_table.h"
#include "src/trace_processor/stack_profile_mapping_table.h"
#include "src/trace_processor/stack_profile_tracker.h"
#include "src/trace_processor/stats_table.h"
#include "src/trace_processor/syscall_tracker.h"
#include "src/trace_processor/systrace_parser.h"
#include "src/trace_processor/systrace_trace_parser.h"
#include "src/trace_processor/thread_table.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/track_table.h"
#include "src/trace_processor/virtual_track_tracker.h"
#include "src/trace_processor/window_operator_table.h"

#include "perfetto/metrics/android/mem_metric.pbzero.h"
#include "perfetto/metrics/metrics.pbzero.h"

// JSON parsing and exporting is only supported in the standalone and
// Chromium builds.
#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD) || \
    PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
#include "src/trace_processor/export_json.h"
#endif

// In Android and Chromium tree builds, we don't have the percentile module.
// Just don't include it.
#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
// defined in sqlite_src/ext/misc/percentile.c
extern "C" int sqlite3_percentile_init(sqlite3* db,
                                       char** error,
                                       const sqlite3_api_routines* api);
#endif

namespace perfetto {
namespace trace_processor {
namespace {

void InitializeSqlite(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db, "PRAGMA temp_store=2", 0, 0, &error);
  if (error) {
    PERFETTO_FATAL("Error setting pragma temp_store: %s", error);
  }
  sqlite3_str_split_init(db);
// In Android tree builds, we don't have the percentile module.
// Just don't include it.
#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
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
               "CREATE VIEW counters AS "
               "SELECT * FROM counter_values "
               "INNER JOIN counter_definitions USING(counter_id);",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  sqlite3_exec(db,
               "CREATE VIEW slice AS "
               "SELECT "
               "  *, "
               "  category as cat, "
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

// Exporting traces in legacy JSON format is only supported
// in the standalone and Chromium builds so far.
#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD) || \
    PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
void ExportJson(sqlite3_context* ctx, int /*argc*/, sqlite3_value** argv) {
  TraceStorage* storage = static_cast<TraceStorage*>(sqlite3_user_data(ctx));
  const char* filename =
      reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
  FILE* output = fopen(filename, "w");
  if (!output) {
    sqlite3_result_error(ctx, "Couldn't open output file", -1);
    return;
  }

  json::ResultCode result = json::ExportJson(storage, output);
  switch (result) {
    case json::kResultOk:
      return;
    case json::kResultWrongRefType:
      sqlite3_result_error(ctx, "Encountered a slice with unsupported ref type",
                           -1);
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

}  // namespace

TraceProcessorImpl::TraceProcessorImpl(const Config& cfg) {
  sqlite3* db = nullptr;
  PERFETTO_CHECK(sqlite3_initialize() == SQLITE_OK);
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  InitializeSqlite(db);
  CreateBuiltinTables(db);
  CreateBuiltinViews(db);
  db_.reset(std::move(db));

  context_.config = cfg;
  context_.storage.reset(new TraceStorage());
  context_.virtual_track_tracker.reset(new VirtualTrackTracker(&context_));
  context_.args_tracker.reset(new ArgsTracker(&context_));
  context_.slice_tracker.reset(new SliceTracker(&context_));
  context_.event_tracker.reset(new EventTracker(&context_));
  context_.process_tracker.reset(new ProcessTracker(&context_));
  context_.syscall_tracker.reset(new SyscallTracker(&context_));
  context_.clock_tracker.reset(new ClockTracker(&context_));
  context_.stack_profile_tracker.reset(new StackProfileTracker(&context_));
  context_.heap_profile_tracker.reset(new HeapProfileTracker(&context_));
  context_.systrace_parser.reset(new SystraceParser(&context_));

#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD) || \
    PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
  CreateJsonExportFunction(this->context_.storage.get(), db);
#endif

  SetupMetrics(this, *db_, &sql_metrics_);

  ArgsTable::RegisterTable(*db_, context_.storage.get());
  ProcessTable::RegisterTable(*db_, context_.storage.get());
  SchedSliceTable::RegisterTable(*db_, context_.storage.get());
  SliceTable::RegisterTable(*db_, context_.storage.get());
  SqlStatsTable::RegisterTable(*db_, context_.storage.get());
  ThreadTable::RegisterTable(*db_, context_.storage.get());
  CounterDefinitionsTable::RegisterTable(*db_, context_.storage.get());
  CounterValuesTable::RegisterTable(*db_, context_.storage.get());
  SpanJoinOperatorTable::RegisterTable(*db_, context_.storage.get());
  WindowOperatorTable::RegisterTable(*db_, context_.storage.get());
  InstantsTable::RegisterTable(*db_, context_.storage.get());
  StatsTable::RegisterTable(*db_, context_.storage.get());
  AndroidLogsTable::RegisterTable(*db_, context_.storage.get());
  RawTable::RegisterTable(*db_, context_.storage.get());
  HeapProfileAllocationTable::RegisterTable(*db_, context_.storage.get());
  StackProfileCallsiteTable::RegisterTable(*db_, context_.storage.get());
  StackProfileFrameTable::RegisterTable(*db_, context_.storage.get());
  StackProfileMappingTable::RegisterTable(*db_, context_.storage.get());
  MetadataTable::RegisterTable(*db_, context_.storage.get());
  TrackTable::RegisterTable(*db_, context_.storage.get());
}

TraceProcessorImpl::~TraceProcessorImpl() {
  for (auto* it : iterators_)
    it->Reset();
}

util::Status TraceProcessorImpl::Parse(std::unique_ptr<uint8_t[]> data,
                                       size_t size) {
  if (size == 0)
    return util::OkStatus();
  if (unrecoverable_parse_error_)
    return util::ErrStatus(
        "Failed unrecoverably while parsing in a previous Parse call");
  if (!context_.chunk_reader)
    context_.chunk_reader.reset(new ForwardingTraceParser(&context_));

  auto scoped_trace = context_.storage->TraceExecutionTimeIntoStats(
      stats::parse_trace_duration_ns);
  util::Status status = context_.chunk_reader->Parse(std::move(data), size);
  unrecoverable_parse_error_ |= !status.ok();
  return status;
}

void TraceProcessorImpl::NotifyEndOfFile() {
  if (unrecoverable_parse_error_ || !context_.chunk_reader)
    return;

  if (context_.sorter)
    context_.sorter->ExtractEventsForced();
  context_.event_tracker->FlushPendingEvents();
  context_.slice_tracker->FlushPendingSlices();
  BuildBoundsTable(*db_, context_.storage->GetTraceTimestampBoundsNs());
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
