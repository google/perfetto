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

#include "src/traceconv/trace_to_bundle.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/read_trace.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/profiling/symbolizer/local_symbolizer.h"
#include "src/profiling/symbolizer/symbolize_database.h"
#include "src/profiling/symbolizer/symbolizer.h"
#include "src/trace_processor/util/tar_writer.h"
#include "src/traceconv/utils.h"

namespace perfetto::trace_to_text {

namespace {

// Gets all mapping names from the trace for potential symbolization
std::vector<std::string> GetAllMappingNames(
    trace_processor::TraceProcessor* tp) {
  std::vector<std::string> mapping_names;

  // Simple query to get all mapping names with build IDs or known kernel
  // mappings
  auto it = tp->ExecuteQuery(R"(
    SELECT DISTINCT name
    FROM stack_profile_mapping
    WHERE build_id != '' OR name GLOB '[[]kernel.kallsyms]*'
    ORDER BY name
  )");

  while (it.Next()) {
    std::string name = it.Get(0).AsString();
    if (!name.empty()) {
      mapping_names.push_back(name);
    }
  }

  return mapping_names;
}

// Default automatic symbol paths to search (standard Linux paths only)
std::vector<std::string> GetDefaultSymbolPaths() {
  std::vector<std::string> paths;

  // Standard Linux debug symbol paths
  paths.push_back("/usr/lib/debug");

  // User-specific debug path
  const char* home = getenv("HOME");
  if (home) {
    paths.push_back(std::string(home) + "/.debug");
  }

  return paths;
}

// Creates a symbolizer based on provided paths, context, and discovered
// mapping names
std::unique_ptr<profiling::Symbolizer> CreateSymbolizer(
    const BundleContext& context,
    const std::vector<std::string>& mapping_names) {
  std::vector<std::string> search_directories;
  std::vector<std::string> individual_files;

  // Always add paths from PERFETTO_BINARY_PATH environment variable
  std::vector<std::string> env_binary_paths =
      profiling::GetPerfettoBinaryPath();
  if (!env_binary_paths.empty()) {
    search_directories.insert(search_directories.end(),
                              env_binary_paths.begin(), env_binary_paths.end());
  }

  // Add automatic paths unless disabled
  if (!context.no_auto_symbol_paths) {
    std::vector<std::string> auto_paths = GetDefaultSymbolPaths();
    search_directories.insert(search_directories.end(), auto_paths.begin(),
                              auto_paths.end());
  }

  // Add user-provided paths
  if (!context.symbol_paths.empty()) {
    search_directories.insert(search_directories.end(),
                              context.symbol_paths.begin(),
                              context.symbol_paths.end());
  }

  // Add binary paths from mappings (they might contain embedded symbols)
  for (const auto& name : mapping_names) {
    if (!name.empty() && name[0] == '/' &&
        std::find(individual_files.begin(), individual_files.end(), name) ==
            individual_files.end()) {
      individual_files.push_back(name);
    }
  }

  // Use the collected directories and files for the local symbolizer

  // Always use local symbolizer with "index" mode
  std::unique_ptr<profiling::Symbolizer> symbolizer =
      profiling::MaybeLocalSymbolizer(search_directories, individual_files,
                                      "index");

  if (symbolizer) {
    return symbolizer;
  }

  PERFETTO_LOG("Failed to create local symbolizer");
  return nullptr;
}

}  // namespace

int TraceToBundle(const std::string& input_file_path,
                  const std::string& output_file_path,
                  const BundleContext& context) {
  // Create TraceProcessor to analyze the trace
  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  // Read trace using ReadTrace utility
  auto status = trace_processor::ReadTrace(tp.get(), input_file_path.c_str());
  if (!status.ok()) {
    PERFETTO_ELOG("Failed to read trace: %s", status.c_message());
    return 1;
  }

  // Check if this is an Android trace - bundle mode doesn't work for Android
  // yet
  auto android_check = tp->ExecuteQuery(R"(
    SELECT COUNT(*)
    FROM metadata
    WHERE name = 'android_build_fingerprint'
       OR (name = 'system_release' AND value LIKE '%android%')
  )");

  bool is_android = false;
  if (android_check.Next()) {
    int64_t count = android_check.Get(0).AsLong();
    is_android = (count > 0);
  }

  if (is_android) {
    PERFETTO_ELOG(R"(
Bundle mode does not currently support Android traces.
For Android traces, please use the existing 'symbolize' mode instead:

  # Set up symbol paths (choose one):
  export PERFETTO_BINARY_PATH="/path/to/android/symbols"
  export PERFETTO_SYMBOLIZER_MODE=index
  # OR
  export BREAKPAD_SYMBOL_DIR="/path/to/breakpad/symbols"

  # Generate symbols and create bundle:
  traceconv symbolize input.perfetto symbols.pb
  cat input.perfetto symbols.pb > output.perfetto

For more information on setting up Android symbols, see:
https://perfetto.dev/docs/data-sources/native-heap-profiler#symbolization
)");
    return 1;
  }

  // Get all mapping names for potential symbolization
  std::vector<std::string> mapping_names = GetAllMappingNames(tp.get());

  // Prepare bundle data
  std::string symbols_proto;

  // Apply symbolization if mappings are available
  if (!mapping_names.empty()) {
    auto symbolizer = CreateSymbolizer(context, mapping_names);
    if (symbolizer) {
      // Use the same TraceProcessor instance for symbolization
      profiling::SymbolizeDatabase(tp.get(), symbolizer.get(),
                                   [&symbols_proto](const std::string& packet) {
                                     symbols_proto += packet;
                                   });
    }
  }

  // TODO: Implement deobfuscation when needed
  // For now, this is just a stub
  std::string deobfuscation_proto;

  // Create TAR archive
  trace_processor::util::TarWriter tar(output_file_path);

  // Add original trace file directly (memory efficient)
  auto add_trace_status =
      tar.AddFileFromPath("trace.perfetto", input_file_path);
  if (!add_trace_status.ok()) {
    PERFETTO_ELOG("Failed to add trace to TAR archive: %s",
                  add_trace_status.c_message());
    return 1;
  }

  // Add symbol packets if available
  if (!symbols_proto.empty()) {
    auto add_symbols_status = tar.AddFile("symbols.pb", symbols_proto);
    if (!add_symbols_status.ok()) {
      PERFETTO_ELOG("Failed to add symbols to TAR archive: %s",
                    add_symbols_status.c_message());
      return 1;
    }
  }

  // Add deobfuscation packets if available (stub for now)
  if (!deobfuscation_proto.empty()) {
    auto add_deobfusc_status =
        tar.AddFile("deobfuscation.pb", deobfuscation_proto);
    if (!add_deobfusc_status.ok()) {
      PERFETTO_ELOG("Failed to add deobfuscation to TAR archive: %s",
                    add_deobfusc_status.c_message());
      return 1;
    }
  }

  // TAR will be automatically finalized in destructor

  return 0;
}

}  // namespace perfetto::trace_to_text
