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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/importers/android_bugreport/android_bugreport_parser.h"
#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_tokenizer.h"
#include "src/trace_processor/importers/gzip/gzip_trace_parser.h"
#include "src/trace_processor/importers/json/json_trace_parser.h"
#include "src/trace_processor/importers/json/json_trace_tokenizer.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/importers/ninja/ninja_log_parser.h"
#include "src/trace_processor/importers/perf/perf_data_parser.h"
#include "src/trace_processor/importers/perf/perf_data_tokenizer.h"
#include "src/trace_processor/importers/perf/perf_data_tracker.h"
#include "src/trace_processor/importers/proto/additional_modules.h"
#include "src/trace_processor/importers/proto/content_analyzer.h"
#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"
#include "src/trace_processor/iterator_impl.h"
#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/metrics/sql/amalgamated_sql_metrics.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/clock_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/create_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/create_view_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/layout_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/math.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/pprof_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/sql_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/sqlite3_str_split.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/stack_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/to_ftrace.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/utils.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/window_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/operators/span_join_operator.h"
#include "src/trace_processor/perfetto_sql/intrinsics/operators/window_operator.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/ancestor.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/connected_flow.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/descendant.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_annotated_stack.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_counter_dur.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flamegraph.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flat_slice.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_sched_upid.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_slice_layout.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/view.h"
#include "src/trace_processor/perfetto_sql/prelude/tables_views.h"
#include "src/trace_processor/perfetto_sql/stdlib/stdlib.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sql_stats_table.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/sqlite/stats_table.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/protozero_to_json.h"
#include "src/trace_processor/util/protozero_to_text.h"
#include "src/trace_processor/util/regex.h"
#include "src/trace_processor/util/sql_modules.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/perfetto/perfetto_metatrace.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#include "src/trace_processor/metrics/all_chrome_metrics.descriptor.h"
#include "src/trace_processor/metrics/all_webview_metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.descriptor.h"

namespace perfetto {
namespace trace_processor {
namespace {

const char kAllTablesQuery[] =
    "SELECT tbl_name, type FROM (SELECT * FROM sqlite_master UNION ALL SELECT "
    "* FROM sqlite_temp_master)";

template <typename SqlFunction, typename Ptr = typename SqlFunction::Context*>
void RegisterFunction(PerfettoSqlEngine* engine,
                      const char* name,
                      int argc,
                      Ptr context = nullptr,
                      bool deterministic = true) {
  auto status = engine->RegisterStaticFunction<SqlFunction>(
      name, argc, std::move(context), deterministic);
  if (!status.ok())
    PERFETTO_ELOG("%s", status.c_message());
}

void BuildBoundsTable(sqlite3* db, std::pair<int64_t, int64_t> bounds) {
  char* error = nullptr;
  sqlite3_exec(db, "DELETE FROM trace_bounds", nullptr, nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error deleting from bounds table: %s", error);
    sqlite3_free(error);
    return;
  }

  base::StackString<1024> sql("INSERT INTO trace_bounds VALUES(%" PRId64
                              ", %" PRId64 ")",
                              bounds.first, bounds.second);
  sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error inserting bounds table: %s", error);
    sqlite3_free(error);
  }
}

struct ValueAtMaxTsContext {
  bool initialized;
  int value_type;

  int64_t max_ts;
  int64_t int_value_at_max_ts;
  double double_value_at_max_ts;
};

void ValueAtMaxTsStep(sqlite3_context* ctx, int, sqlite3_value** argv) {
  sqlite3_value* ts = argv[0];
  sqlite3_value* value = argv[1];

  // Note that sqlite3_aggregate_context zeros the memory for us so all the
  // variables of the struct should be zero.
  ValueAtMaxTsContext* fn_ctx = reinterpret_cast<ValueAtMaxTsContext*>(
      sqlite3_aggregate_context(ctx, sizeof(ValueAtMaxTsContext)));

  // For performance reasons, we only do the check for the type of ts and value
  // on the first call of the function.
  if (PERFETTO_UNLIKELY(!fn_ctx->initialized)) {
    if (sqlite3_value_type(ts) != SQLITE_INTEGER) {
      sqlite3_result_error(ctx, "VALUE_AT_MAX_TS: ts passed was not an integer",
                           -1);
      return;
    }

    fn_ctx->value_type = sqlite3_value_type(value);
    if (fn_ctx->value_type != SQLITE_INTEGER &&
        fn_ctx->value_type != SQLITE_FLOAT) {
      sqlite3_result_error(
          ctx, "VALUE_AT_MAX_TS: value passed was not an integer or float", -1);
      return;
    }

    fn_ctx->max_ts = std::numeric_limits<int64_t>::min();
    fn_ctx->initialized = true;
  }

  // On dcheck builds however, we check every passed ts and value.
#if PERFETTO_DCHECK_IS_ON()
  if (sqlite3_value_type(ts) != SQLITE_INTEGER) {
    sqlite3_result_error(ctx, "VALUE_AT_MAX_TS: ts passed was not an integer",
                         -1);
    return;
  }
  if (sqlite3_value_type(value) != fn_ctx->value_type) {
    sqlite3_result_error(ctx, "VALUE_AT_MAX_TS: value type is inconsistent",
                         -1);
    return;
  }
#endif

  int64_t ts_int = sqlite3_value_int64(ts);
  if (PERFETTO_LIKELY(fn_ctx->max_ts <= ts_int)) {
    fn_ctx->max_ts = ts_int;

    if (fn_ctx->value_type == SQLITE_INTEGER) {
      fn_ctx->int_value_at_max_ts = sqlite3_value_int64(value);
    } else {
      fn_ctx->double_value_at_max_ts = sqlite3_value_double(value);
    }
  }
}

void ValueAtMaxTsFinal(sqlite3_context* ctx) {
  ValueAtMaxTsContext* fn_ctx =
      reinterpret_cast<ValueAtMaxTsContext*>(sqlite3_aggregate_context(ctx, 0));
  if (!fn_ctx) {
    sqlite3_result_null(ctx);
    return;
  }
  if (fn_ctx->value_type == SQLITE_INTEGER) {
    sqlite3_result_int64(ctx, fn_ctx->int_value_at_max_ts);
  } else {
    sqlite3_result_double(ctx, fn_ctx->double_value_at_max_ts);
  }
}

void RegisterValueAtMaxTsFunction(sqlite3* db) {
  auto ret = sqlite3_create_function_v2(
      db, "VALUE_AT_MAX_TS", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
      nullptr, &ValueAtMaxTsStep, &ValueAtMaxTsFinal, nullptr);
  if (ret) {
    PERFETTO_ELOG("Error initializing VALUE_AT_MAX_TS");
  }
}

std::vector<std::string> SanitizeMetricMountPaths(
    const std::vector<std::string>& mount_paths) {
  std::vector<std::string> sanitized;
  for (const auto& path : mount_paths) {
    if (path.length() == 0)
      continue;
    sanitized.push_back(path);
    if (path.back() != '/')
      sanitized.back().append("/");
  }
  return sanitized;
}

void SetupMetrics(TraceProcessor* tp,
                  PerfettoSqlEngine* engine,
                  std::vector<metrics::SqlMetricFile>* sql_metrics,
                  const std::vector<std::string>& extension_paths) {
  const std::vector<std::string> sanitized_extension_paths =
      SanitizeMetricMountPaths(extension_paths);
  std::vector<std::string> skip_prefixes;
  skip_prefixes.reserve(sanitized_extension_paths.size());
  for (const auto& path : sanitized_extension_paths) {
    skip_prefixes.push_back(kMetricProtoRoot + path);
  }
  tp->ExtendMetricsProto(kMetricsDescriptor.data(), kMetricsDescriptor.size(),
                         skip_prefixes);
  tp->ExtendMetricsProto(kAllChromeMetricsDescriptor.data(),
                         kAllChromeMetricsDescriptor.size(), skip_prefixes);
  tp->ExtendMetricsProto(kAllWebviewMetricsDescriptor.data(),
                         kAllWebviewMetricsDescriptor.size(), skip_prefixes);

  // TODO(lalitm): remove this special casing and change
  // SanitizeMetricMountPaths if/when we move all protos for builtin metrics to
  // match extension protos.
  bool skip_all_sql = std::find(extension_paths.begin(), extension_paths.end(),
                                "") != extension_paths.end();
  if (!skip_all_sql) {
    for (const auto& file_to_sql : sql_metrics::kFileToSql) {
      if (base::StartsWithAny(file_to_sql.path, sanitized_extension_paths))
        continue;
      tp->RegisterMetric(file_to_sql.path, file_to_sql.sql);
    }
  }

  RegisterFunction<metrics::NullIfEmpty>(engine, "NULL_IF_EMPTY", 1);
  RegisterFunction<metrics::UnwrapMetricProto>(engine, "UNWRAP_METRIC_PROTO",
                                               2);
  RegisterFunction<metrics::RunMetric>(
      engine, "RUN_METRIC", -1,
      std::unique_ptr<metrics::RunMetric::Context>(
          new metrics::RunMetric::Context{engine, sql_metrics}));

  // TODO(lalitm): migrate this over to using RegisterFunction once aggregate
  // functions are supported.
  {
    auto ret = sqlite3_create_function_v2(
        engine->sqlite_engine()->db(), "RepeatedField", 1, SQLITE_UTF8, nullptr,
        nullptr, metrics::RepeatedFieldStep, metrics::RepeatedFieldFinal,
        nullptr);
    if (ret)
      PERFETTO_FATAL("Error initializing RepeatedField");
  }
}

void InsertIntoTraceMetricsTable(sqlite3* db, const std::string& metric_name) {
  char* insert_sql = sqlite3_mprintf(
      "INSERT INTO trace_metrics(name) VALUES('%q')", metric_name.c_str());
  char* insert_error = nullptr;
  sqlite3_exec(db, insert_sql, nullptr, nullptr, &insert_error);
  sqlite3_free(insert_sql);
  if (insert_error) {
    PERFETTO_ELOG("Error registering table: %s", insert_error);
    sqlite3_free(insert_error);
  }
}

const char* TraceTypeToString(TraceType trace_type) {
  switch (trace_type) {
    case kUnknownTraceType:
      return "unknown";
    case kProtoTraceType:
      return "proto";
    case kJsonTraceType:
      return "json";
    case kFuchsiaTraceType:
      return "fuchsia";
    case kSystraceTraceType:
      return "systrace";
    case kGzipTraceType:
      return "gzip";
    case kCtraceTraceType:
      return "ctrace";
    case kNinjaLogTraceType:
      return "ninja_log";
    case kAndroidBugreportTraceType:
      return "android_bugreport";
    case kPerfDataTraceType:
      return "perf_data";
  }
  PERFETTO_FATAL("For GCC");
}

// Register SQL functions only used in local development instances.
void RegisterDevFunctions(PerfettoSqlEngine* engine) {
  RegisterFunction<WriteFile>(engine, "WRITE_FILE", 2);
}

sql_modules::NameToModule GetStdlibModules() {
  sql_modules::NameToModule modules;
  for (const auto& file_to_sql : stdlib::kFileToSql) {
    std::string import_key = sql_modules::GetIncludeKey(file_to_sql.path);
    std::string module = sql_modules::GetModuleName(import_key);
    modules.Insert(module, {}).first->push_back({import_key, file_to_sql.sql});
  }
  return modules;
}

void InitializePreludeTablesViews(sqlite3* db) {
  for (const auto& file_to_sql : prelude::tables_views::kFileToSql) {
    char* errmsg_raw = nullptr;
    int err = sqlite3_exec(db, file_to_sql.sql, nullptr, nullptr, &errmsg_raw);
    ScopedSqliteString errmsg(errmsg_raw);
    if (err != SQLITE_OK) {
      PERFETTO_FATAL("Failed to initialize prelude %s", errmsg_raw);
    }
  }
}

}  // namespace

template <typename View>
void TraceProcessorImpl::RegisterView(const View& view) {
  RegisterStaticTableFunction(std::unique_ptr<StaticTableFunction>(
      new ViewStaticTableFunction(&view, View::Name())));
}

TraceProcessorImpl::TraceProcessorImpl(const Config& cfg)
    : TraceProcessorStorageImpl(cfg),
      engine_(context_.storage->mutable_string_pool()) {
  context_.fuchsia_trace_tokenizer.reset(new FuchsiaTraceTokenizer(&context_));
  context_.fuchsia_trace_parser.reset(new FuchsiaTraceParser(&context_));
  context_.ninja_log_parser.reset(new NinjaLogParser(&context_));
  context_.systrace_trace_parser.reset(new SystraceTraceParser(&context_));
  context_.perf_data_trace_tokenizer.reset(
      new perf_importer::PerfDataTokenizer(&context_));
  context_.perf_data_parser.reset(new perf_importer::PerfDataParser(&context_));

  if (util::IsGzipSupported()) {
    context_.gzip_trace_parser.reset(new GzipTraceParser(&context_));
    context_.android_bugreport_parser.reset(
        new AndroidBugreportParser(&context_));
  }

  if (json::IsJsonSupported()) {
    context_.json_trace_tokenizer.reset(new JsonTraceTokenizer(&context_));
    context_.json_trace_parser.reset(new JsonTraceParser(&context_));
  }

  if (context_.config.analyze_trace_proto_content) {
    context_.content_analyzer.reset(new ProtoContentAnalyzer(&context_));
  }

  auto v2 = context_.config.dev_flags.find("enable_db2_filtering");
  if (v2 != context_.config.dev_flags.end()) {
    if (v2->second == "true") {
      Table::kUseFilterV2 = true;
    } else if (v2->second == "false") {
      Table::kUseFilterV2 = false;
    } else {
      PERFETTO_ELOG("Unknown value for enable_db2_filtering %s",
                    v2->second.c_str());
    }
  }

  sqlite3_str_split_init(engine_.sqlite_engine()->db());
  RegisterAdditionalModules(&context_);

  // New style function registration.
  if (cfg.enable_dev_features) {
    RegisterDevFunctions(&engine_);
  }
  RegisterFunction<Glob>(&engine_, "glob", 2);
  RegisterFunction<Hash>(&engine_, "HASH", -1);
  RegisterFunction<Base64Encode>(&engine_, "BASE64_ENCODE", 1);
  RegisterFunction<Demangle>(&engine_, "DEMANGLE", 1);
  RegisterFunction<SourceGeq>(&engine_, "SOURCE_GEQ", -1);
  RegisterFunction<ExportJson>(&engine_, "EXPORT_JSON", 1,
                               context_.storage.get(), false);
  RegisterFunction<ExtractArg>(&engine_, "EXTRACT_ARG", 2,
                               context_.storage.get());
  RegisterFunction<AbsTimeStr>(&engine_, "ABS_TIME_STR", 1,
                               context_.clock_converter.get());
  RegisterFunction<Reverse>(&engine_, "REVERSE", 1);
  RegisterFunction<ToMonotonic>(&engine_, "TO_MONOTONIC", 1,
                                context_.clock_converter.get());
  RegisterFunction<ToRealtime>(&engine_, "TO_REALTIME", 1,
                               context_.clock_converter.get());
  RegisterFunction<ToTimecode>(&engine_, "TO_TIMECODE", 1);
  RegisterFunction<CreateFunction>(&engine_, "CREATE_FUNCTION", 3, &engine_);
  RegisterFunction<CreateViewFunction>(&engine_, "CREATE_VIEW_FUNCTION", 3,
                                       &engine_);
  RegisterFunction<ExperimentalMemoize>(&engine_, "EXPERIMENTAL_MEMOIZE", 1,
                                        &engine_);
  RegisterFunction<Import>(
      &engine_, "IMPORT", 1,
      std::unique_ptr<Import::Context>(new Import::Context{&engine_}));
  RegisterFunction<ToFtrace>(
      &engine_, "TO_FTRACE", 1,
      std::unique_ptr<ToFtrace::Context>(new ToFtrace::Context{
          context_.storage.get(), SystraceSerializer(&context_)}));

  if constexpr (regex::IsRegexSupported()) {
    RegisterFunction<Regex>(&engine_, "regexp", 2);
  }
  // Old style function registration.
  // TODO(lalitm): migrate this over to using RegisterFunction once aggregate
  // functions are supported.
  RegisterLastNonNullFunction(engine_.sqlite_engine()->db());
  RegisterValueAtMaxTsFunction(engine_.sqlite_engine()->db());
  {
    base::Status status = RegisterStackFunctions(&engine_, &context_);
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }
  {
    base::Status status =
        PprofFunctions::Register(engine_.sqlite_engine()->db(), &context_);
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }
  {
    base::Status status =
        LayoutFunctions::Register(engine_.sqlite_engine()->db(), &context_);
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }
  {
    base::Status status = RegisterMathFunctions(engine_);
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }

  const TraceStorage* storage = context_.storage.get();

  // Operator tables.
  engine_.sqlite_engine()->RegisterVirtualTableModule<SpanJoinOperatorTable>(
      "span_join", &engine_, SqliteTable::TableType::kExplicitCreate, false);
  engine_.sqlite_engine()->RegisterVirtualTableModule<SpanJoinOperatorTable>(
      "span_left_join", &engine_, SqliteTable::TableType::kExplicitCreate,
      false);
  engine_.sqlite_engine()->RegisterVirtualTableModule<SpanJoinOperatorTable>(
      "span_outer_join", &engine_, SqliteTable::TableType::kExplicitCreate,
      false);
  engine_.sqlite_engine()->RegisterVirtualTableModule<WindowOperatorTable>(
      "window", storage, SqliteTable::TableType::kExplicitCreate, true);

  // Initalize the tables and views in the prelude.
  InitializePreludeTablesViews(engine_.sqlite_engine()->db());

  auto stdlib_modules = GetStdlibModules();
  for (auto module_it = stdlib_modules.GetIterator(); module_it; ++module_it) {
    base::Status status =
        RegisterSqlModule({module_it.key(), module_it.value(), false});
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }

  SetupMetrics(this, &engine_, &sql_metrics_, cfg.skip_builtin_metric_paths);

  // Legacy tables.
  engine_.sqlite_engine()->RegisterVirtualTableModule<SqlStatsTable>(
      "sqlstats", storage, SqliteTable::TableType::kEponymousOnly, false);
  engine_.sqlite_engine()->RegisterVirtualTableModule<StatsTable>(
      "stats", storage, SqliteTable::TableType::kEponymousOnly, false);

  // Tables dynamically generated at query time.
  RegisterStaticTableFunction(std::unique_ptr<ExperimentalFlamegraph>(
      new ExperimentalFlamegraph(&context_)));
  RegisterStaticTableFunction(std::unique_ptr<ExperimentalCounterDur>(
      new ExperimentalCounterDur(storage->counter_table())));
  RegisterStaticTableFunction(std::unique_ptr<ExperimentalSliceLayout>(
      new ExperimentalSliceLayout(context_.storage.get()->mutable_string_pool(),
                                  &storage->slice_table())));
  RegisterStaticTableFunction(std::unique_ptr<Ancestor>(
      new Ancestor(Ancestor::Type::kSlice, context_.storage.get())));
  RegisterStaticTableFunction(std::unique_ptr<Ancestor>(new Ancestor(
      Ancestor::Type::kStackProfileCallsite, context_.storage.get())));
  RegisterStaticTableFunction(std::unique_ptr<Ancestor>(
      new Ancestor(Ancestor::Type::kSliceByStack, context_.storage.get())));
  RegisterStaticTableFunction(std::unique_ptr<Descendant>(
      new Descendant(Descendant::Type::kSlice, context_.storage.get())));
  RegisterStaticTableFunction(std::unique_ptr<Descendant>(
      new Descendant(Descendant::Type::kSliceByStack, context_.storage.get())));
  RegisterStaticTableFunction(std::unique_ptr<ConnectedFlow>(new ConnectedFlow(
      ConnectedFlow::Mode::kDirectlyConnectedFlow, context_.storage.get())));
  RegisterStaticTableFunction(std::unique_ptr<ConnectedFlow>(new ConnectedFlow(
      ConnectedFlow::Mode::kPrecedingFlow, context_.storage.get())));
  RegisterStaticTableFunction(std::unique_ptr<ConnectedFlow>(new ConnectedFlow(
      ConnectedFlow::Mode::kFollowingFlow, context_.storage.get())));
  RegisterStaticTableFunction(
      std::unique_ptr<ExperimentalSchedUpid>(new ExperimentalSchedUpid(
          storage->sched_slice_table(), storage->thread_table())));
  RegisterStaticTableFunction(std::unique_ptr<ExperimentalAnnotatedStack>(
      new ExperimentalAnnotatedStack(&context_)));
  RegisterStaticTableFunction(std::unique_ptr<ExperimentalFlatSlice>(
      new ExperimentalFlatSlice(&context_)));

  // Views.
  RegisterView(storage->thread_slice_view());

  // New style db-backed tables.
  // Note: if adding a table here which might potentially contain many rows
  // (O(rows in sched/slice/counter)), then consider calling ShrinkToFit on
  // that table in TraceStorage::ShrinkToFitTables.
  RegisterStaticTable(storage->arg_table());
  RegisterStaticTable(storage->raw_table());
  RegisterStaticTable(storage->ftrace_event_table());
  RegisterStaticTable(storage->thread_table());
  RegisterStaticTable(storage->process_table());
  RegisterStaticTable(storage->filedescriptor_table());

  RegisterStaticTable(storage->slice_table());
  RegisterStaticTable(storage->flow_table());
  RegisterStaticTable(storage->slice_table());
  RegisterStaticTable(storage->sched_slice_table());
  RegisterStaticTable(storage->spurious_sched_wakeup_table());
  RegisterStaticTable(storage->thread_state_table());
  RegisterStaticTable(storage->gpu_slice_table());

  RegisterStaticTable(storage->track_table());
  RegisterStaticTable(storage->thread_track_table());
  RegisterStaticTable(storage->process_track_table());
  RegisterStaticTable(storage->cpu_track_table());
  RegisterStaticTable(storage->gpu_track_table());

  RegisterStaticTable(storage->counter_table());

  RegisterStaticTable(storage->counter_track_table());
  RegisterStaticTable(storage->process_counter_track_table());
  RegisterStaticTable(storage->thread_counter_track_table());
  RegisterStaticTable(storage->cpu_counter_track_table());
  RegisterStaticTable(storage->irq_counter_track_table());
  RegisterStaticTable(storage->softirq_counter_track_table());
  RegisterStaticTable(storage->gpu_counter_track_table());
  RegisterStaticTable(storage->gpu_counter_group_table());
  RegisterStaticTable(storage->perf_counter_track_table());
  RegisterStaticTable(storage->energy_counter_track_table());
  RegisterStaticTable(storage->uid_counter_track_table());
  RegisterStaticTable(storage->energy_per_uid_counter_track_table());

  RegisterStaticTable(storage->heap_graph_object_table());
  RegisterStaticTable(storage->heap_graph_reference_table());
  RegisterStaticTable(storage->heap_graph_class_table());

  RegisterStaticTable(storage->symbol_table());
  RegisterStaticTable(storage->heap_profile_allocation_table());
  RegisterStaticTable(storage->cpu_profile_stack_sample_table());
  RegisterStaticTable(storage->perf_sample_table());
  RegisterStaticTable(storage->stack_profile_callsite_table());
  RegisterStaticTable(storage->stack_profile_mapping_table());
  RegisterStaticTable(storage->stack_profile_frame_table());
  RegisterStaticTable(storage->package_list_table());
  RegisterStaticTable(storage->profiler_smaps_table());

  RegisterStaticTable(storage->android_log_table());
  RegisterStaticTable(storage->android_dumpstate_table());
  RegisterStaticTable(storage->android_game_intervention_list_table());

  RegisterStaticTable(storage->vulkan_memory_allocations_table());

  RegisterStaticTable(storage->graphics_frame_slice_table());

  RegisterStaticTable(storage->expected_frame_timeline_slice_table());
  RegisterStaticTable(storage->actual_frame_timeline_slice_table());

  RegisterStaticTable(storage->surfaceflinger_layers_snapshot_table());
  RegisterStaticTable(storage->surfaceflinger_layer_table());
  RegisterStaticTable(storage->surfaceflinger_transactions_table());

  RegisterStaticTable(storage->metadata_table());
  RegisterStaticTable(storage->cpu_table());
  RegisterStaticTable(storage->cpu_freq_table());
  RegisterStaticTable(storage->clock_snapshot_table());

  RegisterStaticTable(storage->memory_snapshot_table());
  RegisterStaticTable(storage->process_memory_snapshot_table());
  RegisterStaticTable(storage->memory_snapshot_node_table());
  RegisterStaticTable(storage->memory_snapshot_edge_table());

  RegisterStaticTable(storage->experimental_proto_path_table());
  RegisterStaticTable(storage->experimental_proto_content_table());

  RegisterStaticTable(storage->experimental_missing_chrome_processes_table());
}

TraceProcessorImpl::~TraceProcessorImpl() = default;

base::Status TraceProcessorImpl::Parse(TraceBlobView blob) {
  bytes_parsed_ += blob.size();
  return TraceProcessorStorageImpl::Parse(std::move(blob));
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

void TraceProcessorImpl::Flush() {
  TraceProcessorStorageImpl::Flush();

  context_.metadata_tracker->SetMetadata(
      metadata::trace_size_bytes,
      Variadic::Integer(static_cast<int64_t>(bytes_parsed_)));
  const StringId trace_type_id =
      context_.storage->InternString(TraceTypeToString(context_.trace_type));
  context_.metadata_tracker->SetMetadata(metadata::trace_type,
                                         Variadic::String(trace_type_id));
  BuildBoundsTable(engine_.sqlite_engine()->db(),
                   context_.storage->GetTraceTimestampBoundsNs());
}

void TraceProcessorImpl::NotifyEndOfFile() {
  if (notify_eof_called_) {
    PERFETTO_ELOG(
        "NotifyEndOfFile should only be called once. Try calling Flush instead "
        "if trying to commit the contents of the trace to tables.");
    return;
  }
  notify_eof_called_ = true;

  if (current_trace_name_.empty())
    current_trace_name_ = "Unnamed trace";

  // Last opportunity to flush all pending data.
  Flush();

  TraceProcessorStorageImpl::NotifyEndOfFile();

  // Create a snapshot list of all tables and views created so far. This is so
  // later we can drop all extra tables created by the UI and reset to the
  // original state (see RestoreInitialTables).
  initial_tables_.clear();
  auto it = ExecuteQuery(kAllTablesQuery);
  while (it.Next()) {
    auto value = it.Get(0);
    PERFETTO_CHECK(value.type == SqlValue::Type::kString);
    initial_tables_.push_back(value.string_value);
  }

  context_.storage->ShrinkToFitTables();

  // Rebuild the bounds table once everything has been completed: we do this
  // so that if any data was added to tables in
  // TraceProcessorStorageImpl::NotifyEndOfFile, this will be counted in
  // trace bounds: this is important for parsers like ninja which wait until
  // the end to flush all their data.
  BuildBoundsTable(engine_.sqlite_engine()->db(),
                   context_.storage->GetTraceTimestampBoundsNs());

  TraceProcessorStorageImpl::DestroyContext();
}

size_t TraceProcessorImpl::RestoreInitialTables() {
  // Step 1: figure out what tables/views/indices we need to delete.
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

  // Step 2: actually delete those tables/views/indices.
  for (const auto& tn : deletion_list) {
    std::string query = "DROP " + tn.first + " " + tn.second;
    auto it = ExecuteQuery(query);
    while (it.Next()) {
    }
    // Index deletion can legitimately fail. If one creates an index "i" on a
    // table "t" but issues the deletion in the order (t, i), the DROP index i
    // will fail with "no such index" because deleting the table "t"
    // automatically deletes all associated indexes.
    if (!it.Status().ok() && tn.first != "index")
      PERFETTO_FATAL("%s -> %s", query.c_str(), it.Status().c_message());
  }
  return deletion_list.size();
}

Iterator TraceProcessorImpl::ExecuteQuery(const std::string& sql) {
  PERFETTO_TP_TRACE(metatrace::Category::API_TIMELINE, "EXECUTE_QUERY");

  uint32_t sql_stats_row =
      context_.storage->mutable_sql_stats()->RecordQueryBegin(
          sql, base::GetWallTimeNs().count());
  std::string non_breaking_sql = base::ReplaceAll(sql, "\u00A0", " ");
  base::StatusOr<PerfettoSqlEngine::ExecutionResult> result =
      engine_.ExecuteUntilLastStatement(
          SqlSource::FromExecuteQuery(std::move(non_breaking_sql)));
  std::unique_ptr<IteratorImpl> impl(
      new IteratorImpl(this, std::move(result), sql_stats_row));
  return Iterator(std::move(impl));
}

void TraceProcessorImpl::InterruptQuery() {
  if (!engine_.sqlite_engine()->db())
    return;
  query_interrupted_.store(true);
  sqlite3_interrupt(engine_.sqlite_engine()->db());
}

bool TraceProcessorImpl::IsRootMetricField(const std::string& metric_name) {
  std::optional<uint32_t> desc_idx =
      pool_.FindDescriptorIdx(".perfetto.protos.TraceMetrics");
  if (!desc_idx.has_value())
    return false;
  auto field_idx = pool_.descriptors()[*desc_idx].FindFieldByName(metric_name);
  return field_idx != nullptr;
}

base::Status TraceProcessorImpl::RegisterSqlModule(SqlModule sql_module) {
  sql_modules::RegisteredModule new_module;
  std::string name = sql_module.name;
  if (engine_.FindModule(name) && !sql_module.allow_module_override) {
    return base::ErrStatus(
        "Module '%s' is already registered. Choose a different name.\n"
        "If you want to replace the existing module using trace processor "
        "shell, you need to pass the --dev flag and use --override-sql-module "
        "to pass the module path.",
        name.c_str());
  }
  for (auto const& name_and_sql : sql_module.files) {
    if (sql_modules::GetModuleName(name_and_sql.first) != name) {
      return base::ErrStatus(
          "File import key doesn't match the module name. First part of import "
          "key should be module name. Import key: %s, module name: %s.",
          name_and_sql.first.c_str(), name.c_str());
    }
    new_module.include_key_to_file.Insert(name_and_sql.first,
                                          {name_and_sql.second, false});
  }
  engine_.RegisterModule(name, std::move(new_module));
  return base::OkStatus();
}

base::Status TraceProcessorImpl::RegisterMetric(const std::string& path,
                                                const std::string& sql) {
  // Check if the metric with the given path already exists and if it does,
  // just update the SQL associated with it.
  auto it = std::find_if(
      sql_metrics_.begin(), sql_metrics_.end(),
      [&path](const metrics::SqlMetricFile& m) { return m.path == path; });
  if (it != sql_metrics_.end()) {
    it->sql = sql;
    return base::OkStatus();
  }

  auto sep_idx = path.rfind('/');
  std::string basename =
      sep_idx == std::string::npos ? path : path.substr(sep_idx + 1);

  auto sql_idx = basename.rfind(".sql");
  if (sql_idx == std::string::npos) {
    return base::ErrStatus("Unable to find .sql extension for metric");
  }
  auto no_ext_name = basename.substr(0, sql_idx);

  metrics::SqlMetricFile metric;
  metric.path = path;
  metric.sql = sql;

  if (IsRootMetricField(no_ext_name)) {
    metric.proto_field_name = no_ext_name;
    metric.output_table_name = no_ext_name + "_output";

    auto field_it_and_inserted =
        proto_field_to_sql_metric_path_.emplace(*metric.proto_field_name, path);
    if (!field_it_and_inserted.second) {
      // We already had a metric with this field name in the map. However, if
      // this was the case, we should have found the metric in
      // |path_to_sql_metric_file_| above if we are simply overriding the
      // metric. Return an error since this means we have two different SQL
      // files which are trying to output the same metric.
      const auto& prev_path = field_it_and_inserted.first->second;
      PERFETTO_DCHECK(prev_path != path);
      return base::ErrStatus(
          "RegisterMetric Error: Metric paths %s (which is already "
          "registered) "
          "and %s are both trying to output the proto field %s",
          prev_path.c_str(), path.c_str(), metric.proto_field_name->c_str());
    }

    InsertIntoTraceMetricsTable(engine_.sqlite_engine()->db(), no_ext_name);
  }

  sql_metrics_.emplace_back(metric);
  return base::OkStatus();
}

base::Status TraceProcessorImpl::ExtendMetricsProto(const uint8_t* data,
                                                    size_t size) {
  return ExtendMetricsProto(data, size, /*skip_prefixes*/ {});
}

base::Status TraceProcessorImpl::ExtendMetricsProto(
    const uint8_t* data,
    size_t size,
    const std::vector<std::string>& skip_prefixes) {
  base::Status status =
      pool_.AddFromFileDescriptorSet(data, size, skip_prefixes);
  if (!status.ok())
    return status;

  for (uint32_t i = 0; i < pool_.descriptors().size(); ++i) {
    // Convert the full name (e.g. .perfetto.protos.TraceMetrics.SubMetric)
    // into a function name of the form (TraceMetrics_SubMetric).
    const auto& desc = pool_.descriptors()[i];
    auto fn_name = desc.full_name().substr(desc.package_name().size() + 1);
    std::replace(fn_name.begin(), fn_name.end(), '.', '_');
    RegisterFunction<metrics::BuildProto>(
        &engine_, fn_name.c_str(), -1,
        std::unique_ptr<metrics::BuildProto::Context>(
            new metrics::BuildProto::Context{this, &pool_, i}));
  }
  return base::OkStatus();
}

base::Status TraceProcessorImpl::ComputeMetric(
    const std::vector<std::string>& metric_names,
    std::vector<uint8_t>* metrics_proto) {
  auto opt_idx = pool_.FindDescriptorIdx(".perfetto.protos.TraceMetrics");
  if (!opt_idx.has_value())
    return base::Status("Root metrics proto descriptor not found");

  const auto& root_descriptor = pool_.descriptors()[opt_idx.value()];
  return metrics::ComputeMetrics(&engine_, metric_names, sql_metrics_, pool_,
                                 root_descriptor, metrics_proto);
}

base::Status TraceProcessorImpl::ComputeMetricText(
    const std::vector<std::string>& metric_names,
    TraceProcessor::MetricResultFormat format,
    std::string* metrics_string) {
  std::vector<uint8_t> metrics_proto;
  base::Status status = ComputeMetric(metric_names, &metrics_proto);
  if (!status.ok())
    return status;
  switch (format) {
    case TraceProcessor::MetricResultFormat::kProtoText:
      *metrics_string = protozero_to_text::ProtozeroToText(
          pool_, ".perfetto.protos.TraceMetrics",
          protozero::ConstBytes{metrics_proto.data(), metrics_proto.size()},
          protozero_to_text::kIncludeNewLines);
      break;
    case TraceProcessor::MetricResultFormat::kJson:
      *metrics_string = protozero_to_json::ProtozeroToJson(
          pool_, ".perfetto.protos.TraceMetrics",
          protozero::ConstBytes{metrics_proto.data(), metrics_proto.size()},
          protozero_to_json::kPretty | protozero_to_json::kInlineErrors |
              protozero_to_json::kInlineAnnotations);
      break;
  }
  return status;
}

std::vector<uint8_t> TraceProcessorImpl::GetMetricDescriptors() {
  return pool_.SerializeAsDescriptorSet();
}

void TraceProcessorImpl::EnableMetatrace(MetatraceConfig config) {
  metatrace::Enable(config);
}

namespace {

class StringInterner {
 public:
  StringInterner(protos::pbzero::PerfettoMetatrace& event,
                 base::FlatHashMap<std::string, uint64_t>& interned_strings)
      : event_(event), interned_strings_(interned_strings) {}

  ~StringInterner() {
    for (const auto& interned_string : new_interned_strings_) {
      auto* interned_string_proto = event_.add_interned_strings();
      interned_string_proto->set_iid(interned_string.first);
      interned_string_proto->set_value(interned_string.second);
    }
  }

  uint64_t InternString(const std::string& str) {
    uint64_t new_iid = interned_strings_.size();
    auto insert_result = interned_strings_.Insert(str, new_iid);
    if (insert_result.second) {
      new_interned_strings_.emplace_back(new_iid, str);
    }
    return *insert_result.first;
  }

 private:
  protos::pbzero::PerfettoMetatrace& event_;
  base::FlatHashMap<std::string, uint64_t>& interned_strings_;

  base::SmallVector<std::pair<uint64_t, std::string>, 16> new_interned_strings_;
};

}  // namespace

base::Status TraceProcessorImpl::DisableAndReadMetatrace(
    std::vector<uint8_t>* trace_proto) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;

  {
    uint64_t realtime_timestamp = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch() /
        std::chrono::nanoseconds(1));
    uint64_t boottime_timestamp = metatrace::TraceTimeNowNs();
    auto* clock_snapshot = trace->add_packet()->set_clock_snapshot();
    {
      auto* realtime_clock = clock_snapshot->add_clocks();
      realtime_clock->set_clock_id(
          protos::pbzero::BuiltinClock::BUILTIN_CLOCK_REALTIME);
      realtime_clock->set_timestamp(realtime_timestamp);
    }
    {
      auto* boottime_clock = clock_snapshot->add_clocks();
      boottime_clock->set_clock_id(
          protos::pbzero::BuiltinClock::BUILTIN_CLOCK_BOOTTIME);
      boottime_clock->set_timestamp(boottime_timestamp);
    }
  }

  base::FlatHashMap<std::string, uint64_t> interned_strings;
  metatrace::DisableAndReadBuffer([&trace, &interned_strings](
                                      metatrace::Record* record) {
    auto packet = trace->add_packet();
    packet->set_timestamp(record->timestamp_ns);
    auto* evt = packet->set_perfetto_metatrace();

    StringInterner interner(*evt, interned_strings);

    evt->set_event_name_iid(interner.InternString(record->event_name));
    evt->set_event_duration_ns(record->duration_ns);
    evt->set_thread_id(1);  // Not really important, just required for the ui.

    if (record->args_buffer_size == 0)
      return;

    base::StringSplitter s(
        record->args_buffer, record->args_buffer_size, '\0',
        base::StringSplitter::EmptyTokenMode::ALLOW_EMPTY_TOKENS);
    for (; s.Next();) {
      auto* arg_proto = evt->add_args();
      arg_proto->set_key_iid(interner.InternString(s.cur_token()));

      bool has_next = s.Next();
      PERFETTO_CHECK(has_next);
      arg_proto->set_value_iid(interner.InternString(s.cur_token()));
    }
  });
  *trace_proto = trace.SerializeAsArray();
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
