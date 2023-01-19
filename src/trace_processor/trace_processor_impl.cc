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
#include "src/trace_processor/dynamic/ancestor_generator.h"
#include "src/trace_processor/dynamic/connected_flow_generator.h"
#include "src/trace_processor/dynamic/descendant_generator.h"
#include "src/trace_processor/dynamic/experimental_annotated_stack_generator.h"
#include "src/trace_processor/dynamic/experimental_counter_dur_generator.h"
#include "src/trace_processor/dynamic/experimental_flamegraph_generator.h"
#include "src/trace_processor/dynamic/experimental_flat_slice_generator.h"
#include "src/trace_processor/dynamic/experimental_sched_upid_generator.h"
#include "src/trace_processor/dynamic/experimental_slice_layout_generator.h"
#include "src/trace_processor/dynamic/view_generator.h"
#include "src/trace_processor/importers/android_bugreport/android_bugreport_parser.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_tokenizer.h"
#include "src/trace_processor/importers/gzip/gzip_trace_parser.h"
#include "src/trace_processor/importers/json/json_trace_parser.h"
#include "src/trace_processor/importers/json/json_trace_tokenizer.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/importers/ninja/ninja_log_parser.h"
#include "src/trace_processor/importers/proto/additional_modules.h"
#include "src/trace_processor/importers/proto/content_analyzer.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"
#include "src/trace_processor/iterator_impl.h"
#include "src/trace_processor/prelude/functions/create_function.h"
#include "src/trace_processor/prelude/functions/create_view_function.h"
#include "src/trace_processor/prelude/functions/import.h"
#include "src/trace_processor/prelude/functions/pprof_functions.h"
#include "src/trace_processor/prelude/functions/register_function.h"
#include "src/trace_processor/prelude/functions/sqlite3_str_split.h"
#include "src/trace_processor/prelude/functions/utils.h"
#include "src/trace_processor/prelude/functions/window_functions.h"
#include "src/trace_processor/prelude/operators/span_join_operator.h"
#include "src/trace_processor/prelude/operators/window_operator.h"
#include "src/trace_processor/sqlite/scoped_db.h"
#include "src/trace_processor/sqlite/sql_stats_table.h"
#include "src/trace_processor/sqlite/sqlite_raw_table.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/sqlite/stats_table.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/protozero_to_text.h"
#include "src/trace_processor/util/sql_modules.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/perfetto/perfetto_metatrace.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#include "src/trace_processor/metrics/all_chrome_metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/metrics/sql/amalgamated_sql_metrics.h"
#include "src/trace_processor/stdlib/amalgamated_stdlib.h"

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

template <typename SqlFunction, typename Ptr = typename SqlFunction::Context*>
void RegisterFunction(sqlite3* db,
                      const char* name,
                      int argc,
                      Ptr context = nullptr,
                      bool deterministic = true) {
  auto status = RegisterSqlFunction<SqlFunction>(
      db, name, argc, std::move(context), deterministic);
  if (!status.ok())
    PERFETTO_ELOG("%s", status.c_message());
}

void InitializeSqlite(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db, "PRAGMA temp_store=2", nullptr, nullptr, &error);
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

  sqlite3_exec(db, insert_sql, nullptr, nullptr, &error);
  sqlite3_free(insert_sql);
  if (error) {
    PERFETTO_ELOG("Error inserting bounds table: %s", error);
    sqlite3_free(error);
  }
}

void CreateBuiltinTables(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db, "CREATE TABLE perfetto_tables(name STRING)", nullptr,
               nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
  sqlite3_exec(db, "CREATE TABLE trace_bounds(start_ts BIGINT, end_ts BIGINT)",
               nullptr, nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
  // Ensure that the entries in power_profile are unique to prevent duplicates
  // when the power_profile is augmented with additional profiles.
  sqlite3_exec(db,
               "CREATE TABLE power_profile("
               "device STRING, cpu INT, cluster INT, freq INT, power DOUBLE,"
               "UNIQUE(device, cpu, cluster, freq));",
               nullptr, nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
  sqlite3_exec(db, "CREATE TABLE trace_metrics(name STRING)", nullptr, nullptr,
               &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
  // This is a table intended to be used for metric debugging/developing. Data
  // in the table is shown specially in the UI, and users can insert rows into
  // this table to draw more things.
  sqlite3_exec(db,
               "CREATE TABLE debug_slices (id BIGINT, name STRING, ts BIGINT,"
               "dur BIGINT, depth BIGINT)",
               nullptr, nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  // Initialize the bounds table with some data so even before parsing any data,
  // we still have a valid table.
  BuildBoundsTable(db, std::make_pair(0, 0));
}

void MaybeRegisterError(char* error) {
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
}

void CreateBuiltinViews(sqlite3* db) {
  char* error = nullptr;
  sqlite3_exec(db,
               "CREATE VIEW counter_definitions AS "
               "SELECT "
               "  *, "
               "  id AS counter_id "
               "FROM counter_track",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  sqlite3_exec(db,
               "CREATE VIEW counter_values AS "
               "SELECT "
               "  *, "
               "  track_id as counter_id "
               "FROM counter",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  sqlite3_exec(db,
               "CREATE VIEW counters AS "
               "SELECT * "
               "FROM counter_values v "
               "INNER JOIN counter_track t "
               "ON v.track_id = t.id "
               "ORDER BY ts;",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  sqlite3_exec(db,
               "CREATE VIEW slice AS "
               "SELECT "
               "  *, "
               "  category AS cat, "
               "  id AS slice_id "
               "FROM internal_slice;",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  sqlite3_exec(db,
               "CREATE VIEW instant AS "
               "SELECT "
               "ts, track_id, name, arg_set_id "
               "FROM slice "
               "WHERE dur = 0;",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  sqlite3_exec(db,
               "CREATE VIEW sched AS "
               "SELECT "
               "*, "
               "ts + dur as ts_end "
               "FROM sched_slice;",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  // Legacy view for "slice" table with a deprecated table name.
  // TODO(eseckler): Remove this view when all users have switched to "slice".
  sqlite3_exec(db,
               "CREATE VIEW slices AS "
               "SELECT * FROM slice;",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  sqlite3_exec(db,
               "CREATE VIEW thread AS "
               "SELECT "
               "id as utid, "
               "* "
               "FROM internal_thread;",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  sqlite3_exec(db,
               "CREATE VIEW process AS "
               "SELECT "
               "id as upid, "
               "* "
               "FROM internal_process;",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  // This should be kept in sync with GlobalArgsTracker::AddArgSet.
  sqlite3_exec(db,
               "CREATE VIEW args AS "
               "SELECT "
               "*, "
               "CASE value_type "
               "  WHEN 'int' THEN CAST(int_value AS text) "
               "  WHEN 'uint' THEN CAST(int_value AS text) "
               "  WHEN 'string' THEN string_value "
               "  WHEN 'real' THEN CAST(real_value AS text) "
               "  WHEN 'pointer' THEN printf('0x%x', int_value) "
               "  WHEN 'bool' THEN ( "
               "    CASE WHEN int_value <> 0 THEN 'true' "
               "    ELSE 'false' END) "
               "  WHEN 'json' THEN string_value "
               "ELSE NULL END AS display_value "
               "FROM internal_args;",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  // TODO(lalitm): delete this any time after ~Feb 2023 when no version of the
  // UI will be querying this anymore (describe_slice backing code was removed
  // at end of November).
  sqlite3_exec(db,
               "CREATE TABLE describe_slice(id INT, type TEXT, "
               "slice_id INT, description TEXT, doc_link TEXT);",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);

  sqlite3_exec(db,
               "CREATE VIEW thread_slice AS "
               "SELECT * FROM slice "
               "WHERE thread_dur is NOT NULL",
               nullptr, nullptr, &error);
  MaybeRegisterError(error);
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
                  sqlite3* db,
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

  RegisterFunction<metrics::NullIfEmpty>(db, "NULL_IF_EMPTY", 1);
  RegisterFunction<metrics::UnwrapMetricProto>(db, "UNWRAP_METRIC_PROTO", 2);
  RegisterFunction<metrics::RunMetric>(
      db, "RUN_METRIC", -1,
      std::unique_ptr<metrics::RunMetric::Context>(
          new metrics::RunMetric::Context{tp, sql_metrics}));

  // TODO(lalitm): migrate this over to using RegisterFunction once aggregate
  // functions are supported.
  {
    auto ret = sqlite3_create_function_v2(
        db, "RepeatedField", 1, SQLITE_UTF8, nullptr, nullptr,
        metrics::RepeatedFieldStep, metrics::RepeatedFieldFinal, nullptr);
    if (ret)
      PERFETTO_FATAL("Error initializing RepeatedField");
  }
}

void EnsureSqliteInitialized() {
  // sqlite3_initialize isn't actually thread-safe despite being documented
  // as such; we need to make sure multiple TraceProcessorImpl instances don't
  // call it concurrently and only gets called once per process, instead.
  static bool init_once = [] { return sqlite3_initialize() == SQLITE_OK; }();
  PERFETTO_CHECK(init_once);
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

void IncrementCountForStmt(sqlite3_stmt* stmt,
                           IteratorImpl::StmtMetadata* metadata) {
  metadata->statement_count++;

  // If the stmt is already done, it clearly didn't have any output.
  if (sqlite_utils::IsStmtDone(stmt))
    return;

  if (sqlite3_column_count(stmt) == 1) {
    sqlite3_value* value = sqlite3_column_value(stmt, 0);

    // If the "VOID" pointer associated to the return value is not null,
    // that means this is a function which is forced to return a value
    // (because all functions in SQLite have to) but doesn't actually
    // wait to (i.e. it wants to be treated like CREATE TABLE or similar).
    // Because of this, ignore the return value of this function.
    // See |WrapSqlFunction| for where this is set.
    if (sqlite3_value_pointer(value, "VOID") != nullptr) {
      return;
    }

    // If the statement only has a single column and that column is named
    // "suppress_query_output", treat it as a statement without output for
    // accounting purposes. This allows an escape hatch for cases where the
    // user explicitly wants to ignore functions as having output.
    if (strcmp(sqlite3_column_name(stmt, 0), "suppress_query_output") == 0) {
      return;
    }
  }

  // Otherwise, the statement has output and so increment the count.
  metadata->statement_count_with_output++;
}

base::Status PrepareAndStepUntilLastValidStmt(
    sqlite3* db,
    const std::string& sql,
    ScopedStmt* output_stmt,
    IteratorImpl::StmtMetadata* metadata) {
  ScopedStmt prev_stmt;
  // A sql string can contain several statements. Some of them might be comment
  // only, e.g. "SELECT 1; /* comment */; SELECT 2;". Here we process one
  // statement on each iteration. SQLite's sqlite_prepare_v2 (wrapped by
  // PrepareStmt) returns on each iteration a pointer to the unprocessed string.
  //
  // Unfortunately we cannot call PrepareStmt and tokenize all statements
  // upfront because sqlite_prepare_v2 also semantically checks the statement
  // against the schema. In some cases statements might depend on the execution
  // of previous ones (e.e. CREATE VIEW x; SELECT FROM x; DELETE VIEW x;).
  //
  // Also, unfortunately, we need to PrepareStmt to find out if a statement is a
  // comment or a real statement.
  //
  // The logic here is the following:
  //  - We invoke PrepareStmt on each statement.
  //  - If the statement is a comment we simply skip it.
  //  - If the statement is valid, we step once to make sure side effects take
  //    effect.
  //  - If we encounter a valid statement afterwards, we step internally through
  //    all rows of the previous one. This ensures that any further side effects
  //    take hold *before* we step into the next statement.
  //  - Once no further non-comment statements are encountered, we return an
  //    iterator to the last valid statement.
  for (const char* rem_sql = sql.c_str(); rem_sql && rem_sql[0];) {
    ScopedStmt cur_stmt;
    {
      PERFETTO_TP_TRACE(metatrace::Category::QUERY, "QUERY_PREPARE");
      const char* tail = nullptr;
      RETURN_IF_ERROR(sqlite_utils::PrepareStmt(db, rem_sql, &cur_stmt, &tail));
      rem_sql = tail;
    }

    // The only situation where we'd have an ok status but also no prepared
    // statement is if the statement of SQL we parsed was a pure comment. In
    // this case, just continue to the next statement.
    if (!cur_stmt)
      continue;

    // Before stepping into |cur_stmt|, we need to finish iterating through
    // the previous statement so we don't have two clashing statements (e.g.
    // SELECT * FROM v and DROP VIEW v) partially stepped into.
    if (prev_stmt) {
      PERFETTO_TP_TRACE(metatrace::Category::QUERY, "STMT_STEP_UNTIL_DONE",
                        [&prev_stmt](metatrace::Record* record) {
                          auto expanded_sql =
                              sqlite_utils::ExpandedSqlForStmt(*prev_stmt);
                          record->AddArg("SQL", expanded_sql.get());
                        });
      RETURN_IF_ERROR(sqlite_utils::StepStmtUntilDone(prev_stmt.get()));
    }

    PERFETTO_DLOG("Executing statement: %s", sqlite3_sql(*cur_stmt));

    {
      PERFETTO_TP_TRACE(metatrace::Category::TOPLEVEL, "STMT_FIRST_STEP",
                        [&cur_stmt](metatrace::Record* record) {
                          auto expanded_sql =
                              sqlite_utils::ExpandedSqlForStmt(*cur_stmt);
                          record->AddArg("SQL", expanded_sql.get());
                        });

      // Now step once into |cur_stmt| so that when we prepare the next statment
      // we will have executed any dependent bytecode in this one.
      int err = sqlite3_step(*cur_stmt);
      if (err != SQLITE_ROW && err != SQLITE_DONE) {
        return base::ErrStatus(
            "%s", sqlite_utils::FormatErrorMessage(
                      prev_stmt.get(), base::StringView(sql), db, err)
                      .c_message());
      }
    }

    // Increment the neecessary counts for the statement.
    IncrementCountForStmt(cur_stmt.get(), metadata);

    // Propogate the current statement to the next iteration.
    prev_stmt = std::move(cur_stmt);
  }

  // If we didn't manage to prepare a single statment, that means everything
  // in the SQL was treated as a comment.
  if (!prev_stmt)
    return base::ErrStatus("No valid SQL to run");

  // Update the output statment and column count.
  *output_stmt = std::move(prev_stmt);
  metadata->column_count =
      static_cast<uint32_t>(sqlite3_column_count(output_stmt->get()));
  return base::OkStatus();
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
  }
  PERFETTO_FATAL("For GCC");
}

// Register SQL functions only used in local development instances.
void RegisterDevFunctions(sqlite3* db) {
  RegisterFunction<WriteFile>(db, "WRITE_FILE", 2);
}

sql_modules::NameToModule GetStdlibModules() {
  sql_modules::NameToModule modules;
  for (const auto& file_to_sql : stdlib::kFileToSql) {
    std::string import_key = sql_modules::GetImportKey(file_to_sql.path);
    std::string module = sql_modules::GetModuleName(import_key);
    modules.Insert(module, {}).first->push_back({import_key, file_to_sql.sql});
  }
  return modules;
}

}  // namespace

template <typename View>
void TraceProcessorImpl::RegisterView(const View& view) {
  RegisterDynamicTable(
      std::unique_ptr<ViewGenerator>(new ViewGenerator(&view, View::Name())));
}

TraceProcessorImpl::TraceProcessorImpl(const Config& cfg)
    : TraceProcessorStorageImpl(cfg) {
  context_.fuchsia_trace_tokenizer.reset(new FuchsiaTraceTokenizer(&context_));
  context_.fuchsia_trace_parser.reset(new FuchsiaTraceParser(&context_));

  context_.ninja_log_parser.reset(new NinjaLogParser(&context_));

  context_.systrace_trace_parser.reset(new SystraceTraceParser(&context_));

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

  RegisterAdditionalModules(&context_);

  sqlite3* db = nullptr;
  EnsureSqliteInitialized();
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  InitializeSqlite(db);
  CreateBuiltinTables(db);
  CreateBuiltinViews(db);
  db_.reset(std::move(db));

  // New style function registration.
  if (cfg.enable_dev_features) {
    RegisterDevFunctions(db);
  }
  RegisterFunction<Glob>(db, "glob", 2);
  RegisterFunction<Hash>(db, "HASH", -1);
  RegisterFunction<Base64Encode>(db, "BASE64_ENCODE", 1);
  RegisterFunction<Demangle>(db, "DEMANGLE", 1);
  RegisterFunction<SourceGeq>(db, "SOURCE_GEQ", -1);
  RegisterFunction<ExportJson>(db, "EXPORT_JSON", 1, context_.storage.get(),
                               false);
  RegisterFunction<ExtractArg>(db, "EXTRACT_ARG", 2, context_.storage.get());
  RegisterFunction<AbsTimeStr>(db, "ABS_TIME_STR", 1,
                               context_.clock_tracker.get());
  RegisterFunction<ToMonotonic>(db, "TO_MONOTONIC", 1,
                                context_.clock_tracker.get());
  RegisterFunction<CreateFunction>(
      db, "CREATE_FUNCTION", 3,
      std::unique_ptr<CreateFunction::Context>(
          new CreateFunction::Context{db_.get(), &create_function_state_}));
  RegisterFunction<CreateViewFunction>(
      db, "CREATE_VIEW_FUNCTION", 3,
      std::unique_ptr<CreateViewFunction::Context>(
          new CreateViewFunction::Context{db_.get()}));
  RegisterFunction<Import>(db, "IMPORT", 1,
                           std::unique_ptr<Import::Context>(new Import::Context{
                               db_.get(), this, &sql_modules_}));

  // Old style function registration.
  // TODO(lalitm): migrate this over to using RegisterFunction once aggregate
  // functions are supported.
  RegisterLastNonNullFunction(db);
  RegisterValueAtMaxTsFunction(db);
  {
    base::Status status = PprofFunctions::Register(db, &context_);
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }

  auto stdlib_modules = GetStdlibModules();
  for (auto module_it = stdlib_modules.GetIterator(); module_it; ++module_it) {
    base::Status status =
        RegisterSqlModule({module_it.key(), module_it.value(), false});
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }

  SetupMetrics(this, *db_, &sql_metrics_, cfg.skip_builtin_metric_paths);

  // Setup the query cache.
  query_cache_.reset(new QueryCache());

  const TraceStorage* storage = context_.storage.get();

  SqlStatsTable::RegisterTable(*db_, storage);
  StatsTable::RegisterTable(*db_, storage);

  // Operator tables.
  SpanJoinOperatorTable::RegisterTable(*db_, storage);
  WindowOperatorTable::RegisterTable(*db_, storage);
  CreateViewFunction::RegisterTable(*db_);

  // New style tables but with some custom logic.
  SqliteRawTable::RegisterTable(*db_, query_cache_.get(), &context_);

  // Tables dynamically generated at query time.
  RegisterDynamicTable(std::unique_ptr<ExperimentalFlamegraphGenerator>(
      new ExperimentalFlamegraphGenerator(&context_)));
  RegisterDynamicTable(std::unique_ptr<ExperimentalCounterDurGenerator>(
      new ExperimentalCounterDurGenerator(storage->counter_table())));
  RegisterDynamicTable(std::unique_ptr<ExperimentalSliceLayoutGenerator>(
      new ExperimentalSliceLayoutGenerator(
          context_.storage.get()->mutable_string_pool(),
          &storage->slice_table())));
  RegisterDynamicTable(std::unique_ptr<AncestorGenerator>(new AncestorGenerator(
      AncestorGenerator::Ancestor::kSlice, context_.storage.get())));
  RegisterDynamicTable(std::unique_ptr<AncestorGenerator>(
      new AncestorGenerator(AncestorGenerator::Ancestor::kStackProfileCallsite,
                            context_.storage.get())));
  RegisterDynamicTable(std::unique_ptr<AncestorGenerator>(new AncestorGenerator(
      AncestorGenerator::Ancestor::kSliceByStack, context_.storage.get())));
  RegisterDynamicTable(
      std::unique_ptr<DescendantGenerator>(new DescendantGenerator(
          DescendantGenerator::Descendant::kSlice, context_.storage.get())));
  RegisterDynamicTable(std::unique_ptr<DescendantGenerator>(
      new DescendantGenerator(DescendantGenerator::Descendant::kSliceByStack,
                              context_.storage.get())));
  RegisterDynamicTable(
      std::unique_ptr<ConnectedFlowGenerator>(new ConnectedFlowGenerator(
          ConnectedFlowGenerator::Mode::kDirectlyConnectedFlow,
          context_.storage.get())));
  RegisterDynamicTable(std::unique_ptr<ConnectedFlowGenerator>(
      new ConnectedFlowGenerator(ConnectedFlowGenerator::Mode::kPrecedingFlow,
                                 context_.storage.get())));
  RegisterDynamicTable(std::unique_ptr<ConnectedFlowGenerator>(
      new ConnectedFlowGenerator(ConnectedFlowGenerator::Mode::kFollowingFlow,
                                 context_.storage.get())));
  RegisterDynamicTable(std::unique_ptr<ExperimentalSchedUpidGenerator>(
      new ExperimentalSchedUpidGenerator(storage->sched_slice_table(),
                                         storage->thread_table())));
  RegisterDynamicTable(std::unique_ptr<ExperimentalAnnotatedStackGenerator>(
      new ExperimentalAnnotatedStackGenerator(&context_)));
  RegisterDynamicTable(std::unique_ptr<ExperimentalFlatSliceGenerator>(
      new ExperimentalFlatSliceGenerator(&context_)));

  // Views.
  RegisterView(storage->thread_slice_view());

  // New style db-backed tables.
  // Note: if adding a table here which might potentially contain many rows
  // (O(rows in sched/slice/counter)), then consider calling ShrinkToFit on
  // that table in TraceStorage::ShrinkToFitTables.
  RegisterDbTable(storage->arg_table());
  RegisterDbTable(storage->thread_table());
  RegisterDbTable(storage->process_table());
  RegisterDbTable(storage->filedescriptor_table());

  RegisterDbTable(storage->slice_table());
  RegisterDbTable(storage->flow_table());
  RegisterDbTable(storage->slice_table());
  RegisterDbTable(storage->sched_slice_table());
  RegisterDbTable(storage->thread_state_table());
  RegisterDbTable(storage->gpu_slice_table());

  RegisterDbTable(storage->track_table());
  RegisterDbTable(storage->thread_track_table());
  RegisterDbTable(storage->process_track_table());
  RegisterDbTable(storage->gpu_track_table());

  RegisterDbTable(storage->counter_table());

  RegisterDbTable(storage->counter_track_table());
  RegisterDbTable(storage->process_counter_track_table());
  RegisterDbTable(storage->thread_counter_track_table());
  RegisterDbTable(storage->cpu_counter_track_table());
  RegisterDbTable(storage->irq_counter_track_table());
  RegisterDbTable(storage->softirq_counter_track_table());
  RegisterDbTable(storage->gpu_counter_track_table());
  RegisterDbTable(storage->gpu_counter_group_table());
  RegisterDbTable(storage->perf_counter_track_table());
  RegisterDbTable(storage->energy_counter_track_table());
  RegisterDbTable(storage->uid_counter_track_table());
  RegisterDbTable(storage->energy_per_uid_counter_track_table());

  RegisterDbTable(storage->heap_graph_object_table());
  RegisterDbTable(storage->heap_graph_reference_table());
  RegisterDbTable(storage->heap_graph_class_table());

  RegisterDbTable(storage->symbol_table());
  RegisterDbTable(storage->heap_profile_allocation_table());
  RegisterDbTable(storage->cpu_profile_stack_sample_table());
  RegisterDbTable(storage->perf_sample_table());
  RegisterDbTable(storage->stack_profile_callsite_table());
  RegisterDbTable(storage->stack_profile_mapping_table());
  RegisterDbTable(storage->stack_profile_frame_table());
  RegisterDbTable(storage->package_list_table());
  RegisterDbTable(storage->profiler_smaps_table());

  RegisterDbTable(storage->android_log_table());
  RegisterDbTable(storage->android_dumpstate_table());
  RegisterDbTable(storage->android_game_intervention_list_table());

  RegisterDbTable(storage->vulkan_memory_allocations_table());

  RegisterDbTable(storage->graphics_frame_slice_table());

  RegisterDbTable(storage->expected_frame_timeline_slice_table());
  RegisterDbTable(storage->actual_frame_timeline_slice_table());

  RegisterDbTable(storage->metadata_table());
  RegisterDbTable(storage->cpu_table());
  RegisterDbTable(storage->cpu_freq_table());
  RegisterDbTable(storage->clock_snapshot_table());

  RegisterDbTable(storage->memory_snapshot_table());
  RegisterDbTable(storage->process_memory_snapshot_table());
  RegisterDbTable(storage->memory_snapshot_node_table());
  RegisterDbTable(storage->memory_snapshot_edge_table());

  RegisterDbTable(storage->experimental_proto_path_table());
  RegisterDbTable(storage->experimental_proto_content_table());

  RegisterDbTable(storage->experimental_missing_chrome_processes_table());
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
  BuildBoundsTable(*db_, context_.storage->GetTraceTimestampBoundsNs());
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
  BuildBoundsTable(*db_, context_.storage->GetTraceTimestampBoundsNs());

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
  PERFETTO_TP_TRACE(metatrace::Category::TOPLEVEL, "QUERY_EXECUTE");

  uint32_t sql_stats_row =
      context_.storage->mutable_sql_stats()->RecordQueryBegin(
          sql, base::GetWallTimeNs().count());

  ScopedStmt stmt;
  IteratorImpl::StmtMetadata metadata;
  base::Status status =
      PrepareAndStepUntilLastValidStmt(*db_, sql, &stmt, &metadata);
  PERFETTO_DCHECK((status.ok() && stmt) || (!status.ok() && !stmt));

  std::unique_ptr<IteratorImpl> impl(new IteratorImpl(
      this, *db_, status, std::move(stmt), std::move(metadata), sql_stats_row));
  return Iterator(std::move(impl));
}

void TraceProcessorImpl::InterruptQuery() {
  if (!db_)
    return;
  query_interrupted_.store(true);
  sqlite3_interrupt(db_.get());
}

bool TraceProcessorImpl::IsRootMetricField(const std::string& metric_name) {
  base::Optional<uint32_t> desc_idx =
      pool_.FindDescriptorIdx(".perfetto.protos.TraceMetrics");
  if (!desc_idx.has_value())
    return false;
  auto field_idx = pool_.descriptors()[*desc_idx].FindFieldByName(metric_name);
  return field_idx != nullptr;
}

base::Status TraceProcessorImpl::RegisterSqlModule(SqlModule sql_module) {
  sql_modules::RegisteredModule new_module;
  std::string name = sql_module.name;
  if (sql_modules_.Find(name) && !sql_module.allow_module_override) {
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
    new_module.import_key_to_file.Insert(name_and_sql.first,
                                         {name_and_sql.second, false});
  }
  sql_modules_.Insert(name, std::move(new_module));
  return base::OkStatus();
}

base::Status TraceProcessorImpl::RegisterMetric(const std::string& path,
                                                const std::string& sql) {
  std::string stripped_sql;
  for (base::StringSplitter sp(sql, '\n'); sp.Next();) {
    if (strncmp(sp.cur_token(), "--", 2) != 0) {
      stripped_sql.append(sp.cur_token());
      stripped_sql.push_back('\n');
    }
  }

  // Check if the metric with the given path already exists and if it does,
  // just update the SQL associated with it.
  auto it = std::find_if(
      sql_metrics_.begin(), sql_metrics_.end(),
      [&path](const metrics::SqlMetricFile& m) { return m.path == path; });
  if (it != sql_metrics_.end()) {
    it->sql = stripped_sql;
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
  metric.sql = stripped_sql;

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

    InsertIntoTraceMetricsTable(*db_, no_ext_name);
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
        db_.get(), fn_name.c_str(), -1,
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
  return metrics::ComputeMetrics(this, metric_names, sql_metrics_, pool_,
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
      // TODO(dproy): Implement this.
      PERFETTO_FATAL("Json formatted metrics not supported yet.");
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
