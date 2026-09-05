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

#include "src/trace_processor/util/trace_enrichment/source_files.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"

#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor::util {
namespace {

// Whether a path from debug info can be read from this machine: symbolizers
// report unknown files as "??" and relative paths cannot be resolved without
// knowing the build directory.
bool IsAbsolutePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (path[0] == '/') {
    return true;
  }
  // Windows drive letter, e.g. C:\foo.
  return path.size() > 2 && path[1] == ':' &&
         (path[2] == '\\' || path[2] == '/');
}

void AddPath(std::set<std::string>& paths, std::string path) {
  if (IsAbsolutePath(path)) {
    paths.insert(std::move(path));
  }
}

std::string FormatBytes(size_t bytes) {
  if (bytes >= 1024 * 1024) {
    return std::to_string(bytes / (1024 * 1024)) + " MB";
  }
  return std::to_string(bytes / 1024) + " KB";
}

}  // namespace

std::vector<std::string> CollectSourcePaths(TraceProcessor* tp,
                                            const std::string& symbols_proto) {
  std::set<std::string> paths;

  auto it = tp->ExecuteQuery(R"(
    SELECT DISTINCT source_file
    FROM stack_profile_symbol
    WHERE source_file IS NOT NULL
  )");
  while (it.Next()) {
    AddPath(paths, it.Get(0).AsString());
  }

  // |symbols_proto| may be several serialized Trace messages back to back;
  // decoding them as one message concatenates their packets.
  protos::pbzero::Trace::Decoder trace(symbols_proto);
  for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
    protos::pbzero::TracePacket::Decoder packet(*packet_it);
    if (!packet.has_module_symbols()) {
      continue;
    }
    protos::pbzero::ModuleSymbols::Decoder module(packet.module_symbols());
    for (auto address_it = module.address_symbols(); address_it; ++address_it) {
      protos::pbzero::AddressSymbols::Decoder address(*address_it);
      for (auto line_it = address.lines(); line_it; ++line_it) {
        protos::pbzero::Line::Decoder line(*line_it);
        AddPath(paths, line.source_file_name().ToStdString());
      }
    }
  }
  return {paths.begin(), paths.end()};
}

// The path to read |path| from, after applying the prefix maps.
std::string ResolvePath(const std::string& path,
                        const SourceFilesConfig& config) {
  for (const auto& [from, to] : config.prefix_maps) {
    if (path.compare(0, from.size(), from) == 0) {
      return to + path.substr(from.size());
    }
  }
  return path;
}

SourceFilesResult BundleSourceFiles(const std::vector<std::string>& paths,
                                    const SourceFilesConfig& config) {
  SourceFilesResult result;
  for (const std::string& path : paths) {
    std::string resolved = ResolvePath(path, config);
    std::optional<uint64_t> size = base::GetFileSize(resolved);
    if (!size) {
      result.unreadable.push_back(path);
      continue;
    }
    if (*size > config.max_file_bytes ||
        result.bundled_bytes + *size > config.max_total_bytes) {
      result.skipped.push_back(path);
      continue;
    }
    std::string contents;
    if (!base::ReadFile(resolved, &contents)) {
      result.unreadable.push_back(path);
      continue;
    }

    protozero::HeapBuffered<protos::pbzero::Trace> trace;
    auto* file = trace->add_packet()->set_source_file();
    file->set_path(path);
    file->set_contents(contents);
    result.packets += trace.SerializeAsString();
    result.bundled_count++;
    result.bundled_bytes += contents.size();
  }
  return result;
}

std::string FormatSourceFilesSummary(const SourceFilesResult& result,
                                     bool verbose) {
  if (result.bundled_count == 0 && result.unreadable.empty() &&
      result.skipped.empty()) {
    return "";
  }
  std::string out = "Source files: " + std::to_string(result.bundled_count) +
                    " bundled (" + FormatBytes(result.bundled_bytes) + ")";
  if (!result.unreadable.empty()) {
    out += ", " + std::to_string(result.unreadable.size()) + " not found";
  }
  if (!result.skipped.empty()) {
    out += ", " + std::to_string(result.skipped.size()) + " too large";
  }
  out += "\n";
  if (!verbose) {
    if (!result.unreadable.empty() || !result.skipped.empty()) {
      out += "  Use --verbose to list the files which were not bundled.\n";
    }
    return out;
  }
  for (const std::string& path : result.unreadable) {
    out += "  - not found: " + path + "\n";
  }
  for (const std::string& path : result.skipped) {
    out += "  - too large: " + path + "\n";
  }
  return out;
}

}  // namespace perfetto::trace_processor::util
