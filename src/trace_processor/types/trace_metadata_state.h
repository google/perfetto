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

// Parsed contents of a perfetto_metadata sidecar file: a JSON file inside an
// archive (zip/tar) which overrides clock and machine handling for the other
// files in the archive. Populated by the perfetto_metadata plugin's reader
// and consulted by ForwardingTraceParser when each archive member is
// initialized.
struct TraceMetadataState {
  // Sentinel for TraceTimeState::trace_time_clock_owner: marks the trace
  // time clock as owned by a perfetto_metadata file (which has no trace file
  // context of its own), preventing any trace file from overriding it.
  static constexpr uint32_t kClockOwnerSentinel = 0xffffffffu;

  struct FileEntry {
    // Exact path of the member within the archive.
    std::string path;
    // Set once a parsed archive member consumed this entry. Entries which
    // remain unmatched when the archive has been fully processed are a
    // configuration error.
    bool matched = false;
  };

  // True once a perfetto_metadata file has been parsed; a second one in the
  // same trace is an error.
  bool config_seen = false;

  // trace_file_table id of the archive (zip/tar) directly containing the
  // perfetto_metadata file; entries only apply to files in that archive.
  std::optional<uint32_t> config_archive_file_id;

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

  // Returns the path of the first entry no archive member matched, or
  // nullptr if all entries were consumed. Checked once the whole input has
  // been parsed.
  const std::string* FirstUnmatchedPath() const {
    for (const FileEntry& entry : files) {
      if (!entry.matched) {
        return &entry.path;
      }
    }
    return nullptr;
  }
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_TYPES_TRACE_METADATA_STATE_H_
