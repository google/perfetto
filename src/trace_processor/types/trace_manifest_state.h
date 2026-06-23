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

#ifndef SRC_TRACE_PROCESSOR_TYPES_TRACE_MANIFEST_STATE_H_
#define SRC_TRACE_PROCESSOR_TYPES_TRACE_MANIFEST_STATE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace perfetto::trace_processor {

// Parsed contents of a perfetto_manifest sidecar file: a JSON file which,
// as the first file of the trace (typically inside an archive, where sorting
// puts it first), overrides clock and machine handling for the files that
// follow. Populated by the perfetto_manifest plugin's reader and consulted
// by ForwardingTraceParser for each trace file.
struct TraceManifestState {
  // A file's "clocks" override, normalized to a single concept: file time
  // |file_ts_ns| (nanoseconds, the unit every tokenizer normalizes to)
  // corresponds to |clock_ts_ns| on |clock|. "native" parses to a zero/zero
  // mapping onto the named clock, "offset_ns" to a mapping of file time 0 onto
  // the file's per-format clock at the offset (or, for a negative offset, of
  // file time -offset onto the clock's zero), and an "anchor" sets all three
  // fields explicitly.
  //
  // Invariant: both timestamps are non-negative, so the override never
  // introduces negative timestamps into the clock graph. The reader rebases
  // offsets and rejects negative anchors to maintain this.
  struct ClockOverride {
    // Builtin clock id this file's timeline is correlated with; nullopt = the
    // file's per-format clock.
    std::optional<uint32_t> clock;
    int64_t file_ts_ns = 0;
    int64_t clock_ts_ns = 0;
  };

  struct FileEntry {
    // Exact path of the member within the archive.
    std::string path;
    std::optional<ClockOverride> clock_override;
    // Explicit id, or a synthetic id the reader allocates per distinct
    // |machine_name| so files sharing a name land on the same machine.
    std::optional<uint32_t> machine_id;
    std::optional<std::string> machine_name;
  };

  // True once a perfetto_manifest file has been parsed; a second one is an
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

#endif  // SRC_TRACE_PROCESSOR_TYPES_TRACE_MANIFEST_STATE_H_
