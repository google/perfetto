/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_UTIL_TRACE_ENRICHMENT_SOURCE_FILES_H_
#define SRC_TRACE_PROCESSOR_UTIL_TRACE_ENRICHMENT_SOURCE_FILES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace perfetto::trace_processor {
class TraceProcessor;
}

namespace perfetto::trace_processor::util {

struct SourceFilesConfig {
  // (from, to) prefix pairs: a path starting with `from` is read from the
  // same path under `to`, for sources built in a different location. The
  // first matching pair wins. Bundled files keep the path from the debug
  // info as their key.
  std::vector<std::pair<std::string, std::string>> prefix_maps;

  // Files larger than this are not bundled.
  size_t max_file_bytes = size_t{1} << 20;

  // No further files are bundled once this many bytes of source have been
  // added.
  size_t max_total_bytes = size_t{16} << 20;
};

struct SourceFilesResult {
  // Serialized TracePacket protos containing SourceFile packets.
  // Ready to be appended to the trace or included in a bundle.
  std::string packets;

  uint32_t bundled_count = 0;
  size_t bundled_bytes = 0;

  // Paths which do not exist or could not be read.
  std::vector<std::string> unreadable;

  // Paths which were not bundled because they exceed the size limits.
  std::vector<std::string> skipped;
};

// Returns the distinct source file paths referenced by symbolized frames,
// both those already in the trace (stack_profile_symbol) and those in
// |symbols_proto|, a stream of serialized TracePacket protos carrying
// ModuleSymbols as produced by SymbolizeDatabase. Only absolute paths are
// returned as relative ones cannot be resolved without the build directory.
std::vector<std::string> CollectSourcePaths(TraceProcessor* tp,
                                            const std::string& symbols_proto);

// Reads each of |paths| and serializes its contents as a SourceFile packet.
SourceFilesResult BundleSourceFiles(const std::vector<std::string>& paths,
                                    const SourceFilesConfig& config);

// Formats a human-readable summary of |result|. Returns an empty string if
// there was nothing to bundle.
std::string FormatSourceFilesSummary(const SourceFilesResult& result,
                                     bool verbose);

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_TRACE_ENRICHMENT_SOURCE_FILES_H_
