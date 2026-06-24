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
#include <utility>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"

namespace perfetto::trace_processor {

// Synthetic manifest machine ids start here. Embedded proto machine_ids fill
// the whole uint32 range, so 1<<32 is the only boundary they can't reach; hence
// raw machine ids are int64 throughout trace_processor.
constexpr int64_t kFirstManifestMachineId = 1ll << 32;

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
  // |ref_file| / |ref_machine| are the is.file/is.machine names; together they
  // pick which machine the reference |clock| lives on (default: this file's own
  // machine). A multi-machine file is several machines, so a reference into one
  // needs both (the file locates the trace, the machine picks the row within
  // it); a single-machine file needs only |ref_file|. |ref_machine| alone is
  // ambiguous - embedded ids are scoped to their trace - and is rejected.
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
    // The file's base machine: a synthetic raw id the reader allocates per
    // distinct |machine_name|, so files sharing a name land on one machine.
    std::optional<int64_t> machine_id;
    std::optional<std::string> machine_name;
    // From a `machines` block: each (embedded proto machine_id, declared name)
    // pair. The reader resolves the names to entries in |machine_remap|
    // (embedded uint32 id -> synthetic raw id), which the proto dispatcher uses
    // to place remote machines.
    std::vector<std::pair<uint32_t, std::string>> machine_mappings;
    base::FlatHashMap<uint32_t, int64_t> machine_remap;
  };

  // True once a perfetto_manifest file has been parsed; a second one is an
  // error.
  bool config_seen = false;

  // The global trace time clock named by the top-level `trace_time` block.
  // |trace_time_clock| is the builtin domain; |trace_time_file| (+
  // |trace_time_machine| for a multi-machine file) name the machine whose clock
  // is trace time, resolved the same way as a clocks is.file/is.machine
  // reference (its row is pre-allocated, so the reader claims it directly).
  std::optional<uint32_t> trace_time_clock;
  std::optional<std::string> trace_time_file;
  std::optional<std::string> trace_time_machine;
  std::vector<FileEntry> files;

  // Maps a machine's logical (raw) id - the id the manifest assigns and names
  // machines by - to its machine-table row id, which is what the clock graph
  // keys on. The manifest reader pre-allocates a row for every declared (or
  // clock-referenced) machine and records it here, so references resolve to
  // real rows at parse time and ForkContextForTrace (via MachineTracker) reuses
  // the same row when the file is later forked.
  base::FlatHashMap<int64_t, uint32_t> raw_id_to_table_id;

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
