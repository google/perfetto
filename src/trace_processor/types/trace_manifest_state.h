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
#include <unordered_map>
#include <vector>

namespace perfetto::trace_processor {

// Parsed contents of a perfetto_manifest sidecar file: a JSON file which,
// as the first file of the trace (typically inside an archive, where sorting
// puts it first), overrides clock and machine handling for the files that
// follow. Populated by the perfetto_manifest plugin's reader and consulted
// by ForwardingTraceParser for each trace file.
struct TraceManifestState {
  // A file's "clocks" block, normalized: the source clock reading |file_ts_ns|
  // corresponds to |clock_ts_ns| on the reference (|clock|; nullopt = trace
  // time). offset_ns and the ts/is.ts anchor both set these. The source is:
  //   - |source_clock| unset: the file's own private timeline. The file is
  //     PINNED onto it (single-clock; the file's own ClockSnapshots are then
  //     rejected). Used for clockless files (JSON, ...).
  //   - |source_clock| set: an existing clock of an internally-clocked file
  //     (e.g. a proto's BOOTTIME). This only RELATES that clock to the
  //     reference via a cross-machine edge; it does not pin or reject
  //     snapshots.
  // |ref_file| / |ref_machine| are the is.file/is.machine names; they pick
  // which machine the reference |clock| lives on (default: this file's own
  // machine).
  //
  // Invariant: both timestamps are non-negative, so the override never
  // introduces negative timestamps into the clock graph.
  struct ClockOverride {
    std::optional<uint32_t> source_clock;
    std::optional<uint32_t> clock;
    std::optional<std::string> ref_file;
    std::optional<std::string> ref_machine;
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

  // The global trace time clock named by the top-level `trace_time` block.
  // |trace_time_clock| is the builtin domain; |trace_time_file|, when set,
  // names the file whose machine the clock belongs to (its row is
  // pre-allocated, so the reader claims the qualified clock directly).
  std::optional<uint32_t> trace_time_clock;
  std::optional<std::string> trace_time_file;
  std::vector<FileEntry> files;

  // Maps a machine's logical (raw) id - the id the manifest assigns and names
  // machines by - to its machine-table row id, which is what the clock graph
  // keys on. The manifest reader pre-allocates a row for every declared (or
  // clock-referenced) machine and records it here, so references resolve to
  // real rows at parse time and ForkContextForTrace (via MachineTracker) reuses
  // the same row when the file is later forked.
  std::unordered_map<uint32_t, uint32_t> raw_id_to_table_id;

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
