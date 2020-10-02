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
#include "src/trace_processor/dynamic/ancestor_slice_generator.h"
#include "src/trace_processor/dynamic/connected_flow_generator.h"
#include "src/trace_processor/dynamic/descendant_slice_generator.h"
#include "src/trace_processor/dynamic/describe_slice_generator.h"
#include "src/trace_processor/dynamic/experimental_counter_dur_generator.h"
#include "src/trace_processor/dynamic/experimental_flamegraph_generator.h"
#include "src/trace_processor/dynamic/experimental_sched_upid_generator.h"
#include "src/trace_processor/dynamic/experimental_slice_layout_generator.h"
#include "src/trace_processor/dynamic/thread_state_generator.h"
#include "src/trace_processor/export_json.h"
#include "src/trace_processor/importers/additional_modules.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_tokenizer.h"
#include "src/trace_processor/importers/gzip/gzip_trace_parser.h"
#include "src/trace_processor/importers/json/json_trace_parser.h"
#include "src/trace_processor/importers/json/json_trace_tokenizer.h"
#include "src/trace_processor/importers/proto/metadata_tracker.h"
#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"
#include "src/trace_processor/iterator_impl.h"
#include "src/trace_processor/sqlite/span_join_operator_table.h"
#include "src/trace_processor/sqlite/sql_stats_table.h"
#include "src/trace_processor/sqlite/sqlite3_str_split.h"
#include "src/trace_processor/sqlite/sqlite_raw_table.h"
#include "src/trace_processor/sqlite/sqlite_table.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/sqlite/stats_table.h"
#include "src/trace_processor/sqlite/window_operator_table.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/protozero_to_text.h"

#include "protos/perfetto/trace/perfetto/perfetto_metatrace.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#include "src/trace_processor/metrics/chrome/all_chrome_metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/metrics/sql_metrics.h"

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
  // Ensure that the entries in power_profile are unique to prevent duplicates
  // when the power_profile is augmented with additional profiles.
  sqlite3_exec(db,
               "CREATE TABLE power_profile("
               "device STRING, cpu INT, cluster INT, freq INT, power DOUBLE,"
               "UNIQUE(device, cpu, cluster, freq));",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
  sqlite3_exec(db, "CREATE TABLE trace_metrics(name STRING)", 0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
  // This is a table intended to be used for metric debugging/developing. Data
  // in the table is shown specially in the UI, and users can insert rows into
  // this table to draw more things.
  sqlite3_exec(db,
               "CREATE TABLE debug_slices (id BIG INT, name STRING, ts BIG INT,"
               "dur BIG INT, depth BIG INT)",
               0, 0, &error);
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
               "  id AS slice_id "
               "FROM internal_slice;",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  sqlite3_exec(db,
               "CREATE VIEW instants AS "
               "SELECT "
               "*, "
               "0.0 as value "
               "FROM instant;",
               0, 0, &error);

  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  sqlite3_exec(db,
               "CREATE VIEW sched AS "
               "SELECT "
               "*, "
               "ts + dur as ts_end "
               "FROM sched_slice;",
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

  sqlite3_exec(db,
               "CREATE VIEW thread AS "
               "SELECT "
               "id as utid, "
               "* "
               "FROM internal_thread;",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }

  sqlite3_exec(db,
               "CREATE VIEW process AS "
               "SELECT "
               "id as upid, "
               "* "
               "FROM internal_process;",
               0, 0, &error);
  if (error) {
    PERFETTO_ELOG("Error initializing: %s", error);
    sqlite3_free(error);
  }
}

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
  if (sqlite3_value_type(value) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }
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

void LastNonNullStep(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != 1) {
    sqlite3_result_error(
        ctx, "Unsupported number of args passed to LAST_NON_NULL", -1);
    return;
  }
  sqlite3_value* value = argv[0];
  if (sqlite3_value_type(value) == SQLITE_NULL) {
    return;
  }
  sqlite3_value** ptr = reinterpret_cast<sqlite3_value**>(
      sqlite3_aggregate_context(ctx, sizeof(sqlite3_value*)));
  if (ptr) {
    if (*ptr != nullptr) {
      sqlite3_value_free(*ptr);
    }
    *ptr = sqlite3_value_dup(value);
  }
}

void LastNonNullInverse(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  // Do nothing.
  base::ignore_result(ctx);
  base::ignore_result(argc);
  base::ignore_result(argv);
}

void LastNonNullValue(sqlite3_context* ctx) {
  sqlite3_value** ptr =
      reinterpret_cast<sqlite3_value**>(sqlite3_aggregate_context(ctx, 0));
  if (!ptr || !*ptr) {
    sqlite3_result_null(ctx);
  } else {
    sqlite3_result_value(ctx, *ptr);
  }
}

void LastNonNullFinal(sqlite3_context* ctx) {
  sqlite3_value** ptr =
      reinterpret_cast<sqlite3_value**>(sqlite3_aggregate_context(ctx, 0));
  if (!ptr || !*ptr) {
    sqlite3_result_null(ctx);
  } else {
    sqlite3_result_value(ctx, *ptr);
    sqlite3_value_free(*ptr);
  }
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

void CreateLastNonNullFunction(sqlite3* db) {
  auto ret = sqlite3_create_window_function(
      db, "LAST_NON_NULL", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
      &LastNonNullStep, &LastNonNullFinal, &LastNonNullValue,
      &LastNonNullInverse, nullptr);
  if (ret) {
    PERFETTO_ELOG("Error initializing LAST_NON_NULL");
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
  if (PERFETTO_LIKELY(fn_ctx->max_ts < ts_int)) {
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

void CreateValueAtMaxTsFunction(sqlite3* db) {
  auto ret = sqlite3_create_function_v2(
      db, "VALUE_AT_MAX_TS", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
      nullptr, &ValueAtMaxTsStep, &ValueAtMaxTsFinal, nullptr);
  if (ret) {
    PERFETTO_ELOG("Error initializing VALUE_AT_MAX_TS");
  }
}

void ExtractArg(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != 2) {
    sqlite3_result_error(ctx, "EXTRACT_ARG: 2 args required", -1);
    return;
  }
  if (sqlite3_value_type(argv[0]) != SQLITE_INTEGER) {
    sqlite3_result_error(ctx, "EXTRACT_ARG: 1st argument should be arg set id",
                         -1);
    return;
  }
  if (sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
    sqlite3_result_error(ctx, "EXTRACT_ARG: 2nd argument should be key", -1);
    return;
  }

  TraceStorage* storage = static_cast<TraceStorage*>(sqlite3_user_data(ctx));
  uint32_t arg_set_id = static_cast<uint32_t>(sqlite3_value_int(argv[0]));
  const char* key = reinterpret_cast<const char*>(sqlite3_value_text(argv[1]));

  base::Optional<Variadic> opt_value;
  util::Status status = storage->ExtractArg(arg_set_id, key, &opt_value);
  if (!status.ok()) {
    sqlite3_result_error(ctx, status.c_message(), -1);
    return;
  }

  if (!opt_value) {
    sqlite3_result_null(ctx);
    return;
  }

  switch (opt_value->type) {
    case Variadic::kInt:
      sqlite3_result_int64(ctx, opt_value->int_value);
      break;
    case Variadic::kBool:
      sqlite3_result_int64(ctx, opt_value->bool_value);
      break;
    case Variadic::kUint:
      sqlite3_result_int64(ctx, static_cast<int64_t>(opt_value->uint_value));
      break;
    case Variadic::kPointer:
      sqlite3_result_int64(ctx, static_cast<int64_t>(opt_value->pointer_value));
      break;
    case Variadic::kJson:
      sqlite3_result_text(ctx, storage->GetString(opt_value->json_value).data(),
                          -1, nullptr);
      break;
    case Variadic::kString:
      sqlite3_result_text(
          ctx, storage->GetString(opt_value->string_value).data(), -1, nullptr);
      break;
    case Variadic::kReal:
      sqlite3_result_double(ctx, opt_value->real_value);
      break;
  }
}

void CreateExtractArgFunction(TraceStorage* ts, sqlite3* db) {
  auto ret = sqlite3_create_function_v2(db, "EXTRACT_ARG", 2,
                                        SQLITE_UTF8 | SQLITE_DETERMINISTIC, ts,
                                        &ExtractArg, nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    PERFETTO_FATAL("Error initializing EXTRACT_ARG: %s", sqlite3_errmsg(db));
  }
}

void CreateSourceGeqFunction(sqlite3* db) {
  auto fn = [](sqlite3_context* ctx, int, sqlite3_value**) {
    sqlite3_result_error(
        ctx, "SOURCE_GEQ should not be called from the global scope", -1);
  };
  auto ret = sqlite3_create_function_v2(db, "SOURCE_GEQ", -1,
                                        SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                        nullptr, fn, nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    PERFETTO_FATAL("Error initializing SOURCE_GEQ: %s", sqlite3_errmsg(db));
  }
}

void SetupMetrics(TraceProcessor* tp,
                  sqlite3* db,
                  std::vector<metrics::SqlMetricFile>* sql_metrics) {
  tp->ExtendMetricsProto(kMetricsDescriptor.data(), kMetricsDescriptor.size());
  tp->ExtendMetricsProto(kAllChromeMetricsDescriptor.data(),
                         kAllChromeMetricsDescriptor.size());

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
      PERFETTO_FATAL("Error initializing RUN_METRIC");
  }

  {
    auto ret = sqlite3_create_function_v2(
        db, "RepeatedField", 1, SQLITE_UTF8, nullptr, nullptr,
        metrics::RepeatedFieldStep, metrics::RepeatedFieldFinal, nullptr);
    if (ret)
      PERFETTO_FATAL("Error initializing RepeatedField");
  }

  {
    auto ret = sqlite3_create_function_v2(db, "NULL_IF_EMPTY", 1, SQLITE_UTF8,
                                          nullptr, metrics::NullIfEmpty,
                                          nullptr, nullptr, nullptr);
    if (ret)
      PERFETTO_FATAL("Error initializing NULL_IF_EMPTY");
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

}  // namespace

TraceProcessorImpl::TraceProcessorImpl(const Config& cfg)
    : TraceProcessorStorageImpl(cfg) {
  context_.fuchsia_trace_tokenizer.reset(new FuchsiaTraceTokenizer(&context_));
  context_.fuchsia_trace_parser.reset(new FuchsiaTraceParser(&context_));

  context_.systrace_trace_parser.reset(new SystraceTraceParser(&context_));

  if (gzip::IsGzipSupported())
    context_.gzip_trace_parser.reset(new GzipTraceParser(&context_));

  if (json::IsJsonSupported()) {
    context_.json_trace_tokenizer.reset(new JsonTraceTokenizer(&context_));
    context_.json_trace_parser.reset(new JsonTraceParser(&context_));
  }

  RegisterAdditionalModules(&context_);

  sqlite3* db = nullptr;
  EnsureSqliteInitialized();
  PERFETTO_CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
  InitializeSqlite(db);
  CreateBuiltinTables(db);
  CreateBuiltinViews(db);
  db_.reset(std::move(db));

  CreateJsonExportFunction(context_.storage.get(), db);
  CreateHashFunction(db);
  CreateDemangledNameFunction(db);
  CreateLastNonNullFunction(db);
  CreateExtractArgFunction(context_.storage.get(), db);
  CreateSourceGeqFunction(db);
  CreateValueAtMaxTsFunction(db);

  SetupMetrics(this, *db_, &sql_metrics_);

  // Setup the query cache.
  query_cache_.reset(new QueryCache());

  const TraceStorage* storage = context_.storage.get();

  SqlStatsTable::RegisterTable(*db_, storage);
  StatsTable::RegisterTable(*db_, storage);

  // Operator tables.
  SpanJoinOperatorTable::RegisterTable(*db_, storage);
  WindowOperatorTable::RegisterTable(*db_, storage);

  // New style tables but with some custom logic.
  SqliteRawTable::RegisterTable(*db_, query_cache_.get(), &context_);

  // Tables dynamically generated at query time.
  RegisterDynamicTable(std::unique_ptr<ExperimentalFlamegraphGenerator>(
      new ExperimentalFlamegraphGenerator(&context_)));
  RegisterDynamicTable(std::unique_ptr<ExperimentalCounterDurGenerator>(
      new ExperimentalCounterDurGenerator(storage->counter_table())));
  RegisterDynamicTable(std::unique_ptr<DescribeSliceGenerator>(
      new DescribeSliceGenerator(&context_)));
  RegisterDynamicTable(std::unique_ptr<ExperimentalSliceLayoutGenerator>(
      new ExperimentalSliceLayoutGenerator(
          context_.storage.get()->mutable_string_pool(),
          &storage->slice_table())));
  RegisterDynamicTable(std::unique_ptr<AncestorSliceGenerator>(
      new AncestorSliceGenerator(&context_)));
  RegisterDynamicTable(std::unique_ptr<DescendantSliceGenerator>(
      new DescendantSliceGenerator(&context_)));
  RegisterDynamicTable(
      std::unique_ptr<ConnectedFlowGenerator>(new ConnectedFlowGenerator(
          ConnectedFlowGenerator::Direction::BOTH, &context_)));
  RegisterDynamicTable(
      std::unique_ptr<ConnectedFlowGenerator>(new ConnectedFlowGenerator(
          ConnectedFlowGenerator::Direction::FOLLOWING, &context_)));
  RegisterDynamicTable(
      std::unique_ptr<ConnectedFlowGenerator>(new ConnectedFlowGenerator(
          ConnectedFlowGenerator::Direction::PRECEDING, &context_)));
  RegisterDynamicTable(std::unique_ptr<ExperimentalSchedUpidGenerator>(
      new ExperimentalSchedUpidGenerator(storage->sched_slice_table(),
                                         storage->thread_table())));
  RegisterDynamicTable(std::unique_ptr<ThreadStateGenerator>(
      new ThreadStateGenerator(&context_)));

  // New style db-backed tables.
  RegisterDbTable(storage->arg_table());
  RegisterDbTable(storage->thread_table());
  RegisterDbTable(storage->process_table());

  RegisterDbTable(storage->slice_table());
  RegisterDbTable(storage->flow_table());
  RegisterDbTable(storage->sched_slice_table());
  RegisterDbTable(storage->instant_table());
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

  RegisterDbTable(storage->vulkan_memory_allocations_table());

  RegisterDbTable(storage->graphics_frame_slice_table());

  RegisterDbTable(storage->metadata_table());
  RegisterDbTable(storage->cpu_table());
  RegisterDbTable(storage->cpu_freq_table());

  RegisterDbTable(storage->memory_snapshot_table());
  RegisterDbTable(storage->process_memory_snapshot_table());
  RegisterDbTable(storage->memory_snapshot_node_table());
  RegisterDbTable(storage->memory_snapshot_edge_table());
}

TraceProcessorImpl::~TraceProcessorImpl() = default;

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

  SchedEventTracker::GetOrCreate(&context_)->FlushPendingEvents();
  context_.metadata_tracker->SetMetadata(
      metadata::trace_size_bytes,
      Variadic::Integer(static_cast<int64_t>(bytes_parsed_)));
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
    // Index deletion can legitimately fail. If one creates an index "i" on a
    // table "t" but issues the deletion in the order (t, i), the DROP index i
    // will fail with "no such index" because deleting the table "t"
    // automatically deletes all associated indexes.
    if (!it.Status().ok() && tn.first != "index")
      PERFETTO_FATAL("%s -> %s", query.c_str(), it.Status().c_message());
  }
  return deletion_list.size();
}

Iterator TraceProcessorImpl::ExecuteQuery(const std::string& sql,
                                          int64_t time_queued) {
  sqlite3_stmt* raw_stmt;
  int err;
  {
    PERFETTO_TP_TRACE("QUERY_PREPARE");
    err = sqlite3_prepare_v2(*db_, sql.c_str(), static_cast<int>(sql.size()),
                             &raw_stmt, nullptr);
  }

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
  base::Optional<uint32_t> field_idx =
      pool_.descriptors()[*desc_idx].FindFieldIdxByName(metric_name);
  return field_idx.has_value();
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
  metric.sql = stripped_sql;

  if (IsRootMetricField(no_ext_name)) {
    metric.proto_field_name = no_ext_name;
    metric.output_table_name = no_ext_name + "_output";
    InsertIntoTraceMetricsTable(*db_, no_ext_name);
  }

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

util::Status TraceProcessorImpl::ComputeMetricText(
    const std::vector<std::string>& metric_names,
    TraceProcessor::MetricResultFormat format,
    std::string* metrics_string) {
  std::vector<uint8_t> metrics_proto;
  util::Status status = ComputeMetric(metric_names, &metrics_proto);
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

void TraceProcessorImpl::EnableMetatrace() {
  metatrace::Enable();
}

util::Status TraceProcessorImpl::DisableAndReadMetatrace(
    std::vector<uint8_t>* trace_proto) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;
  metatrace::DisableAndReadBuffer([&trace](metatrace::Record* record) {
    auto packet = trace->add_packet();
    packet->set_timestamp(record->timestamp_ns);
    auto* evt = packet->set_perfetto_metatrace();
    evt->set_event_name(record->event_name);
    evt->set_event_duration_ns(record->duration_ns);
    evt->set_thread_id(1);  // Not really important, just required for the ui.

    if (record->args_buffer_size == 0)
      return;

    base::StringSplitter s(record->args_buffer, record->args_buffer_size, '\0');
    for (; s.Next();) {
      auto* arg_proto = evt->add_args();
      arg_proto->set_key(s.cur_token());

      bool has_next = s.Next();
      PERFETTO_CHECK(has_next);
      arg_proto->set_value(s.cur_token());
    }
  });
  *trace_proto = trace.SerializeAsArray();
  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
