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
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/thread_utils.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/clock_snapshots.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/importers/android_bugreport/android_log_event_parser_impl.h"
#include "src/trace_processor/importers/android_bugreport/android_log_reader.h"
#include "src/trace_processor/importers/archive/gzip_trace_parser.h"
#include "src/trace_processor/importers/archive/tar_trace_reader.h"
#include "src/trace_processor/importers/archive/zip_trace_reader.h"
#include "src/trace_processor/importers/art_method/art_method_parser_impl.h"
#include "src/trace_processor/importers/art_method/art_method_tokenizer.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_tokenizer.h"
#include "src/trace_processor/importers/gecko/gecko_trace_parser_impl.h"
#include "src/trace_processor/importers/gecko/gecko_trace_tokenizer.h"
#include "src/trace_processor/importers/json/json_trace_parser_impl.h"
#include "src/trace_processor/importers/json/json_trace_tokenizer.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/importers/ninja/ninja_log_parser.h"
#include "src/trace_processor/importers/perf/perf_data_tokenizer.h"
#include "src/trace_processor/importers/perf/record_parser.h"
#include "src/trace_processor/importers/perf/spe_record_parser.h"
#include "src/trace_processor/importers/perf_text/perf_text_trace_parser_impl.h"
#include "src/trace_processor/importers/perf_text/perf_text_trace_tokenizer.h"
#include "src/trace_processor/importers/proto/additional_modules.h"
#include "src/trace_processor/importers/proto/content_analyzer.h"
#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"
#include "src/trace_processor/iterator_impl.h"
#include "src/trace_processor/metrics/all_chrome_metrics.descriptor.h"
#include "src/trace_processor/metrics/all_webview_metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/metrics/sql/amalgamated_sql_metrics.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/engine/table_pointer_module.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/base64.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/clock_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/counter_intervals.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/create_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/create_view_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/dominator_tree.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/graph_scan.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/graph_traversal.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/import.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/interval_intersect.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/layout_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/math.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/pprof_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/sqlite3_str_split.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/stack_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/structural_tree_partition.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/to_ftrace.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/type_builders.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/utils.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/window_functions.h"
#include "src/trace_processor/perfetto_sql/intrinsics/operators/counter_mipmap_operator.h"
#include "src/trace_processor/perfetto_sql/intrinsics/operators/slice_mipmap_operator.h"
#include "src/trace_processor/perfetto_sql/intrinsics/operators/span_join_operator.h"
#include "src/trace_processor/perfetto_sql/intrinsics/operators/window_operator.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/ancestor.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/connected_flow.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/descendant.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/dfs_weight_bounded.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_annotated_stack.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_counter_dur.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flamegraph.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flat_slice.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_sched_upid.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_slice_layout.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/table_info.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/winscope_proto_to_args_with_defaults.h"
#include "src/trace_processor/perfetto_sql/stdlib/stdlib.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sql_stats_table.h"
#include "src/trace_processor/sqlite/stats_table.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/trace_processor_storage_impl.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/gzip_utils.h"
#include "src/trace_processor/util/protozero_to_json.h"
#include "src/trace_processor/util/protozero_to_text.h"
#include "src/trace_processor/util/regex.h"
#include "src/trace_processor/util/sql_modules.h"
#include "src/trace_processor/util/status_macros.h"
#include "src/trace_processor/util/trace_type.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_INSTRUMENTS)
#include "src/trace_processor/importers/instruments/instruments_xml_tokenizer.h"
#include "src/trace_processor/importers/instruments/row_parser.h"
#endif

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/perfetto/perfetto_metatrace.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {
namespace {

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

base::Status RegisterAllProtoBuilderFunctions(
    DescriptorPool* pool,
    std::unordered_map<std::string, std::string>* proto_fn_name_to_path,
    PerfettoSqlEngine* engine,
    TraceProcessor* tp) {
  for (uint32_t i = 0; i < pool->descriptors().size(); ++i) {
    // Convert the full name (e.g. .perfetto.protos.TraceMetrics.SubMetric)
    // into a function name of the form (TraceMetrics_SubMetric).
    const auto& desc = pool->descriptors()[i];
    auto fn_name = desc.full_name().substr(desc.package_name().size() + 1);
    std::replace(fn_name.begin(), fn_name.end(), '.', '_');
    auto registered_fn = proto_fn_name_to_path->find(fn_name);
    if (registered_fn != proto_fn_name_to_path->end() &&
        registered_fn->second != desc.full_name()) {
      return base::ErrStatus(
          "Attempt to create new metric function '%s' for different descriptor "
          "'%s' that conflicts with '%s'",
          fn_name.c_str(), desc.full_name().c_str(),
          registered_fn->second.c_str());
    }
    RegisterFunction<metrics::BuildProto>(
        engine, fn_name.c_str(), -1,
        std::make_unique<metrics::BuildProto::Context>(
            metrics::BuildProto::Context{tp, pool, i}));
    proto_fn_name_to_path->emplace(fn_name, desc.full_name());
  }
  return base::OkStatus();
}

void BuildBoundsTable(sqlite3* db, std::pair<int64_t, int64_t> bounds) {
  char* error = nullptr;
  sqlite3_exec(db, "DELETE FROM _trace_bounds", nullptr, nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error deleting from bounds table: %s", error);
    sqlite3_free(error);
    return;
  }

  base::StackString<1024> sql("INSERT INTO _trace_bounds VALUES(%" PRId64
                              ", %" PRId64 ")",
                              bounds.first, bounds.second);
  sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  if (error) {
    PERFETTO_ELOG("Error inserting bounds table: %s", error);
    sqlite3_free(error);
  }
}

class ValueAtMaxTs : public SqliteAggregateFunction<ValueAtMaxTs> {
 public:
  static constexpr char kName[] = "VALUE_AT_MAX_TS";
  static constexpr int kArgCount = 2;
  struct Context {
    bool initialized;
    int value_type;

    int64_t max_ts;
    int64_t int_value_at_max_ts;
    double double_value_at_max_ts;
  };

  static void Step(sqlite3_context* ctx, int, sqlite3_value** argv) {
    sqlite3_value* ts = argv[0];
    sqlite3_value* value = argv[1];

    // Note that sqlite3_aggregate_context zeros the memory for us so all the
    // variables of the struct should be zero.
    auto* fn_ctx = reinterpret_cast<Context*>(
        sqlite3_aggregate_context(ctx, sizeof(Context)));

    // For performance reasons, we only do the check for the type of ts and
    // value on the first call of the function.
    if (PERFETTO_UNLIKELY(!fn_ctx->initialized)) {
      if (sqlite3_value_type(ts) != SQLITE_INTEGER) {
        return sqlite::result::Error(
            ctx, "VALUE_AT_MAX_TS: ts passed was not an integer");
      }

      fn_ctx->value_type = sqlite3_value_type(value);
      if (fn_ctx->value_type != SQLITE_INTEGER &&
          fn_ctx->value_type != SQLITE_FLOAT) {
        return sqlite::result::Error(
            ctx, "VALUE_AT_MAX_TS: value passed was not an integer or float");
      }

      fn_ctx->max_ts = std::numeric_limits<int64_t>::min();
      fn_ctx->initialized = true;
    }

    // On dcheck builds however, we check every passed ts and value.
#if PERFETTO_DCHECK_IS_ON()
    if (sqlite3_value_type(ts) != SQLITE_INTEGER) {
      return sqlite::result::Error(
          ctx, "VALUE_AT_MAX_TS: ts passed was not an integer");
    }
    if (sqlite3_value_type(value) != fn_ctx->value_type) {
      return sqlite::result::Error(
          ctx, "VALUE_AT_MAX_TS: value type is inconsistent");
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

  static void Final(sqlite3_context* ctx) {
    auto* fn_ctx = static_cast<Context*>(sqlite3_aggregate_context(ctx, 0));
    if (!fn_ctx) {
      sqlite::result::Null(ctx);
      return;
    }
    if (fn_ctx->value_type == SQLITE_INTEGER) {
      sqlite::result::Long(ctx, fn_ctx->int_value_at_max_ts);
    } else {
      sqlite::result::Double(ctx, fn_ctx->double_value_at_max_ts);
    }
  }
};

void RegisterValueAtMaxTsFunction(PerfettoSqlEngine& engine) {
  base::Status status =
      engine.RegisterSqliteAggregateFunction<ValueAtMaxTs>(nullptr);
  if (!status.ok()) {
    PERFETTO_ELOG("Error initializing VALUE_AT_MAX_TS");
  }
}

std::vector<std::string> SanitizeMetricMountPaths(
    const std::vector<std::string>& mount_paths) {
  std::vector<std::string> sanitized;
  for (const auto& path : mount_paths) {
    if (path.empty())
      continue;
    sanitized.push_back(path);
    if (path.back() != '/')
      sanitized.back().append("/");
  }
  return sanitized;
}

void InsertIntoTraceMetricsTable(sqlite3* db, const std::string& metric_name) {
  char* insert_sql = sqlite3_mprintf(
      "INSERT INTO _trace_metrics(name) VALUES('%q')", metric_name.c_str());
  char* insert_error = nullptr;
  sqlite3_exec(db, insert_sql, nullptr, nullptr, &insert_error);
  sqlite3_free(insert_sql);
  if (insert_error) {
    PERFETTO_ELOG("Error registering table: %s", insert_error);
    sqlite3_free(insert_error);
  }
}

sql_modules::NameToPackage GetStdlibPackages() {
  sql_modules::NameToPackage packages;
  for (const auto& file_to_sql : stdlib::kFileToSql) {
    std::string module_name = sql_modules::GetIncludeKey(file_to_sql.path);
    std::string package_name = sql_modules::GetPackageName(module_name);
    packages.Insert(package_name, {})
        .first->push_back({module_name, file_to_sql.sql});
  }
  return packages;
}

std::pair<int64_t, int64_t> GetTraceTimestampBoundsNs(
    const TraceStorage& storage) {
  int64_t start_ns = std::numeric_limits<int64_t>::max();
  int64_t end_ns = std::numeric_limits<int64_t>::min();
  for (auto it = storage.raw_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts(), end_ns);
  }
  for (auto it = storage.sched_slice_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts() + it.dur(), end_ns);
  }
  for (auto it = storage.counter_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts(), end_ns);
  }
  for (auto it = storage.slice_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts() + it.dur(), end_ns);
  }
  for (auto it = storage.heap_profile_allocation_table().IterateRows(); it;
       ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts(), end_ns);
  }
  for (auto it = storage.thread_state_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts() + it.dur(), end_ns);
  }
  for (auto it = storage.android_log_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts(), end_ns);
  }
  for (auto it = storage.heap_graph_object_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.graph_sample_ts(), start_ns);
    end_ns = std::max(it.graph_sample_ts(), end_ns);
  }
  for (auto it = storage.perf_sample_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts(), end_ns);
  }
  for (auto it = storage.instruments_sample_table().IterateRows(); it; ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts(), end_ns);
  }
  for (auto it = storage.cpu_profile_stack_sample_table().IterateRows(); it;
       ++it) {
    start_ns = std::min(it.ts(), start_ns);
    end_ns = std::max(it.ts(), end_ns);
  }
  if (start_ns == std::numeric_limits<int64_t>::max()) {
    return std::make_pair(0, 0);
  }
  if (start_ns == end_ns) {
    end_ns += 1;
  }
  return std::make_pair(start_ns, end_ns);
}

}  // namespace

TraceProcessorImpl::TraceProcessorImpl(const Config& cfg)
    : TraceProcessorStorageImpl(cfg), config_(cfg) {
  context_.reader_registry->RegisterTraceReader<AndroidLogReader>(
      kAndroidLogcatTraceType);
  context_.android_log_event_parser =
      std::make_unique<AndroidLogEventParserImpl>(&context_);

  context_.reader_registry->RegisterTraceReader<FuchsiaTraceTokenizer>(
      kFuchsiaTraceType);
  context_.fuchsia_record_parser =
      std::make_unique<FuchsiaTraceParser>(&context_);

  context_.reader_registry->RegisterTraceReader<SystraceTraceParser>(
      kSystraceTraceType);
  context_.reader_registry->RegisterTraceReader<NinjaLogParser>(
      kNinjaLogTraceType);

  context_.reader_registry
      ->RegisterTraceReader<perf_importer::PerfDataTokenizer>(
          kPerfDataTraceType);
  context_.perf_record_parser =
      std::make_unique<perf_importer::RecordParser>(&context_);
  context_.spe_record_parser =
      std::make_unique<perf_importer::SpeRecordParserImpl>(&context_);

#if PERFETTO_BUILDFLAG(PERFETTO_TP_INSTRUMENTS)
  context_.reader_registry
      ->RegisterTraceReader<instruments_importer::InstrumentsXmlTokenizer>(
          kInstrumentsXmlTraceType);
  context_.instruments_row_parser =
      std::make_unique<instruments_importer::RowParser>(&context_);
#endif

  if constexpr (util::IsGzipSupported()) {
    context_.reader_registry->RegisterTraceReader<GzipTraceParser>(
        kGzipTraceType);
    context_.reader_registry->RegisterTraceReader<GzipTraceParser>(
        kCtraceTraceType);
    context_.reader_registry->RegisterTraceReader<ZipTraceReader>(kZipFile);
  }

  if constexpr (json::IsJsonSupported()) {
    context_.reader_registry->RegisterTraceReader<JsonTraceTokenizer>(
        kJsonTraceType);
    context_.json_trace_parser =
        std::make_unique<JsonTraceParserImpl>(&context_);

    context_.reader_registry
        ->RegisterTraceReader<gecko_importer::GeckoTraceTokenizer>(
            kGeckoTraceType);
    context_.gecko_trace_parser =
        std::make_unique<gecko_importer::GeckoTraceParserImpl>(&context_);
  }

  context_.reader_registry->RegisterTraceReader<art_method::ArtMethodTokenizer>(
      kArtMethodTraceType);
  context_.art_method_parser =
      std::make_unique<art_method::ArtMethodParserImpl>(&context_);

  context_.reader_registry
      ->RegisterTraceReader<perf_text_importer::PerfTextTraceTokenizer>(
          kPerfTextTraceType);
  context_.perf_text_parser =
      std::make_unique<perf_text_importer::PerfTextTraceParserImpl>(&context_);

  context_.reader_registry->RegisterTraceReader<TarTraceReader>(kTarTraceType);

  if (context_.config.analyze_trace_proto_content) {
    context_.content_analyzer =
        std::make_unique<ProtoContentAnalyzer>(&context_);
  }

  // Add metrics to descriptor pool
  const std::vector<std::string> sanitized_extension_paths =
      SanitizeMetricMountPaths(config_.skip_builtin_metric_paths);
  std::vector<std::string> skip_prefixes;
  skip_prefixes.reserve(sanitized_extension_paths.size());
  for (const auto& path : sanitized_extension_paths) {
    skip_prefixes.push_back(kMetricProtoRoot + path);
  }
  pool_.AddFromFileDescriptorSet(kMetricsDescriptor.data(),
                                 kMetricsDescriptor.size(), skip_prefixes);
  pool_.AddFromFileDescriptorSet(kAllChromeMetricsDescriptor.data(),
                                 kAllChromeMetricsDescriptor.size(),
                                 skip_prefixes);
  pool_.AddFromFileDescriptorSet(kAllWebviewMetricsDescriptor.data(),
                                 kAllWebviewMetricsDescriptor.size(),
                                 skip_prefixes);

  RegisterAdditionalModules(&context_);
  InitPerfettoSqlEngine();

  sqlite_objects_post_prelude_ = engine_->SqliteRegisteredObjectCount();

  bool skip_all_sql = std::find(config_.skip_builtin_metric_paths.begin(),
                                config_.skip_builtin_metric_paths.end(),
                                "") != config_.skip_builtin_metric_paths.end();
  if (!skip_all_sql) {
    for (const auto& file_to_sql : sql_metrics::kFileToSql) {
      if (base::StartsWithAny(file_to_sql.path, sanitized_extension_paths))
        continue;
      RegisterMetric(file_to_sql.path, file_to_sql.sql);
    }
  }
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
  BuildBoundsTable(engine_->sqlite_engine()->db(),
                   GetTraceTimestampBoundsNs(*context_.storage));
}

base::Status TraceProcessorImpl::NotifyEndOfFile() {
  if (notify_eof_called_) {
    const char kMessage[] =
        "NotifyEndOfFile should only be called once. Try calling Flush instead "
        "if trying to commit the contents of the trace to tables.";
    PERFETTO_ELOG(kMessage);
    return base::ErrStatus(kMessage);
  }
  notify_eof_called_ = true;

  if (current_trace_name_.empty())
    current_trace_name_ = "Unnamed trace";

  // Last opportunity to flush all pending data.
  Flush();

  RETURN_IF_ERROR(TraceProcessorStorageImpl::NotifyEndOfFile());
  context_.storage->ShrinkToFitTables();

  // Rebuild the bounds table once everything has been completed: we do this
  // so that if any data was added to tables in
  // TraceProcessorStorageImpl::NotifyEndOfFile, this will be counted in
  // trace bounds: this is important for parsers like ninja which wait until
  // the end to flush all their data.
  BuildBoundsTable(engine_->sqlite_engine()->db(),
                   GetTraceTimestampBoundsNs(*context_.storage));

  TraceProcessorStorageImpl::DestroyContext();

  IncludeAfterEofPrelude();
  sqlite_objects_post_prelude_ = engine_->SqliteRegisteredObjectCount();
  return base::OkStatus();
}

size_t TraceProcessorImpl::RestoreInitialTables() {
  // We should always have at least as many objects now as we did in the
  // constructor.
  uint64_t registered_count_before = engine_->SqliteRegisteredObjectCount();
  PERFETTO_CHECK(registered_count_before >= sqlite_objects_post_prelude_);

  InitPerfettoSqlEngine();

  // The registered count should now be the same as it was in the constructor.
  uint64_t registered_count_after = engine_->SqliteRegisteredObjectCount();
  PERFETTO_CHECK(registered_count_after == sqlite_objects_post_prelude_);
  return static_cast<size_t>(registered_count_before - registered_count_after);
}

Iterator TraceProcessorImpl::ExecuteQuery(const std::string& sql) {
  PERFETTO_TP_TRACE(metatrace::Category::API_TIMELINE, "EXECUTE_QUERY",
                    [&](metatrace::Record* r) { r->AddArg("query", sql); });

  uint32_t sql_stats_row =
      context_.storage->mutable_sql_stats()->RecordQueryBegin(
          sql, base::GetWallTimeNs().count());
  std::string non_breaking_sql = base::ReplaceAll(sql, "\u00A0", " ");
  base::StatusOr<PerfettoSqlEngine::ExecutionResult> result =
      engine_->ExecuteUntilLastStatement(
          SqlSource::FromExecuteQuery(std::move(non_breaking_sql)));
  std::unique_ptr<IteratorImpl> impl(
      new IteratorImpl(this, std::move(result), sql_stats_row));
  return Iterator(std::move(impl));
}

void TraceProcessorImpl::InterruptQuery() {
  if (!engine_->sqlite_engine()->db())
    return;
  query_interrupted_.store(true);
  sqlite3_interrupt(engine_->sqlite_engine()->db());
}

bool TraceProcessorImpl::IsRootMetricField(const std::string& metric_name) {
  std::optional<uint32_t> desc_idx =
      pool_.FindDescriptorIdx(".perfetto.protos.TraceMetrics");
  if (!desc_idx.has_value())
    return false;
  const auto* field_idx =
      pool_.descriptors()[*desc_idx].FindFieldByName(metric_name);
  return field_idx != nullptr;
}

base::Status TraceProcessorImpl::RegisterSqlPackage(SqlPackage sql_package) {
  sql_modules::RegisteredPackage new_package;
  std::string name = sql_package.name;
  if (engine_->FindPackage(name) && !sql_package.allow_override) {
    return base::ErrStatus(
        "Package '%s' is already registered. Choose a different name.\n"
        "If you want to replace the existing package using trace processor "
        "shell, you need to pass the --dev flag and use "
        "--override-sql-module "
        "to pass the module path.",
        name.c_str());
  }
  for (auto const& module_name_and_sql : sql_package.modules) {
    if (sql_modules::GetPackageName(module_name_and_sql.first) != name) {
      return base::ErrStatus(
          "Module name doesn't match the package name. First part of module "
          "name should be package name. Import key: '%s', package name: '%s'.",
          module_name_and_sql.first.c_str(), name.c_str());
    }
    new_package.modules.Insert(module_name_and_sql.first,
                               {module_name_and_sql.second, false});
  }
  manually_registered_sql_packages_.push_back(SqlPackage(sql_package));
  engine_->RegisterPackage(name, std::move(new_package));
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
  }

  if (metric.proto_field_name) {
    InsertIntoTraceMetricsTable(engine_->sqlite_engine()->db(),
                                *metric.proto_field_name);
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
  RETURN_IF_ERROR(pool_.AddFromFileDescriptorSet(data, size, skip_prefixes));
  RETURN_IF_ERROR(RegisterAllProtoBuilderFunctions(
      &pool_, &proto_fn_name_to_path_, engine_.get(), this));
  return base::OkStatus();
}

base::Status TraceProcessorImpl::ComputeMetric(
    const std::vector<std::string>& metric_names,
    std::vector<uint8_t>* metrics_proto) {
  auto opt_idx = pool_.FindDescriptorIdx(".perfetto.protos.TraceMetrics");
  if (!opt_idx.has_value())
    return base::Status("Root metrics proto descriptor not found");

  const auto& root_descriptor = pool_.descriptors()[opt_idx.value()];
  return metrics::ComputeMetrics(engine_.get(), metric_names, sql_metrics_,
                                 pool_, root_descriptor, metrics_proto);
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

void TraceProcessorImpl::InitPerfettoSqlEngine() {
  engine_.reset(new PerfettoSqlEngine(context_.storage->mutable_string_pool(),
                                      config_.enable_extra_checks));
  sqlite3* db = engine_->sqlite_engine()->db();
  sqlite3_str_split_init(db);

  // Register SQL functions only used in local development instances.
  if (config_.enable_dev_features) {
    RegisterFunction<WriteFile>(engine_.get(), "WRITE_FILE", 2);
  }
  RegisterFunction<Glob>(engine_.get(), "glob", 2);
  RegisterFunction<Hash>(engine_.get(), "HASH", -1);
  RegisterFunction<Base64Encode>(engine_.get(), "BASE64_ENCODE", 1);
  RegisterFunction<Demangle>(engine_.get(), "DEMANGLE", 1);
  RegisterFunction<SourceGeq>(engine_.get(), "SOURCE_GEQ", -1);
  RegisterFunction<TablePtrBind>(engine_.get(), "__intrinsic_table_ptr_bind",
                                 -1);
  RegisterFunction<ExportJson>(engine_.get(), "EXPORT_JSON", 1,
                               context_.storage.get(), false);
  RegisterFunction<ExtractArg>(engine_.get(), "EXTRACT_ARG", 2,
                               context_.storage.get());
  RegisterFunction<AbsTimeStr>(engine_.get(), "ABS_TIME_STR", 1,
                               context_.clock_converter.get());
  RegisterFunction<Reverse>(engine_.get(), "REVERSE", 1);
  RegisterFunction<ToMonotonic>(engine_.get(), "TO_MONOTONIC", 1,
                                context_.clock_converter.get());
  RegisterFunction<ToRealtime>(engine_.get(), "TO_REALTIME", 1,
                               context_.clock_converter.get());
  RegisterFunction<ToTimecode>(engine_.get(), "TO_TIMECODE", 1);
  RegisterFunction<CreateFunction>(engine_.get(), "CREATE_FUNCTION", 3,
                                   engine_.get());
  RegisterFunction<CreateViewFunction>(engine_.get(), "CREATE_VIEW_FUNCTION", 3,
                                       engine_.get());
  RegisterFunction<ExperimentalMemoize>(engine_.get(), "EXPERIMENTAL_MEMOIZE",
                                        1, engine_.get());
  RegisterFunction<Import>(
      engine_.get(), "IMPORT", 1,
      std::make_unique<Import::Context>(Import::Context{engine_.get()}));
  RegisterFunction<ToFtrace>(
      engine_.get(), "TO_FTRACE", 1,
      std::make_unique<ToFtrace::Context>(ToFtrace::Context{
          context_.storage.get(), SystraceSerializer(&context_)}));

  if constexpr (regex::IsRegexSupported()) {
    RegisterFunction<Regex>(engine_.get(), "regexp", 2);
  }
  // Old style function registration.
  // TODO(lalitm): migrate this over to using RegisterFunction once aggregate
  // functions are supported.
  RegisterValueAtMaxTsFunction(*engine_);
  {
    base::Status status = RegisterLastNonNullFunction(*engine_);
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = RegisterStackFunctions(engine_.get(), &context_);
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = PprofFunctions::Register(*engine_, &context_);
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = RegisterLayoutFunctions(*engine_);
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = RegisterMathFunctions(*engine_);
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = RegisterBase64Functions(*engine_);
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = RegisterTypeBuilderFunctions(*engine_);
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = RegisterGraphScanFunctions(
        *engine_, context_.storage->mutable_string_pool());
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = RegisterGraphTraversalFunctions(
        *engine_, *context_.storage->mutable_string_pool());
    if (!status.ok())
      PERFETTO_FATAL("%s", status.c_message());
  }
  {
    base::Status status = perfetto_sql::RegisterIntervalIntersectFunctions(
        *engine_, context_.storage->mutable_string_pool());
  }
  {
    base::Status status = perfetto_sql::RegisterCounterIntervalsFunctions(
        *engine_, context_.storage->mutable_string_pool());
  }

  TraceStorage* storage = context_.storage.get();

  // Operator tables.
  engine_->sqlite_engine()->RegisterVirtualTableModule<SpanJoinOperatorModule>(
      "span_join",
      std::make_unique<SpanJoinOperatorModule::Context>(engine_.get()));
  engine_->sqlite_engine()->RegisterVirtualTableModule<SpanJoinOperatorModule>(
      "span_left_join",
      std::make_unique<SpanJoinOperatorModule::Context>(engine_.get()));
  engine_->sqlite_engine()->RegisterVirtualTableModule<SpanJoinOperatorModule>(
      "span_outer_join",
      std::make_unique<SpanJoinOperatorModule::Context>(engine_.get()));
  engine_->sqlite_engine()->RegisterVirtualTableModule<WindowOperatorModule>(
      "window", std::make_unique<WindowOperatorModule::Context>());
  engine_->sqlite_engine()->RegisterVirtualTableModule<CounterMipmapOperator>(
      "__intrinsic_counter_mipmap",
      std::make_unique<CounterMipmapOperator::Context>(engine_.get()));
  engine_->sqlite_engine()->RegisterVirtualTableModule<SliceMipmapOperator>(
      "__intrinsic_slice_mipmap",
      std::make_unique<SliceMipmapOperator::Context>(engine_.get()));

  // Register stdlib packages.
  auto packages = GetStdlibPackages();
  for (auto package = packages.GetIterator(); package; ++package) {
    base::Status status =
        RegisterSqlPackage({/*name=*/package.key(), /*modules=*/package.value(),
                            /*allow_override=*/false});
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }

  // Register metrics functions.
  {
    base::Status status =
        engine_->RegisterSqliteAggregateFunction<metrics::RepeatedField>(
            nullptr);
    if (!status.ok())
      PERFETTO_ELOG("%s", status.c_message());
  }

  RegisterFunction<metrics::NullIfEmpty>(engine_.get(), "NULL_IF_EMPTY", 1);
  RegisterFunction<metrics::UnwrapMetricProto>(engine_.get(),
                                               "UNWRAP_METRIC_PROTO", 2);
  RegisterFunction<metrics::RunMetric>(
      engine_.get(), "RUN_METRIC", -1,
      std::make_unique<metrics::RunMetric::Context>(
          metrics::RunMetric::Context{engine_.get(), &sql_metrics_}));

  // Legacy tables.
  engine_->sqlite_engine()->RegisterVirtualTableModule<SqlStatsModule>(
      "sqlstats", storage);
  engine_->sqlite_engine()->RegisterVirtualTableModule<StatsModule>("stats",
                                                                    storage);
  engine_->sqlite_engine()->RegisterVirtualTableModule<TablePointerModule>(
      "__intrinsic_table_ptr", nullptr);

  // New style db-backed tables.
  // Note: if adding a table here which might potentially contain many rows
  // (O(rows in sched/slice/counter)), then consider calling ShrinkToFit on
  // that table in TraceStorage::ShrinkToFitTables.
  RegisterStaticTable(storage->mutable_machine_table());
  RegisterStaticTable(storage->mutable_arg_table());
  RegisterStaticTable(storage->mutable_raw_table());
  RegisterStaticTable(storage->mutable_ftrace_event_table());
  RegisterStaticTable(storage->mutable_thread_table());
  RegisterStaticTable(storage->mutable_process_table());
  RegisterStaticTable(storage->mutable_filedescriptor_table());
  RegisterStaticTable(storage->mutable_trace_file_table());

  RegisterStaticTable(storage->mutable_slice_table());
  RegisterStaticTable(storage->mutable_flow_table());
  RegisterStaticTable(storage->mutable_sched_slice_table());
  RegisterStaticTable(storage->mutable_spurious_sched_wakeup_table());
  RegisterStaticTable(storage->mutable_thread_state_table());
  RegisterStaticTable(storage->mutable_gpu_slice_table());

  RegisterStaticTable(storage->mutable_track_table());
  RegisterStaticTable(storage->mutable_thread_track_table());
  RegisterStaticTable(storage->mutable_process_track_table());
  RegisterStaticTable(storage->mutable_cpu_track_table());
  RegisterStaticTable(storage->mutable_gpu_track_table());

  RegisterStaticTable(storage->mutable_counter_table());

  RegisterStaticTable(storage->mutable_counter_track_table());
  RegisterStaticTable(storage->mutable_process_counter_track_table());
  RegisterStaticTable(storage->mutable_thread_counter_track_table());
  RegisterStaticTable(storage->mutable_cpu_counter_track_table());
  RegisterStaticTable(storage->mutable_gpu_counter_track_table());
  RegisterStaticTable(storage->mutable_gpu_counter_group_table());
  RegisterStaticTable(storage->mutable_perf_counter_track_table());

  RegisterStaticTable(storage->mutable_heap_graph_object_table());
  RegisterStaticTable(storage->mutable_heap_graph_reference_table());
  RegisterStaticTable(storage->mutable_heap_graph_class_table());

  RegisterStaticTable(storage->mutable_symbol_table());
  RegisterStaticTable(storage->mutable_heap_profile_allocation_table());
  RegisterStaticTable(storage->mutable_cpu_profile_stack_sample_table());
  RegisterStaticTable(storage->mutable_perf_session_table());
  RegisterStaticTable(storage->mutable_perf_sample_table());
  RegisterStaticTable(storage->mutable_instruments_sample_table());
  RegisterStaticTable(storage->mutable_stack_profile_callsite_table());
  RegisterStaticTable(storage->mutable_stack_profile_mapping_table());
  RegisterStaticTable(storage->mutable_stack_profile_frame_table());
  RegisterStaticTable(storage->mutable_package_list_table());
  RegisterStaticTable(storage->mutable_profiler_smaps_table());

  RegisterStaticTable(storage->mutable_android_log_table());
  RegisterStaticTable(storage->mutable_android_dumpstate_table());
  RegisterStaticTable(storage->mutable_android_game_intervenion_list_table());
  RegisterStaticTable(storage->mutable_android_key_events_table());
  RegisterStaticTable(storage->mutable_android_motion_events_table());
  RegisterStaticTable(storage->mutable_android_input_event_dispatch_table());

  RegisterStaticTable(storage->mutable_vulkan_memory_allocations_table());

  RegisterStaticTable(storage->mutable_graphics_frame_slice_table());

  RegisterStaticTable(storage->mutable_expected_frame_timeline_slice_table());
  RegisterStaticTable(storage->mutable_actual_frame_timeline_slice_table());

  RegisterStaticTable(storage->mutable_android_network_packets_table());

  RegisterStaticTable(storage->mutable_v8_isolate_table());
  RegisterStaticTable(storage->mutable_v8_js_script_table());
  RegisterStaticTable(storage->mutable_v8_wasm_script_table());
  RegisterStaticTable(storage->mutable_v8_js_function_table());
  RegisterStaticTable(storage->mutable_v8_js_code_table());
  RegisterStaticTable(storage->mutable_v8_internal_code_table());
  RegisterStaticTable(storage->mutable_v8_wasm_code_table());
  RegisterStaticTable(storage->mutable_v8_regexp_code_table());

  RegisterStaticTable(storage->mutable_jit_code_table());
  RegisterStaticTable(storage->mutable_jit_frame_table());

  RegisterStaticTable(storage->mutable_spe_record_table());

  RegisterStaticTable(storage->mutable_inputmethod_clients_table());
  RegisterStaticTable(storage->mutable_inputmethod_manager_service_table());
  RegisterStaticTable(storage->mutable_inputmethod_service_table());

  RegisterStaticTable(storage->mutable_surfaceflinger_layers_snapshot_table());
  RegisterStaticTable(storage->mutable_surfaceflinger_layer_table());
  RegisterStaticTable(storage->mutable_surfaceflinger_transactions_table());

  RegisterStaticTable(storage->mutable_viewcapture_table());

  RegisterStaticTable(storage->mutable_windowmanager_table());

  RegisterStaticTable(
      storage->mutable_window_manager_shell_transitions_table());
  RegisterStaticTable(
      storage->mutable_window_manager_shell_transition_handlers_table());

  RegisterStaticTable(storage->mutable_protolog_table());

  RegisterStaticTable(storage->mutable_metadata_table());
  RegisterStaticTable(storage->mutable_cpu_table());
  RegisterStaticTable(storage->mutable_cpu_freq_table());
  RegisterStaticTable(storage->mutable_clock_snapshot_table());

  RegisterStaticTable(storage->mutable_memory_snapshot_table());
  RegisterStaticTable(storage->mutable_process_memory_snapshot_table());
  RegisterStaticTable(storage->mutable_memory_snapshot_node_table());
  RegisterStaticTable(storage->mutable_memory_snapshot_edge_table());

  RegisterStaticTable(storage->mutable_experimental_proto_path_table());
  RegisterStaticTable(storage->mutable_experimental_proto_content_table());

  RegisterStaticTable(
      storage->mutable_experimental_missing_chrome_processes_table());

  // Tables dynamically generated at query time.
  engine_->RegisterStaticTableFunction(
      std::make_unique<ExperimentalFlamegraph>(&context_));
  engine_->RegisterStaticTableFunction(
      std::make_unique<ExperimentalCounterDur>(storage->counter_table()));
  engine_->RegisterStaticTableFunction(
      std::make_unique<ExperimentalSliceLayout>(
          context_.storage->mutable_string_pool(), &storage->slice_table()));
  engine_->RegisterStaticTableFunction(std::make_unique<TableInfo>(
      context_.storage->mutable_string_pool(), engine_.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<Ancestor>(
      Ancestor::Type::kSlice, context_.storage.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<Ancestor>(
      Ancestor::Type::kStackProfileCallsite, context_.storage.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<Ancestor>(
      Ancestor::Type::kSliceByStack, context_.storage.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<Descendant>(
      Descendant::Type::kSlice, context_.storage.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<Descendant>(
      Descendant::Type::kSliceByStack, context_.storage.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<ConnectedFlow>(
      ConnectedFlow::Mode::kDirectlyConnectedFlow, context_.storage.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<ConnectedFlow>(
      ConnectedFlow::Mode::kPrecedingFlow, context_.storage.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<ConnectedFlow>(
      ConnectedFlow::Mode::kFollowingFlow, context_.storage.get()));
  engine_->RegisterStaticTableFunction(std::make_unique<ExperimentalSchedUpid>(
      storage->sched_slice_table(), storage->thread_table()));
  engine_->RegisterStaticTableFunction(
      std::make_unique<ExperimentalAnnotatedStack>(&context_));
  engine_->RegisterStaticTableFunction(
      std::make_unique<ExperimentalFlatSlice>(&context_));
  engine_->RegisterStaticTableFunction(std::make_unique<DfsWeightBounded>(
      context_.storage->mutable_string_pool()));
  engine_->RegisterStaticTableFunction(
      std::make_unique<WinscopeProtoToArgsWithDefaults>(
          context_.storage->mutable_string_pool(), engine_.get(), &context_));

  // Value table aggregate functions.
  engine_->RegisterSqliteAggregateFunction<DominatorTree>(
      context_.storage->mutable_string_pool());
  engine_->RegisterSqliteAggregateFunction<StructuralTreePartition>(
      context_.storage->mutable_string_pool());

  // Metrics.
  {
    auto status = RegisterAllProtoBuilderFunctions(
        &pool_, &proto_fn_name_to_path_, engine_.get(), this);
    if (!status.ok()) {
      PERFETTO_FATAL("%s", status.c_message());
    }
  }

  // Import prelude package.
  IncludeBeforeEofPrelude();
  if (notify_eof_called_) {
    IncludeAfterEofPrelude();
  }

  for (const auto& metric : sql_metrics_) {
    if (metric.proto_field_name) {
      InsertIntoTraceMetricsTable(db, *metric.proto_field_name);
    }
  }

  // Fill trace bounds table.
  BuildBoundsTable(db, GetTraceTimestampBoundsNs(*context_.storage));

  // Reregister manually added stdlib packages.
  for (const auto& package : manually_registered_sql_packages_) {
    RegisterSqlPackage(package);
  }
}

void TraceProcessorImpl::IncludeBeforeEofPrelude() {
  auto result = engine_->Execute(SqlSource::FromTraceProcessorImplementation(
      "INCLUDE PERFETTO MODULE prelude.before_eof.*"));
  if (!result.status().ok()) {
    PERFETTO_FATAL("Failed to import prelude: %s", result.status().c_message());
  }
}

void TraceProcessorImpl::IncludeAfterEofPrelude() {
  auto result = engine_->Execute(SqlSource::FromTraceProcessorImplementation(
      "INCLUDE PERFETTO MODULE prelude.after_eof.*"));
  if (!result.status().ok()) {
    PERFETTO_FATAL("Failed to import prelude: %s", result.status().c_message());
  }
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

  auto* clock_snapshot = trace->add_packet()->set_clock_snapshot();
  for (const auto& [clock_id, ts] : base::CaptureClockSnapshots()) {
    auto* clock = clock_snapshot->add_clocks();
    clock->set_clock_id(clock_id);
    clock->set_timestamp(ts);
  }

  auto tid = static_cast<uint32_t>(base::GetThreadId());
  base::FlatHashMap<std::string, uint64_t> interned_strings;
  metatrace::DisableAndReadBuffer(
      [&trace, &interned_strings, tid](metatrace::Record* record) {
        auto* packet = trace->add_packet();
        packet->set_timestamp(record->timestamp_ns);
        auto* evt = packet->set_perfetto_metatrace();

        StringInterner interner(*evt, interned_strings);

        evt->set_event_name_iid(interner.InternString(record->event_name));
        evt->set_event_duration_ns(record->duration_ns);
        evt->set_thread_id(tid);

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

}  // namespace perfetto::trace_processor
