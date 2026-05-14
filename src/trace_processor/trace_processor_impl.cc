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
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/thread_utils.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/clock_snapshots.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/summarizer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/android_bugreport/android_dumpstate_event_parser.h"
#include "src/trace_processor/importers/android_bugreport/android_dumpstate_reader.h"
#include "src/trace_processor/importers/android_bugreport/android_log_event_parser.h"
#include "src/trace_processor/importers/android_bugreport/android_log_reader.h"
#include "src/trace_processor/importers/archive/gzip_trace_parser.h"
#include "src/trace_processor/importers/archive/tar_trace_reader.h"
#include "src/trace_processor/importers/archive/zip_trace_reader.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_parser.h"
#include "src/trace_processor/importers/art_method/art_method_tokenizer.h"
#include "src/trace_processor/importers/art_method/art_method_v2_tokenizer.h"
#include "src/trace_processor/importers/collapsed_stack/collapsed_stack_trace_reader.h"
#include "src/trace_processor/importers/common/registered_file_tracker.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_tokenizer.h"
#include "src/trace_processor/importers/gecko/gecko_trace_tokenizer.h"
#include "src/trace_processor/importers/json/json_trace_tokenizer.h"
#include "src/trace_processor/importers/ninja/ninja_log_parser.h"
#include "src/trace_processor/importers/perf/perf_data_tokenizer.h"
#include "src/trace_processor/importers/perf/record_parser.h"
#include "src/trace_processor/importers/perf/spe_record_parser.h"
#include "src/trace_processor/importers/perf_text/perf_text_trace_tokenizer.h"
#include "src/trace_processor/importers/pprof/pprof_trace_reader.h"
#include "src/trace_processor/importers/primes/primes_trace_tokenizer.h"
#include "src/trace_processor/importers/proto/additional_modules.h"
#include "src/trace_processor/importers/proto/deobfuscation_tracker.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/importers/simpleperf_proto/simpleperf_proto_tokenizer.h"
#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"
#include "src/trace_processor/iterator_impl.h"
#include "src/trace_processor/metrics/all_chrome_metrics.descriptor.h"
#include "src/trace_processor/metrics/all_webview_metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.descriptor.h"
#include "src/trace_processor/metrics/metrics.h"
#include "src/trace_processor/metrics/sql/amalgamated_sql_metrics.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/ancestor.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/connected_flow.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/descendant.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/dfs_weight_bounded.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_annotated_stack.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flamegraph.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flat_slice.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_slice_layout.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/stdlib_docs_table_function.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/table_info.h"
#include "src/trace_processor/perfetto_sql/stdlib/stdlib.h"
#include "src/trace_processor/plugins/args/args.h"
#include "src/trace_processor/plugins/art_heap_graph_functions/art_heap_graph_functions.h"
#include "src/trace_processor/plugins/base64_functions/base64_functions.h"
#include "src/trace_processor/plugins/core_functions/core_functions.h"
#include "src/trace_processor/plugins/counter_intervals/counter_intervals.h"
#include "src/trace_processor/plugins/counter_mipmap_operator/counter_mipmap_operator.h"
#include "src/trace_processor/plugins/create_function/create_function.h"
#include "src/trace_processor/plugins/create_intervals/create_intervals.h"
#include "src/trace_processor/plugins/create_view_function/create_view_function.h"
#include "src/trace_processor/plugins/critical_path/critical_path.h"
#include "src/trace_processor/plugins/developer_functions/developer_functions.h"
#include "src/trace_processor/plugins/dominator_tree/dominator_tree.h"
#include "src/trace_processor/plugins/etm_decode_chunk/etm_decode_chunk.h"
#include "src/trace_processor/plugins/etm_iterate_range/etm_iterate_range.h"
#include "src/trace_processor/plugins/graph_scan/graph_scan.h"
#include "src/trace_processor/plugins/graph_traversal/graph_traversal.h"
#include "src/trace_processor/plugins/import/import.h"
#include "src/trace_processor/plugins/interval_intersect/interval_intersect.h"
#include "src/trace_processor/plugins/layout_functions/layout_functions.h"
#include "src/trace_processor/plugins/math_functions/math_functions.h"
#include "src/trace_processor/plugins/metadata/metadata.h"
#include "src/trace_processor/plugins/package_lookup/package_lookup.h"
#include "src/trace_processor/plugins/perf_counter/perf_counter.h"
#include "src/trace_processor/plugins/pprof_functions/pprof_functions.h"
#include "src/trace_processor/plugins/slice_mipmap_operator/slice_mipmap_operator.h"
#include "src/trace_processor/plugins/span_join_operator/span_join_operator.h"
#include "src/trace_processor/plugins/sql_stats_table/sql_stats_table.h"
#include "src/trace_processor/plugins/stack_functions/stack_functions.h"
#include "src/trace_processor/plugins/string_functions/string_functions.h"
#include "src/trace_processor/plugins/structural_tree_partition/structural_tree_partition.h"
#include "src/trace_processor/plugins/symbolize/symbolize.h"
#include "src/trace_processor/plugins/table_pointer_module/table_pointer_module.h"
#include "src/trace_processor/plugins/time_functions/time_functions.h"
#include "src/trace_processor/plugins/to_ftrace/to_ftrace.h"
#include "src/trace_processor/plugins/tree_functions/tree_functions.h"
#include "src/trace_processor/plugins/type_builder_functions/type_builder_functions.h"
#include "src/trace_processor/plugins/utils_functions/utils_functions.h"
#include "src/trace_processor/plugins/wattson/wattson.h"
#include "src/trace_processor/plugins/window_operator/window_operator.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"   // IWYU pragma: keep
#include "src/trace_processor/tables/jit_tables_py.h"       // IWYU pragma: keep
#include "src/trace_processor/tables/memory_tables_py.h"    // IWYU pragma: keep
#include "src/trace_processor/tables/metadata_tables_py.h"  // IWYU pragma: keep
#include "src/trace_processor/tables/trace_proto_tables_py.h"  // IWYU pragma: keep
#include "src/trace_processor/tables/v8_tables_py.h"        // IWYU pragma: keep
#include "src/trace_processor/tables/winscope_tables_py.h"  // IWYU pragma: keep
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/trace_processor_storage_impl.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/trace_summary/summarizer.h"
#include "src/trace_processor/trace_summary/summary.h"
#include "src/trace_processor/trace_summary/trace_summary.descriptor.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/gzip_utils.h"
#include "src/trace_processor/util/protozero_to_json.h"
#include "src/trace_processor/util/protozero_to_text.h"
#include "src/trace_processor/util/sql_bundle.h"
#include "src/trace_processor/util/sql_modules.h"
#include "src/trace_processor/util/trace_type.h"

#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/perfetto/perfetto_metatrace.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_INSTRUMENTS)
#include "src/trace_processor/importers/instruments/instruments_xml_tokenizer.h"
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ENABLE_ETM_IMPORTER)
#include "src/trace_processor/importers/common/registered_file_tracker.h"
#include "src/trace_processor/importers/etm/etm_v4_stream_demultiplexer.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/importers/perf/perf_tracker.h"
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ENABLE_WINSCOPE)
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/winscope_proto_to_args_with_defaults.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/winscope_surfaceflinger_hierarchy_paths.h"
#endif

namespace perfetto::trace_processor {
namespace {

template <typename SqlFunction, typename Ptr = typename SqlFunction::UserData*>
void RegisterFunction(
    PerfettoSqlConnection* connection,
    Ptr context = nullptr,
    const PerfettoSqlConnection::RegisterFunctionArgs& args = {}) {
  auto status =
      connection->RegisterFunction<SqlFunction>(std::move(context), args);
  if (!status.ok()) {
    const char* name = args.name ? args.name : SqlFunction::kName;
    PERFETTO_FATAL("Failed to register %s function: %s", name,
                   status.c_message());
  }
}

base::Status RegisterAllProtoBuilderFunctions(
    const DescriptorPool* pool,
    std::unordered_map<std::string, std::string>* proto_fn_name_to_path,
    PerfettoSqlConnection* connection,
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
        connection,
        std::make_unique<metrics::BuildProto::UserData>(
            metrics::BuildProto::UserData{tp, pool, i}),
        PerfettoSqlConnection::RegisterFunctionArgs(fn_name.c_str()));
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

template <typename T>
void AddStaticTable(std::vector<PerfettoSqlConnection::StaticTable>& tables,
                    T* table_instance) {
  tables.push_back({
      &table_instance->dataframe(),
      T::Name(),
  });
}

base::StatusOr<sql_modules::RegisteredPackage> ToRegisteredPackage(
    const SqlPackage& package) {
  const std::string& name = package.name;
  sql_modules::RegisteredPackage new_package;
  for (auto const& module_name_and_sql : package.modules) {
    const std::string& module_name = module_name_and_sql.first;
    // Module name must start with package name as prefix (and be longer)
    if (!sql_modules::IsPackagePrefixOf(name, module_name) ||
        name == module_name) {
      return base::ErrStatus(
          "Module name '%s' must start with package name '%s.' as prefix.",
          module_name.c_str(), name.c_str());
    }
    new_package.modules.Insert(module_name, module_name_and_sql.second);
  }
  return base::StatusOr<sql_modules::RegisteredPackage>(std::move(new_package));
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

void InsertIntoBuildFlagsTable(tables::BuildFlagsTable* table,
                               StringPool* string_pool) {
  for (int i = 0; i < kPerfettoBuildFlagsCount; ++i) {
    const auto& build_flag = kPerfettoBuildFlags[i];
    tables::BuildFlagsTable::Row row;
    row.name = string_pool->InternString(build_flag.name);
    row.enabled = static_cast<uint32_t>(build_flag.value);
    table->Insert(row);
  }
}

void InsertIntoModulesTable(tables::ModulesTable* table,
                            StringPool* string_pool) {
  base::ignore_result(table, string_pool);

#if PERFETTO_BUILDFLAG(PERFETTO_ENABLE_ETM_IMPORTER)
  table->Insert({string_pool->InternString("etm")});
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ENABLE_ETM_IMPORTER)

#if PERFETTO_BUILDFLAG(PERFETTO_ENABLE_WINSCOPE)
  table->Insert({string_pool->InternString("winscope")});
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ENABLE_WINSCOPE)

#if PERFETTO_BUILDFLAG(PERFETTO_LLVM_SYMBOLIZER)
  table->Insert({string_pool->InternString("llvm_symbolizer")});
#endif  // PERFETTO_BUILDFLAG(PERFETTO_LLVM_SYMBOLIZER)
}

sql_modules::NameToPackage GetStdlibPackages() {
  sql_modules::NameToPackage packages;
  for (const auto& file_to_sql : SqlBundle(stdlib::kStdlib)) {
    std::string module_name = sql_modules::GetIncludeKey(file_to_sql.path);
    std::string package_name = sql_modules::GetPackageName(module_name);
    packages.Insert(package_name, {})
        .first->emplace_back(module_name, file_to_sql.sql_view());
  }
  return packages;
}

// IMPORTANT: GetBoundsMutationCount and GetTraceTimestampBoundsNs must be kept
// in sync.
uint64_t GetBoundsMutationCount(const TraceStorage& storage) {
  return storage.ftrace_event_table().mutations() +
         storage.sched_slice_table().mutations() +
         storage.counter_table().mutations() +
         storage.slice_table().mutations() +
         storage.heap_profile_allocation_table().mutations() +
         storage.thread_state_table().mutations() +
         storage.android_log_table().mutations() +
         storage.heap_graph_object_table().mutations() +
         storage.perf_sample_table().mutations() +
         storage.instruments_sample_table().mutations() +
         storage.cpu_profile_stack_sample_table().mutations();
}

// IMPORTANT: GetBoundsMutationCount and GetTraceTimestampBoundsNs must be kept
// in sync.
std::pair<int64_t, int64_t> GetTraceTimestampBoundsNs(
    const TraceStorage& storage) {
  int64_t start_ns = std::numeric_limits<int64_t>::max();
  int64_t end_ns = 0;
  for (auto it = storage.ftrace_event_table().IterateRows(); it; ++it) {
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
  // TODO(lalitm): plugins should self-register via PERFETTO_TP_REGISTER_PLUGIN
  // (a global static initializer). That's currently disabled due to build-time
  // issues, so instead each plugin exposes an explicit Register* function that
  // we call here before GetPluginSet() builds its cached set. Remove these
  // explicit calls once the static-init based registration is restored.
  args::RegisterPlugin();
  art_heap_graph_functions::RegisterPlugin();
  base64_functions::RegisterPlugin();
  core_functions::RegisterPlugin();
  counter_intervals::RegisterPlugin();
  counter_mipmap_operator::RegisterPlugin();
  create_function::RegisterPlugin();
  create_intervals::RegisterPlugin();
  create_view_function::RegisterPlugin();
  critical_path::RegisterPlugin();
  developer_functions::RegisterPlugin();
  dominator_tree::RegisterPlugin();
  etm_decode_chunk::RegisterPlugin();
  etm_iterate_range::RegisterPlugin();
  graph_scan::RegisterPlugin();
  graph_traversal::RegisterPlugin();
  import::RegisterPlugin();
  interval_intersect::RegisterPlugin();
  layout_functions::RegisterPlugin();
  math_functions::RegisterPlugin();
  metadata::RegisterPlugin();
  package_lookup::RegisterPlugin();
  perf_counter::RegisterPlugin();
  pprof_functions::RegisterPlugin();
  slice_mipmap_operator::RegisterPlugin();
  span_join_operator::RegisterPlugin();
  sql_stats_table::RegisterPlugin();
  stack_functions::RegisterPlugin();
  string_functions::RegisterPlugin();
  structural_tree_partition::RegisterPlugin();
  symbolize::RegisterPlugin();
  table_pointer_module::RegisterPlugin();
  time_functions::RegisterPlugin();
  to_ftrace::RegisterPlugin();
  tree_functions::RegisterPlugin();
  type_builder_functions::RegisterPlugin();
  utils_functions::RegisterPlugin();
  wattson::RegisterPlugin();
  window_operator::RegisterPlugin();

  // Initialize plugins using the statically pre-computed PluginSet.
  // Dep indices are resolved once at static init time; here we just
  // instantiate, resolve dep pointers, and register importers.
  {
    const PluginSet& pset = GetPluginSet();
    plugins_.reserve(pset.entries.size());
    for (const auto& pse : pset.entries) {
      plugins_.push_back(pse.factory());
    }
    for (size_t i = 0; i < pset.entries.size(); ++i) {
      auto& p = *plugins_[i];
      p.trace_context_ = context();
      for (size_t dep_idx : pset.entries[i].dep_indices) {
        p.resolved_deps_.push_back(plugins_[dep_idx].get());
      }
    }
    // Let plugins register trace readers and proto importer modules.
    for (auto& p : plugins_) {
      p->RegisterImporters(*context()->reader_registry);
    }
  }
  context()->register_additional_proto_modules = &RegisterAdditionalModules;

#if PERFETTO_BUILDFLAG(PERFETTO_ENABLE_ETM_IMPORTER)
  context()->perf_aux_tokenizer_registrations.push_back(
      [](perf_importer::PerfTracker* pt) {
        pt->RegisterAuxTokenizer(PERF_AUXTRACE_CS_ETM,
                                 etm::CreateEtmV4StreamDemultiplexer);
      });
#endif
  context()->reader_registry->RegisterTraceReader<AndroidDumpstateReader>(
      kAndroidDumpstateTraceType);
  context()->reader_registry->RegisterTraceReader<AndroidLogReader>(
      kAndroidLogcatTraceType);
  context()->reader_registry->RegisterTraceReader<FuchsiaTraceTokenizer>(
      kFuchsiaTraceType);
  context()->reader_registry->RegisterTraceReader<SystraceTraceParser>(
      kSystraceTraceType);
  context()->reader_registry->RegisterTraceReader<NinjaLogParser>(
      kNinjaLogTraceType);
  context()->reader_registry->RegisterTraceReader<PprofTraceReader>(
      kPprofTraceType);
  context()->reader_registry->RegisterTraceReader<CollapsedStackTraceReader>(
      kCollapsedStackTraceType);
  context()
      ->reader_registry->RegisterTraceReader<perf_importer::PerfDataTokenizer>(
          kPerfDataTraceType);
#if PERFETTO_BUILDFLAG(PERFETTO_TP_INSTRUMENTS)
  context()
      ->reader_registry
      ->RegisterTraceReader<instruments_importer::InstrumentsXmlTokenizer>(
          kInstrumentsXmlTraceType);
#endif
  if constexpr (util::IsGzipSupported()) {
    context()->reader_registry->RegisterTraceReader<GzipTraceParser>(
        kGzipTraceType);
    context()->reader_registry->RegisterTraceReader<GzipTraceParser>(
        kCtraceTraceType);
    context()->reader_registry->RegisterTraceReader<ZipTraceReader>(kZipFile);
  }
  context()->reader_registry->RegisterTraceReader<JsonTraceTokenizer>(
      kJsonTraceType);
  context()
      ->reader_registry
      ->RegisterTraceReader<gecko_importer::GeckoTraceTokenizer>(
          kGeckoTraceType);
  context()
      ->reader_registry->RegisterTraceReader<art_method::ArtMethodTokenizer>(
          kArtMethodTraceType);
  context()
      ->reader_registry->RegisterTraceReader<art_method::ArtMethodV2Tokenizer>(
          kArtMethodV2TraceType);
  context()->reader_registry->RegisterTraceReader<art_hprof::ArtHprofParser>(
      kArtHprofTraceType);
  context()
      ->reader_registry
      ->RegisterTraceReader<perf_text_importer::PerfTextTraceTokenizer>(
          kPerfTextTraceType);
  context()
      ->reader_registry->RegisterTraceReader<
          simpleperf_proto_importer::SimpleperfProtoTokenizer>(
          kSimpleperfProtoTraceType);
  context()->reader_registry->RegisterTraceReader<TarTraceReader>(
      kTarTraceType);
  context()->reader_registry->RegisterTraceReader<primes::PrimesTraceTokenizer>(
      kPrimesTraceType);

  // Force initialization of heap graph tracker.
  //
  // TODO(lalitm): remove heap graph tracker from global context and get rid
  // of this.
  context()->heap_graph_tracker = std::make_unique<HeapGraphTracker>(
      context()->storage.get(), context()->global_stats_tracker.get());

  // Initialize deobfuscation tracker.
  context()->deobfuscation_tracker =
      std::make_unique<DeobfuscationTracker>(context());

  const std::vector<std::string> sanitized_extension_paths =
      SanitizeMetricMountPaths(config_.skip_builtin_metric_paths);
  std::vector<std::string> skip_prefixes;
  skip_prefixes.reserve(sanitized_extension_paths.size());
  for (const auto& path : sanitized_extension_paths) {
    skip_prefixes.push_back(kMetricProtoRoot + path);
  }

  // Add metrics to descriptor pool
  metrics_descriptor_pool_.AddFromFileDescriptorSet(
      kMetricsDescriptor.data(), kMetricsDescriptor.size(), skip_prefixes);
  metrics_descriptor_pool_.AddFromFileDescriptorSet(
      kAllChromeMetricsDescriptor.data(), kAllChromeMetricsDescriptor.size(),
      skip_prefixes);
  metrics_descriptor_pool_.AddFromFileDescriptorSet(
      kAllWebviewMetricsDescriptor.data(), kAllWebviewMetricsDescriptor.size(),
      skip_prefixes);

  // Add the summary descriptor to the summary pool.
  {
    base::Status status = context()->descriptor_pool_->AddFromFileDescriptorSet(
        kTraceSummaryDescriptor.data(), kTraceSummaryDescriptor.size());
    PERFETTO_CHECK(status.ok());
  }

  // Register stdlib packages.
  auto packages = GetStdlibPackages();
  for (auto package = packages.GetIterator(); package; ++package) {
    registered_sql_packages_.emplace_back<SqlPackage>({
        /*name=*/package.key(),
        /*modules=*/package.value(),
        /*allow_override=*/false,
    });
  }

  // Compute initial trace bounds before any tables are finalized.
  cached_trace_bounds_ = GetTraceTimestampBoundsNs(*context()->storage);

  engine_ = InitPerfettoSqlConnection({
      context(),
      context()->storage.get(),
      config_,
      registered_sql_packages_,
      sql_metrics_,
      &metrics_descriptor_pool_,
      &proto_fn_name_to_path_,
      this,
      notify_eof_called_,
      cached_trace_bounds_,
      plugins_,
  });

  sqlite_objects_post_prelude_ = engine_->SqliteRegisteredObjectCount();

  bool skip_all_sql = std::find(config_.skip_builtin_metric_paths.begin(),
                                config_.skip_builtin_metric_paths.end(),
                                "") != config_.skip_builtin_metric_paths.end();
  if (!skip_all_sql) {
    for (const auto& file_to_sql :
         SqlBundle(sql_metrics::kAmalgamatedSqlMetrics)) {
      if (base::StartsWithAny(file_to_sql.path, sanitized_extension_paths))
        continue;
      RegisterMetric(file_to_sql.path, std::string(file_to_sql.sql_view()));
    }
  }

  InsertIntoBuildFlagsTable(context()->storage->mutable_build_flags_table(),
                            context()->storage->mutable_string_pool());
  InsertIntoModulesTable(context()->storage->mutable_modules_table(),
                         context()->storage->mutable_string_pool());
}

TraceProcessorImpl::~TraceProcessorImpl() = default;

// =================================================================
// |        TraceProcessorStorage implementation starts here       |
// =================================================================

base::Status TraceProcessorImpl::Parse(TraceBlobView blob) {
  bytes_parsed_ += blob.size();
  return TraceProcessorStorageImpl::Parse(std::move(blob));
}

void TraceProcessorImpl::Flush() {
  TraceProcessorStorageImpl::Flush();
  CacheBoundsAndBuildTable();
}

base::Status TraceProcessorImpl::NotifyEndOfFile() {
  if (notify_eof_called_) {
    constexpr char kMessage[] =
        "NotifyEndOfFile should only be called once. Try calling Flush instead "
        "if trying to commit the contents of the trace to tables.";
    PERFETTO_ELOG(kMessage);
    return base::ErrStatus(kMessage);
  }
  eof_ = true;
  notify_eof_called_ = true;

  if (current_trace_name_.empty()) {
    current_trace_name_ = "Unnamed trace";
  }

  // Note: we very intentionally do not call
  // TraceProcessorStorageImpl::NotifyEndOfFile as we have very special
  // ordering requirements on how we need to push data to the sorter and
  // finalize trackers. In any case, all logic of TraceProcessorStorage
  // is confined to OnPushDataToSorter and OnEventsFullyExtracted,
  // so we can just call those directly here.

  // Stage 1: push all data to the sorter
  RETURN_IF_ERROR(TraceProcessorStorageImpl::OnPushDataToSorter());

  // Stage 2: finalize all data.
  HeapGraphTracker::Get(context())->FinalizeAllProfiles();
  TraceProcessorStorageImpl::OnEventsFullyExtracted();
  DeobfuscationTracker::Get(context())->OnEventsFullyExtracted();
  CacheBoundsAndBuildTable();

  // Stage 3: reduce memory usage by both destroying parser context *and*
  // finalizing dataframes.
  TraceProcessorStorageImpl::DestroyContext();
  for (const auto& table : GetStaticTables(context()->storage.get())) {
    table.dataframe->Finalize();
  }
  // Also finalize plugin-owned tables.
  {
    std::vector<PluginDataframe> plugin_tables;
    for (auto& p : plugins_) {
      p->RegisterDataframes(plugin_tables);
    }
    for (const auto& table : plugin_tables) {
      table.dataframe->Finalize();
    }
  }

  // Stage 4: prepare the connection for queries.
  IncludeAfterEofPrelude(engine_.get());
  sqlite_objects_post_prelude_ = engine_->SqliteRegisteredObjectCount();

  return base::OkStatus();
}

void TraceProcessorImpl::CacheBoundsAndBuildTable() {
  uint64_t mutations = GetBoundsMutationCount(*context()->storage);
  if (mutations == bounds_tables_mutations_) {
    return;
  }
  bounds_tables_mutations_ = mutations;
  cached_trace_bounds_ = GetTraceTimestampBoundsNs(*context()->storage);
  BuildBoundsTable(engine_->sqlite_connection()->db(), cached_trace_bounds_);
}

// =================================================================
// |        PerfettoSQL related functionality starts here          |
// =================================================================

Iterator TraceProcessorImpl::ExecuteQuery(const std::string& sql) {
  PERFETTO_TP_TRACE(metatrace::Category::API_TIMELINE, "EXECUTE_QUERY",
                    [&](metatrace::Record* r) { r->AddArg("query", sql); });

  uint32_t sql_stats_row =
      context()->storage->mutable_sql_stats()->RecordQueryBegin(
          sql, base::GetWallTimeNs().count());
  std::string non_breaking_sql = base::ReplaceAll(sql, "\u00A0", " ");
  base::StatusOr<PerfettoSqlConnection::ExecutionResult> result =
      engine_->ExecuteUntilLastStatement(
          SqlSource::FromExecuteQuery(std::move(non_breaking_sql)));
  std::unique_ptr<IteratorImpl> impl(
      new IteratorImpl(this, std::move(result), sql_stats_row));
  return Iterator(std::move(impl));
}

base::Status TraceProcessorImpl::RegisterSqlPackage(SqlPackage sql_package) {
  const std::string& name = sql_package.name;

  // Check for prefix clashes with existing packages
  std::optional<size_t> same_package_idx;
  for (size_t i = 0; i < registered_sql_packages_.size(); ++i) {
    const std::string& existing_name = registered_sql_packages_[i].name;
    bool is_same_package = (name == existing_name);
    bool has_prefix_clash =
        sql_modules::IsPackagePrefixOf(name, existing_name) ||
        sql_modules::IsPackagePrefixOf(existing_name, name);

    if (is_same_package) {
      // Same package name: only allow if allow_override is set
      if (!sql_package.allow_override) {
        return base::ErrStatus(
            "Package '%s' is already registered. Choose a different name.\n"
            "If you want to replace the existing package using trace processor "
            "shell, you need to pass the --dev flag and use "
            "--override-sql-package to pass the module path.",
            name.c_str());
      }
      same_package_idx = i;
    } else if (has_prefix_clash) {
      // Prefix clash with DIFFERENT package: always fail
      return base::ErrStatus(
          "Package '%s' clashes with existing package '%s'. "
          "Package names cannot be prefixes of each other.",
          name.c_str(), existing_name.c_str());
    }
  }

  ASSIGN_OR_RETURN(auto new_package, ToRegisteredPackage(sql_package));

  // If overriding same package, remove old one first
  if (same_package_idx.has_value()) {
    registered_sql_packages_.erase(registered_sql_packages_.begin() +
                                   static_cast<ptrdiff_t>(*same_package_idx));
    engine_->ErasePackage(name);
  }

  // Save the name before moving sql_package
  std::string pkg_name = name;
  registered_sql_packages_.emplace_back(std::move(sql_package));
  return engine_->RegisterPackage(pkg_name, std::move(new_package));
}

// =================================================================
// |  Trace-based metrics (v2) related functionality starts here   |
// =================================================================

base::Status TraceProcessorImpl::Summarize(
    const TraceSummaryComputationSpec& computation,
    const std::vector<TraceSummarySpecBytes>& specs,
    std::vector<uint8_t>* output,
    const TraceSummaryOutputSpec& output_spec) {
  return summary::Summarize(this, *context()->descriptor_pool_, computation,
                            specs, output, output_spec);
}

// =================================================================
// |        Metatracing related functionality starts here          |
// =================================================================

void TraceProcessorImpl::EnableMetatrace(MetatraceConfig config) {
  metatrace::Enable(config);
}

// =================================================================
// |                      Experimental                             |
// =================================================================

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

// =================================================================
// |              Advanced functionality starts here               |
// =================================================================

std::string TraceProcessorImpl::GetCurrentTraceName() {
  if (current_trace_name_.empty())
    return "";
  auto size = " (" + std::to_string(bytes_parsed_ / 1024 / 1024) + " MB)";
  return current_trace_name_ + size;
}

void TraceProcessorImpl::SetCurrentTraceName(const std::string& name) {
  current_trace_name_ = name;
}

base::Status TraceProcessorImpl::RegisterFileContent(const std::string& path,
                                                     TraceBlob content) {
  return context_.registered_file_tracker->AddFile(path, std::move(content));
}

void TraceProcessorImpl::InterruptQuery() {
  if (!engine_->sqlite_connection()->db())
    return;
  query_interrupted_.store(true);
  sqlite3_interrupt(engine_->sqlite_connection()->db());
}

size_t TraceProcessorImpl::RestoreInitialTables() {
  // We should always have at least as many objects now as we did in the
  // constructor.
  uint64_t registered_count_before = engine_->SqliteRegisteredObjectCount();
  PERFETTO_CHECK(registered_count_before >= sqlite_objects_post_prelude_);

  // Reset the connection (and the database it owns) to its initial state.
  // Pass cached bounds to avoid recomputing them.
  engine_ = InitPerfettoSqlConnection({
      context(),
      context()->storage.get(),
      config_,
      registered_sql_packages_,
      sql_metrics_,
      &metrics_descriptor_pool_,
      &proto_fn_name_to_path_,
      this,
      notify_eof_called_,
      cached_trace_bounds_,
      plugins_,
  });

  // The registered count should now be the same as it was in the constructor.
  uint64_t registered_count_after = engine_->SqliteRegisteredObjectCount();
  PERFETTO_CHECK(registered_count_after == sqlite_objects_post_prelude_);
  return static_cast<size_t>(registered_count_before - registered_count_after);
}

// =================================================================
// |  Trace-based metrics (v1) related functionality starts here   |
// =================================================================

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
    InsertIntoTraceMetricsTable(engine_->sqlite_connection()->db(),
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
  RETURN_IF_ERROR(metrics_descriptor_pool_.AddFromFileDescriptorSet(
      data, size, skip_prefixes));
  RETURN_IF_ERROR(RegisterAllProtoBuilderFunctions(
      &metrics_descriptor_pool_, &proto_fn_name_to_path_, engine_.get(), this));
  return base::OkStatus();
}

base::Status TraceProcessorImpl::ComputeMetric(
    const std::vector<std::string>& metric_names,
    std::vector<uint8_t>* metrics_proto) {
  auto opt_idx = metrics_descriptor_pool_.FindDescriptorIdx(
      ".perfetto.protos.TraceMetrics");
  if (!opt_idx.has_value())
    return base::Status("Root metrics proto descriptor not found");

  const auto& root_descriptor =
      metrics_descriptor_pool_.descriptors()[opt_idx.value()];
  return metrics::ComputeMetrics(engine_.get(), metric_names, sql_metrics_,
                                 metrics_descriptor_pool_, root_descriptor,
                                 metrics_proto);
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
          metrics_descriptor_pool_, ".perfetto.protos.TraceMetrics",
          protozero::ConstBytes{metrics_proto.data(), metrics_proto.size()},
          protozero_to_text::kIncludeNewLines);
      break;
    case TraceProcessor::MetricResultFormat::kJson:
      *metrics_string = protozero_to_json::ProtozeroToJson(
          metrics_descriptor_pool_, ".perfetto.protos.TraceMetrics",
          protozero::ConstBytes{metrics_proto.data(), metrics_proto.size()},
          protozero_to_json::kPretty | protozero_to_json::kInlineErrors |
              protozero_to_json::kInlineAnnotations);
      break;
  }
  return status;
}

std::vector<uint8_t> TraceProcessorImpl::GetMetricDescriptors() {
  return metrics_descriptor_pool_.SerializeAsDescriptorSet();
}

std::vector<PerfettoSqlConnection::StaticTable>
TraceProcessorImpl::GetStaticTables(TraceStorage* storage) {
  std::vector<PerfettoSqlConnection::StaticTable> tables;
  AddStaticTable(tables, storage->mutable_aggregate_profile_table());
  AddStaticTable(tables, storage->mutable_aggregate_sample_table());
  AddStaticTable(tables, storage->mutable_android_aflags_table());
  AddStaticTable(tables, storage->mutable_android_cpu_per_uid_track_table());
  AddStaticTable(tables, storage->mutable_android_dumpstate_table());
  AddStaticTable(tables,
                 storage->mutable_android_game_intervenion_list_table());
  AddStaticTable(tables, storage->mutable_android_log_table());
  AddStaticTable(tables, storage->mutable_build_flags_table());
  AddStaticTable(tables, storage->mutable_modules_table());
  AddStaticTable(tables, storage->mutable_clock_snapshot_table());
  AddStaticTable(tables, storage->mutable_cpu_freq_table());
  AddStaticTable(tables, storage->mutable_cpu_profile_stack_sample_table());
  AddStaticTable(tables, storage->mutable_elf_file_table());
  AddStaticTable(tables, storage->mutable_etm_v4_configuration_table());
  AddStaticTable(tables, storage->mutable_etm_v4_session_table());
  AddStaticTable(tables, storage->mutable_etm_v4_chunk_table());
  AddStaticTable(
      tables, storage->mutable_experimental_missing_chrome_processes_table());
  AddStaticTable(tables, storage->mutable_experimental_proto_content_table());
  AddStaticTable(tables, storage->mutable_file_table());
  AddStaticTable(tables, storage->mutable_filedescriptor_table());
  AddStaticTable(tables, storage->mutable_gpu_context_table());
  AddStaticTable(tables, storage->mutable_gpu_counter_group_table());
  AddStaticTable(tables, storage->mutable_gpu_table());
  AddStaticTable(tables, storage->mutable_instruments_sample_table());
  AddStaticTable(tables, storage->mutable_machine_table());
  AddStaticTable(tables, storage->mutable_memory_snapshot_edge_table());
  AddStaticTable(tables, storage->mutable_memory_snapshot_table());
  AddStaticTable(tables, storage->mutable_mmap_record_table());
  AddStaticTable(tables, storage->mutable_package_list_table());
  AddStaticTable(tables, storage->mutable_user_list_table());
  AddStaticTable(tables, storage->mutable_perf_session_table());
  AddStaticTable(tables, storage->mutable_process_memory_snapshot_table());
  AddStaticTable(tables, storage->mutable_profiler_smaps_table());
  AddStaticTable(tables, storage->mutable_protolog_table());
  AddStaticTable(tables, storage->mutable_winscope_trace_rect_table());
  AddStaticTable(tables, storage->mutable_winscope_rect_table());
  AddStaticTable(tables, storage->mutable_winscope_fill_region_table());
  AddStaticTable(tables, storage->mutable_winscope_transform_table());
  AddStaticTable(tables, storage->mutable_spe_record_table());
  AddStaticTable(tables, storage->mutable_spurious_sched_wakeup_table());
  AddStaticTable(tables,
                 storage->mutable_surfaceflinger_transaction_flag_table());
  AddStaticTable(tables, storage->mutable_trace_file_table());
  AddStaticTable(tables, storage->mutable_trace_import_logs_table());
  AddStaticTable(tables, storage->mutable_v8_isolate_table());
  AddStaticTable(tables, storage->mutable_v8_js_function_table());
  AddStaticTable(tables, storage->mutable_v8_js_script_table());
  AddStaticTable(tables, storage->mutable_v8_wasm_script_table());
  AddStaticTable(
      tables,
      storage->mutable_window_manager_shell_transition_handlers_table());
  AddStaticTable(
      tables,
      storage->mutable_window_manager_shell_transition_participants_table());
  AddStaticTable(tables, storage->mutable_v8_js_code_table());
  AddStaticTable(tables, storage->mutable_v8_internal_code_table());
  AddStaticTable(tables, storage->mutable_v8_wasm_code_table());
  AddStaticTable(tables, storage->mutable_v8_regexp_code_table());
  AddStaticTable(tables, storage->mutable_symbol_table());
  AddStaticTable(tables, storage->mutable_jit_code_table());
  AddStaticTable(tables, storage->mutable_jit_frame_table());
  AddStaticTable(tables, storage->mutable_android_key_events_table());
  AddStaticTable(tables, storage->mutable_android_motion_events_table());
  AddStaticTable(tables, storage->mutable_android_input_event_dispatch_table());
  AddStaticTable(tables, storage->mutable_inputmethod_clients_table());
  AddStaticTable(tables, storage->mutable_inputmethod_manager_service_table());
  AddStaticTable(tables, storage->mutable_inputmethod_service_table());
  AddStaticTable(tables,
                 storage->mutable_surfaceflinger_layers_snapshot_table());
  AddStaticTable(tables, storage->mutable_surfaceflinger_display_table());
  AddStaticTable(tables, storage->mutable_surfaceflinger_layer_table());
  AddStaticTable(tables, storage->mutable_surfaceflinger_transactions_table());
  AddStaticTable(tables, storage->mutable_surfaceflinger_transaction_table());
  AddStaticTable(tables, storage->mutable_viewcapture_table());
  AddStaticTable(tables, storage->mutable_viewcapture_view_table());
  AddStaticTable(tables, storage->mutable_windowmanager_table());
  AddStaticTable(tables,
                 storage->mutable_windowmanager_windowcontainer_table());
  AddStaticTable(
      tables, storage->mutable_window_manager_shell_transition_protos_table());
  AddStaticTable(tables,
                 storage->mutable_window_manager_shell_transitions_table());
  AddStaticTable(tables, storage->mutable_memory_snapshot_node_table());
  AddStaticTable(tables, storage->mutable_experimental_proto_path_table());
  AddStaticTable(tables, storage->mutable_arg_table());
  AddStaticTable(tables, storage->mutable_heap_graph_object_table());
  AddStaticTable(tables, storage->mutable_heap_graph_primitive_table());
  AddStaticTable(tables, storage->mutable_heap_graph_object_data_table());
  AddStaticTable(tables, storage->mutable_heap_graph_reference_table());
  AddStaticTable(tables, storage->mutable_heap_graph_class_table());
  AddStaticTable(tables, storage->mutable_heap_profile_allocation_table());
  AddStaticTable(tables, storage->mutable_perf_sample_table());
  AddStaticTable(tables, storage->mutable_perf_counter_set_table());
  AddStaticTable(tables, storage->mutable_stack_profile_mapping_table());
  AddStaticTable(tables, storage->mutable_vulkan_memory_allocations_table());
  AddStaticTable(tables, storage->mutable_chrome_raw_table());
  AddStaticTable(tables, storage->mutable_ftrace_event_table());
  AddStaticTable(tables, storage->mutable_thread_table());
  AddStaticTable(tables, storage->mutable_process_table());
  AddStaticTable(tables, storage->mutable_cpu_table());
  AddStaticTable(tables, storage->mutable_interrupt_mapping_table());
  AddStaticTable(tables, storage->mutable_sched_slice_table());
  AddStaticTable(tables, storage->mutable_thread_state_table());
  AddStaticTable(tables, storage->mutable_track_table());
  AddStaticTable(tables, storage->mutable_counter_table());
  AddStaticTable(tables, storage->mutable_android_network_packets_table());
  AddStaticTable(tables, storage->mutable_metadata_table());
  AddStaticTable(tables, storage->mutable_stats_table());
  AddStaticTable(tables, storage->mutable_slice_table());
  AddStaticTable(tables, storage->mutable_track_event_callstacks_table());
  AddStaticTable(tables, storage->mutable_flow_table());
  AddStaticTable(tables, storage->mutable_stack_profile_frame_table());
  AddStaticTable(tables, storage->mutable_stack_profile_callsite_table());
  return tables;
}

std::vector<std::unique_ptr<StaticTableFunction>>
TraceProcessorImpl::CreateStaticTableFunctions(
    TraceProcessorContext* context,
    TraceStorage* storage,
    PerfettoSqlConnection* connection) {
  std::vector<std::unique_ptr<StaticTableFunction>> fns;
  fns.emplace_back(std::make_unique<ExperimentalFlamegraph>(context));
  fns.emplace_back(std::make_unique<ExperimentalSliceLayout>(
      storage->mutable_string_pool(), &storage->slice_table()));
  fns.emplace_back(
      std::make_unique<TableInfo>(storage->mutable_string_pool(), connection));
  fns.emplace_back(std::make_unique<Ancestor>(Ancestor::Type::kSlice, storage));
  fns.emplace_back(std::make_unique<Ancestor>(
      Ancestor::Type::kStackProfileCallsite, storage));
  fns.emplace_back(
      std::make_unique<Descendant>(Descendant::Type::kSlice, storage));
  fns.emplace_back(std::make_unique<ConnectedFlow>(
      ConnectedFlow::Mode::kDirectlyConnectedFlow, storage));
  fns.emplace_back(std::make_unique<ConnectedFlow>(
      ConnectedFlow::Mode::kPrecedingFlow, storage));
  fns.emplace_back(std::make_unique<ConnectedFlow>(
      ConnectedFlow::Mode::kFollowingFlow, storage));
  fns.emplace_back(std::make_unique<ExperimentalAnnotatedStack>(context));
  fns.emplace_back(std::make_unique<ExperimentalFlatSlice>(context));
  fns.emplace_back(
      std::make_unique<DfsWeightBounded>(storage->mutable_string_pool()));

#if PERFETTO_BUILDFLAG(PERFETTO_ENABLE_WINSCOPE)
  fns.emplace_back(std::make_unique<WinscopeProtoToArgsWithDefaults>(
      storage->mutable_string_pool(), connection, context));
  fns.emplace_back(std::make_unique<WinscopeSurfaceFlingerHierarchyPaths>(
      storage->mutable_string_pool(), connection));
#endif

  fns.emplace_back(std::make_unique<StdlibDocsModules>(
      storage->mutable_string_pool(), connection));
  fns.emplace_back(std::make_unique<StdlibDocsTables>(
      storage->mutable_string_pool(), connection));
  fns.emplace_back(std::make_unique<StdlibDocsFunctions>(
      storage->mutable_string_pool(), connection));
  fns.emplace_back(std::make_unique<StdlibDocsMacros>(
      storage->mutable_string_pool(), connection));

  return fns;
}

std::unique_ptr<PerfettoSqlConnection>
TraceProcessorImpl::InitPerfettoSqlConnection(
    const InitPerfettoSqlConnectionArgs& args) {
  auto* context = args.context;
  auto* storage = args.storage;
  const auto& config = args.config;
  const auto& packages = args.packages;
  auto& sql_metrics = args.sql_metrics;
  const auto* metrics_descriptor_pool = args.metrics_descriptor_pool;
  auto* proto_fn_name_to_path = args.proto_fn_name_to_path;
  auto* trace_processor = args.trace_processor;
  bool notify_eof_called = args.notify_eof_called;
  auto cached_trace_bounds = args.cached_trace_bounds;
  const auto& plugins = args.plugins;

  auto connection = PerfettoSqlConnection::CreateConnectionToNewDatabase(
      storage->mutable_string_pool(), config.enable_extra_checks);

  PerfettoSqlConnection::Initializer init;
  init.static_tables = GetStaticTables(storage);
  init.static_table_functions =
      CreateStaticTableFunctions(context, storage, connection.get());

  // Plugin contributions.
  std::vector<PluginDataframe> plugin_dataframes;
  for (auto& p : plugins) {
    p->RegisterDataframes(plugin_dataframes);
    p->RegisterStaticTableFunctions(init.static_table_functions);
    p->RegisterSqliteModules(connection.get(), init.sqlite_modules);
    p->RegisterFunctions(connection.get(), init.functions);
    p->RegisterAggregateFunctions(connection.get(), init.aggregate_functions);
    p->RegisterWindowFunctions(connection.get(), init.window_functions);
  }
  for (auto& df : plugin_dataframes) {
    init.static_tables.push_back({df.dataframe, std::move(df.name)});
  }

  // Carve-outs that don't fit cleanly in a plugin:
  // - metrics::RunMetric needs &sql_metrics (a TraceProcessorImpl member).
  // - metrics aggregates / NullIfEmpty / UnwrapMetricProto belong to the
  //   trace_processor library, not the plugin set.
  // - Proto-builder functions are descriptor-pool-driven and registered
  //   dynamically per-connection from the live descriptor pool.
  // - Virtual table modules whose context is the live connection itself
  //   (span_join variants, mipmap operators) can only be wired here.
  init.functions.push_back(
      MakeFunctionRegistration<metrics::NullIfEmpty>(nullptr));
  init.functions.push_back(
      MakeFunctionRegistration<metrics::UnwrapMetricProto>(nullptr));
  init.functions.push_back(MakeFunctionRegistration<metrics::RunMetric>(
      std::make_unique<metrics::RunMetric::UserData>(
          metrics::RunMetric::UserData{connection.get(), &sql_metrics})));
  init.aggregate_functions.push_back(
      MakeAggregateRegistration<metrics::RepeatedField>(nullptr));

  connection->Initialize(std::move(init));

  // Proto-builder registrations are descriptor-pool-driven and don't fit
  // the data-only Initializer shape; register them directly after Initialize.
  {
    auto s = RegisterAllProtoBuilderFunctions(
        metrics_descriptor_pool, proto_fn_name_to_path, connection.get(),
        trace_processor);
    if (!s.ok()) {
      PERFETTO_FATAL("%s", s.c_message());
    }
  }

  // Reregister manually added stdlib packages.
  for (const auto& package : packages) {
    auto new_package = ToRegisteredPackage(package);
    if (!new_package.ok()) {
      PERFETTO_FATAL("%s", new_package.status().c_message());
    }
    auto status =
        connection->RegisterPackage(package.name, std::move(*new_package));
    if (!status.ok()) {
      PERFETTO_FATAL("%s", status.c_message());
    }
  }

  // Import prelude package.
  auto result = connection->Execute(SqlSource::FromTraceProcessorImplementation(
      "INCLUDE PERFETTO MODULE prelude.before_eof.*"));
  if (!result.status().ok()) {
    PERFETTO_FATAL("Failed to import prelude: %s", result.status().c_message());
  }

  if (notify_eof_called) {
    IncludeAfterEofPrelude(connection.get());
  }

  sqlite3* db = connection->sqlite_connection()->db();
  for (const auto& metric : sql_metrics) {
    if (metric.proto_field_name) {
      InsertIntoTraceMetricsTable(db, *metric.proto_field_name);
    }
  }
  BuildBoundsTable(db, cached_trace_bounds);
  return connection;
}

void TraceProcessorImpl::IncludeAfterEofPrelude(
    PerfettoSqlConnection* connection) {
  auto result = connection->Execute(SqlSource::FromTraceProcessorImplementation(
      "INCLUDE PERFETTO MODULE prelude.after_eof.*"));
  if (!result.status().ok()) {
    PERFETTO_FATAL("Failed to import prelude: %s", result.status().c_message());
  }
}

bool TraceProcessorImpl::IsRootMetricField(const std::string& metric_name) {
  std::optional<uint32_t> desc_idx = metrics_descriptor_pool_.FindDescriptorIdx(
      ".perfetto.protos.TraceMetrics");
  if (!desc_idx.has_value())
    return false;
  const auto* field_idx =
      metrics_descriptor_pool_.descriptors()[*desc_idx].FindFieldByName(
          metric_name);
  return field_idx != nullptr;
}

// =================================================================
// |                        Summarizer                              |
// =================================================================

base::Status TraceProcessorImpl::CreateSummarizer(
    std::unique_ptr<Summarizer>* out) {
  // Lazily initialize the descriptor pool for textproto generation.
  auto opt_idx = metrics_descriptor_pool_.FindDescriptorIdx(
      ".perfetto.protos.TraceSummarySpec");
  if (!opt_idx) {
    metrics_descriptor_pool_.AddFromFileDescriptorSet(
        kTraceSummaryDescriptor.data(), kTraceSummaryDescriptor.size());
  }

  // Auto-generate a unique id for table namespacing. The id is embedded in
  // SQL table names (e.g. "_exp_mat_{id}_{seq}") to prevent collisions
  // between multiple summarizer instances.
  std::string id = std::to_string(next_summarizer_id_++);
  *out = std::make_unique<summary::SummarizerImpl>(
      this, &metrics_descriptor_pool_, std::move(id));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
