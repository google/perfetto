/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/util/auto_symbolizer.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "perfetto/trace_processor/trace_processor.h"
#include "src/profiling/symbolizer/local_symbolizer.h"
#include "src/profiling/symbolizer/symbolize_database.h"
#include "src/profiling/symbolizer/symbolizer.h"

namespace perfetto::trace_processor::util {

namespace {

// Returns all mapping names from the trace that have build IDs.
std::vector<std::string> GetAllMappingNames(TraceProcessor* tp) {
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

// Creates a symbolizer based on provided paths and mapping names.
std::unique_ptr<profiling::Symbolizer> CreateSymbolizer(
    const SymbolizerConfig& config,
    const std::vector<std::string>& mapping_names) {
  if (mapping_names.empty()) {
    return nullptr;
  }

  std::unordered_set<std::string> dirs;
  std::unordered_set<std::string> files;

  // Always add paths from PERFETTO_BINARY_PATH environment variable
  std::vector<std::string> env_binary_paths =
      profiling::GetPerfettoBinaryPath();
  if (!env_binary_paths.empty()) {
    dirs.insert(env_binary_paths.begin(), env_binary_paths.end());
  }

  // Add automatic paths unless disabled
  if (!config.no_auto_symbol_paths) {
    std::vector<std::string> auto_paths = GetDefaultSymbolPaths();
    dirs.insert(auto_paths.begin(), auto_paths.end());
  }

  // Add user-provided paths
  if (!config.symbol_paths.empty()) {
    dirs.insert(config.symbol_paths.begin(), config.symbol_paths.end());
  }

  // Add binary paths from mappings (they might contain embedded symbols)
  for (const auto& name : mapping_names) {
    if (!name.empty() && name[0] == '/') {
      files.insert(name);
    }
  }
  return profiling::MaybeLocalSymbolizer(
      std::vector<std::string>(dirs.begin(), dirs.end()),
      std::vector<std::string>(files.begin(), files.end()), "index");
}

}  // namespace

SymbolizerResult Symbolize(TraceProcessor* tp, const SymbolizerConfig& config) {
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
    result.error_details = "Could not create symbolizer (llvm-symbolizer not found?)";
    return result;
  }

  // Run symbolization.
  std::string symbols_proto;
  profiling::SymbolizeDatabase(tp, symbolizer.get(),
                               [&symbols_proto](const std::string& packet) {
                                 symbols_proto += packet;
                               });

  result.error = SymbolizerError::kOk;
  result.symbols = std::move(symbols_proto);
  return result;
}

}  // namespace perfetto::trace_processor::util
