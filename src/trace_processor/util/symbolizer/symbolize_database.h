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

#ifndef SRC_TRACE_PROCESSOR_UTIL_SYMBOLIZER_SYMBOLIZE_DATABASE_H_
#define SRC_TRACE_PROCESSOR_UTIL_SYMBOLIZER_SYMBOLIZE_DATABASE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace perfetto::trace_processor {
class TraceProcessor;
}

namespace perfetto::profiling {

// Error codes for symbolization operations.
// Caller uses these to decide what user-facing message to show.
enum class SymbolizerError {
  kOk,
  kSymbolizerNotAvailable,  // llvm-symbolizer not found
  kSymbolizationFailed,     // Symbolizer ran but failed
};

// Configuration for symbolization.
struct SymbolizerConfig {
  // Directories to search using "index" mode (builds an index by build ID).
  // Faster for repeated lookups.
  std::vector<std::string> index_symbol_paths;

  // Directories to search using "find" mode (searches each time).
  // Slower but uses less memory.
  std::vector<std::string> find_symbol_paths;

  // Specific files to check for symbols (e.g., binary paths from mappings
  // that might contain embedded symbols). Used with "index" mode.
  std::vector<std::string> symbol_files;

  // Directories containing breakpad symbol files (.breakpad format).
  // Each directory will be searched for symbol files matching build IDs.
  std::vector<std::string> breakpad_paths;
};

// Result of symbolization operation.
struct SymbolizerResult {
  SymbolizerError error = SymbolizerError::kOk;

  // Machine-readable details about the error (e.g., missing path).
  // Empty on success.
  std::string error_details;

  // Serialized TracePacket protos containing symbol data.
  // Empty if no symbols were found or on error.
  std::string symbols;

  // Number of frames that could not be symbolized because their mapping
  // has an empty build ID.
  uint32_t frames_without_build_id = 0;

  // Names of mappings with empty build IDs that could not be symbolized.
  std::vector<std::string> mappings_without_build_id;
};

// Performs native symbolization on a trace.
//
// This function:
// 1. Queries the trace for stack_profile_mapping entries with build IDs
// 2. Creates local and/or breakpad symbolizers based on config
// 3. Runs symbolization and returns the result as serialized TracePacket protos
SymbolizerResult SymbolizeDatabase(trace_processor::TraceProcessor* tp,
                                   const SymbolizerConfig& config);

// Returns paths from PERFETTO_BINARY_PATH environment variable.
std::vector<std::string> GetPerfettoBinaryPath();

}  // namespace perfetto::profiling

#endif  // SRC_TRACE_PROCESSOR_UTIL_SYMBOLIZER_SYMBOLIZE_DATABASE_H_
