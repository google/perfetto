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
// files in the archive. Populated by PerfettoMetadataReader and consulted by
// ForwardingTraceParser when each archive member is initialized.
struct TraceMetadataState {
  // Sentinel for TraceTimeState::trace_time_clock_owner: marks the trace
  // time clock as owned by a perfetto_metadata file (which has no trace file
  // context of its own), preventing any trace file from overriding it.
  static constexpr uint32_t kClockOwnerSentinel = 0xffffffffu;

  struct Anchor {
    // A timestamp from the file, in nanoseconds (the unit every tokenizer
    // normalizes to before clock conversion).
    int64_t file_ts_ns = 0;
    // The clock domain the anchor target is expressed in (builtin clock id).
    uint32_t target_clock = 0;
    // Name of |target_clock| (e.g. "REALTIME"), recorded in the
    // clock_snapshot table.
    std::string target_clock_name;
    // The anchor target value, in nanoseconds.
    int64_t target_ts_ns = 0;
  };

  struct ClocksOverride {
    // Reinterprets which builtin clock the file's timestamps are on.
    std::optional<uint32_t> native;
    // Shifts the file's events relative to where they would land by default.
    // Mutually exclusive with |anchor|.
    std::optional<int64_t> offset_ns;
    // Pins a file timestamp to a value on a named clock domain.
    std::optional<Anchor> anchor;
  };

  struct FileEntry {
    // Exact path of the member within the archive.
    std::string path;
    // Raw machine id the file's events should be attributed to.
    std::optional<uint32_t> machine_id;
    std::optional<ClocksOverride> clocks;
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
