/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/util/symbolizer/symbolize_database.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/util/build_id.h"
#include "src/trace_processor/util/symbolizer/breakpad_symbolizer.h"
#include "src/trace_processor/util/symbolizer/local_symbolizer.h"
#include "src/trace_processor/util/symbolizer/symbolizer.h"

#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::profiling {

namespace {
using trace_processor::Iterator;

constexpr const char* kQueryUnsymbolized =
    R"(
      select
        spm.name,
        spm.build_id,
        spf.rel_pc,
        spm.load_bias
      from stack_profile_frame spf
      join stack_profile_mapping spm on spf.mapping = spm.id
      where (
          spm.build_id != ''
          -- The [[] is *not* a typo: that's how you escape [ inside a glob.
          or spm.name GLOB '[[]kernel.kallsyms]*'
        )
        and spf.symbol_set_id IS NULL
    )";

// Query to get mappings with empty build IDs and their frame counts.
// These frames cannot be symbolized because we cannot look up symbols without
// a build ID.
constexpr const char* kQueryMappingsWithoutBuildId =
    R"(
      select iif(spm.name = '', '[empty mapping name]', spm.name), count(*)
      from stack_profile_frame spf
      join stack_profile_mapping spm on spf.mapping = spm.id
      where spm.build_id = ''
        and spm.name NOT GLOB '[[]kernel.kallsyms]*'
        and spf.symbol_set_id IS NULL
      group by spm.name
    )";

struct UnsymbolizedMapping {
  std::string name;
  std::string build_id;
  uint64_t load_bias;
  bool operator<(const UnsymbolizedMapping& o) const {
    return std::tie(name, build_id, load_bias) <
           std::tie(o.name, o.build_id, o.load_bias);
  }
};

std::map<UnsymbolizedMapping, std::vector<uint64_t>> GetUnsymbolizedFrames(
    trace_processor::TraceProcessor* tp) {
  std::map<UnsymbolizedMapping, std::vector<uint64_t>> res;
  Iterator it = tp->ExecuteQuery(kQueryUnsymbolized);
  while (it.Next()) {
    int64_t load_bias = it.Get(3).AsLong();
    PERFETTO_CHECK(load_bias >= 0);
    trace_processor::BuildId build_id =
        trace_processor::BuildId::FromHex(it.Get(1).AsString());
    UnsymbolizedMapping unsymbolized_mapping{
        it.Get(0).AsString(), build_id.raw(), static_cast<uint64_t>(load_bias)};
    int64_t rel_pc = it.Get(2).AsLong();
    res[unsymbolized_mapping].emplace_back(rel_pc);
  }
  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Invalid iterator: %s",
                            it.Status().message().c_str());
    return {};
  }
  return res;
}

std::vector<std::pair<std::string, uint32_t>> GetMappingsWithoutBuildId(
    trace_processor::TraceProcessor* tp) {
  std::vector<std::pair<std::string, uint32_t>> result;
  Iterator it = tp->ExecuteQuery(kQueryMappingsWithoutBuildId);
  while (it.Next()) {
    std::string name = it.Get(0).AsString();
    int64_t count = it.Get(1).AsLong();
    PERFETTO_CHECK(count >= 0);
    result.emplace_back(std::move(name), static_cast<uint32_t>(count));
  }
  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to query mappings without build ID: %s",
                            it.Status().message().c_str());
  }
  return result;
}

std::optional<std::string> GetOsRelease(trace_processor::TraceProcessor* tp) {
  Iterator it = tp->ExecuteQuery(
      "select str_value from metadata where name = 'system_release'");
  if (it.Next() && it.ColumnCount() > 0 &&
      it.Get(0).type == trace_processor::SqlValue::kString) {
    return it.Get(0).AsString();
  }
  return std::nullopt;
}

// Creates a local symbolizer for "index" mode.
std::unique_ptr<Symbolizer> CreateIndexSymbolizer(
    const SymbolizerConfig& config) {
  if (config.index_symbol_paths.empty() && config.symbol_files.empty()) {
    return nullptr;
  }
  return MaybeLocalSymbolizer(config.index_symbol_paths, config.symbol_files,
                              "index");
}

// Creates a local symbolizer for "find" mode.
std::unique_ptr<Symbolizer> CreateFindSymbolizer(
    const SymbolizerConfig& config) {
  if (config.find_symbol_paths.empty()) {
    return nullptr;
  }
  return MaybeLocalSymbolizer(config.find_symbol_paths, {}, "find");
}

std::string SymbolizeDatabaseWithSymbolizer(trace_processor::TraceProcessor* tp,
                                            Symbolizer* symbolizer) {
  PERFETTO_CHECK(symbolizer);
  auto unsymbolized = GetUnsymbolizedFrames(tp);
  Symbolizer::Environment env = {GetOsRelease(tp)};

  std::string symbols_proto;
  for (const auto& [unsymbolized_mapping, rel_pcs] : unsymbolized) {
    auto res = symbolizer->Symbolize(env, unsymbolized_mapping.name,
                                     unsymbolized_mapping.build_id,
                                     unsymbolized_mapping.load_bias, rel_pcs);
    if (res.empty()) {
      continue;
    }

    protozero::HeapBuffered<perfetto::protos::pbzero::Trace> trace;
    auto* packet = trace->add_packet();
    auto* module_symbols = packet->set_module_symbols();
    module_symbols->set_path(unsymbolized_mapping.name);
    module_symbols->set_build_id(unsymbolized_mapping.build_id);
    PERFETTO_DCHECK(res.size() == rel_pcs.size());
    for (size_t i = 0; i < res.size(); ++i) {
      auto* address_symbols = module_symbols->add_address_symbols();
      address_symbols->set_address(rel_pcs[i]);
      for (const SymbolizedFrame& frame : res[i]) {
        auto* line = address_symbols->add_lines();
        line->set_function_name(frame.function_name);
        line->set_source_file_name(frame.file_name);
        line->set_line_number(frame.line);
      }
    }
    symbols_proto += trace.SerializeAsString();
  }
  return symbols_proto;
}

}  // namespace

SymbolizerResult SymbolizeDatabase(trace_processor::TraceProcessor* tp,
                                   const SymbolizerConfig& config) {
  SymbolizerResult result;

  // Get mappings and frame count for frames with empty build IDs.
  result.mappings_without_build_id = GetMappingsWithoutBuildId(tp);

  bool has_any_paths =
      !config.index_symbol_paths.empty() || !config.symbol_files.empty() ||
      !config.find_symbol_paths.empty() || !config.breakpad_paths.empty();
  if (!has_any_paths) {
    result.error = SymbolizerError::kSymbolizerNotAvailable;
    result.error_details = "No symbol paths or breakpad paths provided";
    return result;
  }

  // Run "index" mode symbolizer if paths are provided.
  if (auto symbolizer = CreateIndexSymbolizer(config); symbolizer) {
    result.symbols += SymbolizeDatabaseWithSymbolizer(tp, symbolizer.get());
  }

  // Run "find" mode symbolizer if paths are provided.
  if (auto symbolizer = CreateFindSymbolizer(config); symbolizer) {
    result.symbols += SymbolizeDatabaseWithSymbolizer(tp, symbolizer.get());
  }

  // Run breakpad symbolizers for each breakpad path.
  for (const std::string& breakpad_path : config.breakpad_paths) {
    BreakpadSymbolizer symbolizer(breakpad_path);
    result.symbols += SymbolizeDatabaseWithSymbolizer(tp, &symbolizer);
  }

  result.error = SymbolizerError::kOk;
  return result;
}

std::vector<std::string> GetPerfettoBinaryPath() {
  const char* root = getenv("PERFETTO_BINARY_PATH");
  if (root != nullptr) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    const char* delimiter = ";";
#else
    const char* delimiter = ":";
#endif
    return base::SplitString(root, delimiter);
  }
  return {};
}

}  // namespace perfetto::profiling
