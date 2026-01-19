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

#ifndef SRC_TRACE_PROCESSOR_UTIL_AUTO_SYMBOLIZER_H_
#define SRC_TRACE_PROCESSOR_UTIL_AUTO_SYMBOLIZER_H_

#include <string>
#include <vector>

namespace perfetto::trace_processor {
class TraceProcessor;
}

namespace perfetto::trace_processor::util {

// Error codes for symbolization operations.
// Caller uses these to decide what user-facing message to show.
enum class SymbolizerError {
  kOk,
  kNoMappingsToSymbolize,    // No mappings with build IDs found in trace
  kSymbolizerNotAvailable,   // llvm-symbolizer not found
  kSymbolizationFailed,      // Symbolizer ran but failed
};

// Configuration for symbolization.
struct SymbolizerConfig {
  // Additional paths to search for symbols (beyond automatic discovery).
  std::vector<std::string> symbol_paths;

  // If true, disables automatic symbol path discovery (e.g., /usr/lib/debug).
  bool no_auto_symbol_paths = false;
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
// 2. Creates a symbolizer using the provided paths (and default paths unless
//    disabled)
// 3. Runs symbolization and returns the result as serialized TracePacket protos
//
// The caller is responsible for formatting user-facing error messages based on
// the returned error code.
//
// Args:
//   tp: TraceProcessor instance with the trace already loaded
//   config: Configuration for symbolization (paths, flags)
//
// Returns:
//   SymbolizerResult containing error code and symbols (if successful)
SymbolizerResult Symbolize(TraceProcessor* tp, const SymbolizerConfig& config);

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_AUTO_SYMBOLIZER_H_
