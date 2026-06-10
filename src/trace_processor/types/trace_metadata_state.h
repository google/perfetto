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

#ifndef SRC_TRACE_PROCESSOR_TYPES_TRACE_METADATA_STATE_H_
#define SRC_TRACE_PROCESSOR_TYPES_TRACE_METADATA_STATE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace perfetto::trace_processor {

// Parsed contents of a perfetto_metadata sidecar file: a JSON file which,
// as the first file of the trace (typically inside an archive, where sorting
// puts it first), overrides clock and machine handling for the files that
// follow. Populated by the perfetto_metadata plugin's reader and consulted
// by ForwardingTraceParser for each trace file.
struct TraceMetadataState {
  struct FileEntry {
    // Exact path of the member within the archive.
    std::string path;
  };

  // True once a perfetto_metadata file has been parsed; a second one is an
  // error.
  bool config_seen = false;

  std::optional<uint32_t> trace_time_clock;
  std::vector<FileEntry> files;

  FileEntry* FindEntry(const std::string& path) {
    for (FileEntry& entry : files) {
      if (entry.path == path) {
        return &entry;
      }
    }
    return nullptr;
  }
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_TYPES_TRACE_METADATA_STATE_H_
