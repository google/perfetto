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
#include <unordered_set>
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

std::optional<std::string> GetOsRelease(trace_processor::TraceProcessor* tp) {
  Iterator it = tp->ExecuteQuery(
      "select str_value from metadata where name = 'system_release'");
  if (it.Next() && it.ColumnCount() > 0 &&
      it.Get(0).type == trace_processor::SqlValue::kString) {
    return it.Get(0).AsString();
  }
  return std::nullopt;
}

// Returns all mapping names from the trace that have build IDs.
std::vector<std::string> GetAllMappingNames(
    trace_processor::TraceProcessor* tp) {
  std::vector<std::string> mapping_names;
  auto it = tp->ExecuteQuery(R"(
    SELECT DISTINCT name
    FROM stack_profile_mapping
    WHERE build_id != '' AND name != ''
  )");
  while (it.Next()) {
    mapping_names.push_back(it.Get(0).AsString());
  }
  return mapping_names;
}

// Returns default symbol paths (system debug directories).
std::vector<std::string> GetDefaultSymbolPaths() {
  std::vector<std::string> paths;
  paths.emplace_back("/usr/lib/debug");
  const char* home = getenv("HOME");
  if (home) {
    paths.emplace_back(std::string(home) + "/.debug");
  }
  return paths;
}

// Creates a symbolizer based on provided config and mapping names.
std::unique_ptr<Symbolizer> CreateSymbolizer(
    const SymbolizerConfig& config,
    const std::vector<std::string>& mapping_names) {
  if (mapping_names.empty()) {
    return nullptr;
  }

  std::unordered_set<std::string> dirs;
  std::unordered_set<std::string> files;

  // Always add paths from PERFETTO_BINARY_PATH environment variable.
  std::vector<std::string> env_binary_paths = GetPerfettoBinaryPath();
  if (!env_binary_paths.empty()) {
    dirs.insert(env_binary_paths.begin(), env_binary_paths.end());
  }

  // Add automatic paths unless disabled.
  if (!config.no_auto_symbol_paths) {
    std::vector<std::string> auto_paths = GetDefaultSymbolPaths();
    dirs.insert(auto_paths.begin(), auto_paths.end());
  }

  // Add user-provided paths.
  if (!config.symbol_paths.empty()) {
    dirs.insert(config.symbol_paths.begin(), config.symbol_paths.end());
  }

  // Add binary paths from mappings (they might contain embedded symbols).
  for (const auto& name : mapping_names) {
    if (!name.empty() && name[0] == '/') {
      files.insert(name);
    }
  }
  return MaybeLocalSymbolizer(
      std::vector<std::string>(dirs.begin(), dirs.end()),
      std::vector<std::string>(files.begin(), files.end()), "index");
}

}  // namespace

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

SymbolizerResult SymbolizeDatabase(trace_processor::TraceProcessor* tp,
                                   const SymbolizerConfig& config) {
  SymbolizerResult result;

  // Get all mappings with build IDs from the trace.
  std::vector<std::string> mapping_names = GetAllMappingNames(tp);
  if (mapping_names.empty()) {
    result.error = SymbolizerError::kNoMappingsToSymbolize;
    result.error_details = "No mappings with build IDs found in trace";
    return result;
  }

  // Create the symbolizer.
  auto symbolizer = CreateSymbolizer(config, mapping_names);
  if (!symbolizer) {
    result.error = SymbolizerError::kSymbolizerNotAvailable;
    result.error_details =
        "Could not create symbolizer (llvm-symbolizer not found?)";
    return result;
  }

  // Run symbolization.
  result.symbols = SymbolizeDatabaseWithSymbolizer(tp, symbolizer.get());
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
