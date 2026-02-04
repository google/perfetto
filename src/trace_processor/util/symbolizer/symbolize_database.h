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

#include <string>
#include <vector>

#include "src/trace_processor/util/symbolizer/symbolizer.h"

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
  // Directories to search for symbols. These are added to paths from
  // PERFETTO_BINARY_PATH env var.
  std::vector<std::string> symbol_paths;

  // Specific files to check for symbols (e.g., binary paths from mappings
  // that might contain embedded symbols).
  std::vector<std::string> symbol_files;
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
};

// Performs native symbolization on a trace.
//
// This function:
// 1. Queries the trace for stack_profile_mapping entries with build IDs
// 2. Discovers symbol paths from:
//    - PERFETTO_BINARY_PATH environment variable (always)
//    - User-provided paths in config
//    - Binary paths from mappings (for embedded symbols)
// 3. Creates a symbolizer and runs symbolization
// 4. Returns the result as serialized TracePacket protos
//
// Args:
//   tp: TraceProcessor instance with the trace already loaded
//   config: Configuration for symbolization (paths)
//
// Returns:
//   SymbolizerResult containing error code and symbols (if successful)
SymbolizerResult SymbolizeDatabase(trace_processor::TraceProcessor* tp,
                                   const SymbolizerConfig& config);

// Returns paths from PERFETTO_BINARY_PATH environment variable.
// Used internally but exposed for callers that need to inspect paths.
std::vector<std::string> GetPerfettoBinaryPath();

// Low-level symbolization with a pre-created symbolizer.
// Use this when you need custom symbolizer configuration (e.g., Breakpad).
// Returns serialized TracePacket protos containing symbol data.
std::string SymbolizeDatabaseWithSymbolizer(trace_processor::TraceProcessor* tp,
                                            Symbolizer* symbolizer);

}  // namespace perfetto::profiling

#endif  // SRC_TRACE_PROCESSOR_UTIL_SYMBOLIZER_SYMBOLIZE_DATABASE_H_
