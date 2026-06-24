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

#include "src/trace_processor/plugins/perfetto_manifest/perfetto_manifest_reader.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_manifest_state.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/clock_synchronizer.h"
#include "src/trace_processor/util/json_value.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"

namespace perfetto::trace_processor::perfetto_manifest {
namespace {

using FileEntry = TraceManifestState::FileEntry;
using ClockOverride = TraceManifestState::ClockOverride;

bool IsLeapYear(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

// Parses an ISO-8601 UTC timestamp ("2026-06-10T10:15:30.123Z") into
// nanoseconds since the unix epoch. All fields are range-validated and the
// arithmetic is integer-only: a double-based epoch computation loses up to
// ~256ns near the current epoch (1.7e18 ns exceeds double's 2^53
// exact-integer range). The seconds fraction is parsed by hand to stay
// independent of the LC_NUMERIC locale of embedding applications.
std::optional<int64_t> ParseUtcTimestamp(const std::string& utc) {
  int y, mon, day, h, min, sec;
  int consumed = 0;
  if (sscanf(utc.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mon, &day, &h, &min,
             &sec, &consumed) != 6) {
    return std::nullopt;
  }
  // The upper year bound keeps the result well inside int64 nanoseconds
  // (which overflows in 2262).
  static constexpr int kDaysPerMonth[] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
  if (y < 1970 || y > 2200 || mon < 1 || mon > 12 || h < 0 || h > 23 ||
      min < 0 || min > 59 || sec < 0 || sec > 60) {
    return std::nullopt;
  }
  int max_day = kDaysPerMonth[mon - 1] + (mon == 2 && IsLeapYear(y) ? 1 : 0);
  if (day < 1 || day > max_day) {
    return std::nullopt;
  }
  const char* rest = utc.c_str() + consumed;
  int64_t frac_ns = 0;
  if (*rest == '.') {
    ++rest;
    int significant = 0;
    int total = 0;
    for (; *rest >= '0' && *rest <= '9'; ++rest, ++total) {
      if (significant < 9) {
        frac_ns = frac_ns * 10 + (*rest - '0');
        ++significant;
      }
    }
    if (total == 0) {
      return std::nullopt;
    }
    for (; significant < 9; ++significant) {
      frac_ns *= 10;
    }
  }
  if (rest[0] != 'Z' || rest[1] != '\0') {
    return std::nullopt;
  }
  return base::MkTime(y, mon, day, h, min, sec) * 1000000000 + frac_ns;
}

base::StatusOr<uint32_t> ParseClockName(const json::Dom& value) {
  if (!value.IsString()) {
    return base::ErrStatus("perfetto_manifest: clock name must be a string");
  }
  using protos::pbzero::BuiltinClock;
  std::string name = value.AsString();
  if (name == "REALTIME")
    return BuiltinClock::BUILTIN_CLOCK_REALTIME;
  if (name == "REALTIME_COARSE")
    return BuiltinClock::BUILTIN_CLOCK_REALTIME_COARSE;
  if (name == "MONOTONIC")
    return BuiltinClock::BUILTIN_CLOCK_MONOTONIC;
  if (name == "MONOTONIC_COARSE")
    return BuiltinClock::BUILTIN_CLOCK_MONOTONIC_COARSE;
  if (name == "MONOTONIC_RAW")
    return BuiltinClock::BUILTIN_CLOCK_MONOTONIC_RAW;
  if (name == "BOOTTIME")
    return BuiltinClock::BUILTIN_CLOCK_BOOTTIME;
  return base::ErrStatus(
      "perfetto_manifest: unknown clock name: %s. Use one of REALTIME, "
      "REALTIME_COARSE, MONOTONIC, MONOTONIC_COARSE, MONOTONIC_RAW, BOOTTIME.",
      name.c_str());
}

// Reads a required, non-negative integral nanosecond timestamp stored under
// `key` in `obj`. `label` names the field in error messages (it may be
// qualified, e.g. "is.ts").
base::StatusOr<int64_t> ParseAnchorTs(const json::Dom& obj,
                                      const char* key,
                                      const char* label) {
  if (!obj.HasMember(key) || !obj[key].IsIntegral()) {
    return base::ErrStatus(
        "perfetto_manifest: anchor: missing required field: %s", label);
  }
  int64_t ts = obj[key].AsInt64();
  if (ts < 0) {
    return base::ErrStatus("perfetto_manifest: anchor: %s must not be negative",
                           label);
  }
  return ts;
}

// Parses a file's "clocks" block. The file's clock is related to a reference
// |is| (a builtin clock, or trace time when |is| is omitted) by either an
// |offset_ns| or a pair of coinciding readings (|ts| on this file, |is.ts| on
// the reference). Normalized into ClockOverride: file reading |file_ts_ns|
// corresponds to |clock_ts_ns| on |clock| (nullopt clock = trace time).
//   {is:{clock:"X"}}                          file clock IS X (identity)
//   {offset_ns:N}                             file = trace time + N
//   {ts:A, is:{clock:"X", ts:B}}              file reads A when X reads B
//   {is:{utc:"<ISO>"}}                        anchor onto REALTIME wall time
//   {clock:"X", is:{file:"f", clock:"Y"}}     relate this file's X to file f's
//   Y
// A top-level `clock` names which of this file's clocks to relate (an existing
// clock of an internally-clocked trace); without it the file's own private
// clock is pinned. is.file/is.machine point the reference at another file's or
// machine's clock.
base::StatusOr<ClockOverride> ParseClocks(const json::Dom& clocks) {
  ClockOverride result;

  if (clocks.HasMember("clock")) {
    ASSIGN_OR_RETURN(uint32_t source_clock, ParseClockName(clocks["clock"]));
    result.source_clock = source_clock;
  }

  bool has_is_ts = false;
  if (clocks.HasMember("is")) {
    const json::Dom& is = clocks["is"];
    if (!is.IsObject()) {
      return base::ErrStatus("perfetto_manifest: clocks: is must be an object");
    }
    // is.file and is.machine are complementary, not exclusive: naming a machine
    // inside a multi-machine file needs both (see resolve_ref below).
    if (is.HasMember("file")) {
      if (!is["file"].IsString()) {
        return base::ErrStatus(
            "perfetto_manifest: clocks: is.file must be a string");
      }
      result.ref_file = is["file"].AsString();
    }
    if (is.HasMember("machine")) {
      if (!is["machine"].IsString()) {
        return base::ErrStatus(
            "perfetto_manifest: clocks: is.machine must be a string");
      }
      result.ref_machine = is["machine"].AsString();
    }
    if (is.HasMember("utc")) {
      if (is.HasMember("clock") || is.HasMember("ts")) {
        return base::ErrStatus(
            "perfetto_manifest: clocks: is.utc cannot be combined with "
            "clock/ts");
      }
      if (!is["utc"].IsString()) {
        return base::ErrStatus(
            "perfetto_manifest: clocks: is.utc must be a string");
      }
      std::string utc = is["utc"].AsString();
      auto ts = ParseUtcTimestamp(utc);
      if (!ts) {
        return base::ErrStatus(
            "perfetto_manifest: clocks: invalid is.utc timestamp: %s",
            utc.c_str());
      }
      result.clock = protos::pbzero::BUILTIN_CLOCK_REALTIME;
      result.clock_ts_ns = *ts;
    } else {
      if (!is.HasMember("clock")) {
        return base::ErrStatus(
            "perfetto_manifest: clocks: is needs a clock (or utc)");
      }
      ASSIGN_OR_RETURN(uint32_t clock, ParseClockName(is["clock"]));
      result.clock = clock;
      if (is.HasMember("ts")) {
        ASSIGN_OR_RETURN(result.clock_ts_ns, ParseAnchorTs(is, "ts", "is.ts"));
        has_is_ts = true;
      }
    }
  }

  if (clocks.HasMember("offset_ns")) {
    if (has_is_ts || clocks.HasMember("ts")) {
      return base::ErrStatus(
          "perfetto_manifest: clocks: offset_ns and a reading (ts) are "
          "mutually exclusive");
    }
    if (!clocks["offset_ns"].IsIntegral()) {
      return base::ErrStatus(
          "perfetto_manifest: clocks: offset_ns must be an integer");
    }
    int64_t offset_ns = clocks["offset_ns"].AsInt64();
    if (offset_ns == std::numeric_limits<int64_t>::min()) {
      return base::ErrStatus(
          "perfetto_manifest: clocks: offset_ns is out of range");
    }
    // Snapshot timestamps must never be negative: a backwards shift maps a
    // later file reading onto the reference's zero instead.
    if (offset_ns < 0) {
      result.file_ts_ns = -offset_ns;
    } else {
      result.clock_ts_ns = offset_ns;
    }
  } else if (clocks.HasMember("ts")) {
    ASSIGN_OR_RETURN(result.file_ts_ns, ParseAnchorTs(clocks, "ts", "ts"));
  }
  return result;
}

base::StatusOr<FileEntry> ParseFileEntry(const json::Dom& file) {
  if (!file.IsObject() || !file["path"].IsString()) {
    return base::ErrStatus(
        "perfetto_manifest: files entries must be objects with a string "
        "path");
  }
  FileEntry entry;
  entry.path = file["path"].AsString();
  if (file.HasMember("clocks")) {
    if (!file["clocks"].IsObject()) {
      return base::ErrStatus("perfetto_manifest: clocks must be an object");
    }
    ASSIGN_OR_RETURN(entry.clock_override, ParseClocks(file["clocks"]));
  }
  if (file.HasMember("machine") && file.HasMember("machines")) {
    return base::ErrStatus(
        "perfetto_manifest: machine and machines are mutually exclusive. Use "
        "`machine` for a single-machine file, or `machines` to remap a "
        "multi-machine proto's embedded ids; not both.");
  }
  // `machine` attributes the whole file to one named machine. It is an object
  // (rather than a bare string) so future per-machine attributes can be added
  // without a breaking format change.
  if (file.HasMember("machine")) {
    const json::Dom& machine = file["machine"];
    if (!machine.IsObject() || !machine.HasMember("name") ||
        !machine["name"].IsString()) {
      return base::ErrStatus(
          "perfetto_manifest: machine must be an object with a string name, "
          R"(e.g. "machine": {"name": "phone"}.)");
    }
    std::string name = machine["name"].AsString();
    if (name.empty()) {
      return base::ErrStatus(
          "perfetto_manifest: machine: name must be non-empty");
    }
    entry.machine_name = std::move(name);
  }
  // `machines` remaps a multi-machine proto's embedded machine_ids to named
  // machines.
  if (file.HasMember("machines")) {
    if (!file["machines"].IsArray()) {
      return base::ErrStatus("perfetto_manifest: machines must be an array");
    }
    for (const json::Dom& m : file["machines"]) {
      if (!m.IsObject() || !m.HasMember("id") || !m["id"].IsNumeric() ||
          !m.HasMember("name") || !m["name"].IsString()) {
        return base::ErrStatus(
            "perfetto_manifest: machines entries need an integer id and a "
            R"(string name, e.g. "machines": [{"id": 0, "name": "phone"}].)");
      }
      int64_t id = m["id"].AsInt64();
      if (id < 0 || id > std::numeric_limits<uint32_t>::max()) {
        return base::ErrStatus(
            "perfetto_manifest: machines: id must be in [0, 4294967295]");
      }
      std::string name = m["name"].AsString();
      if (name.empty()) {
        return base::ErrStatus(
            "perfetto_manifest: machines: name must be non-empty");
      }
      entry.machine_mappings.emplace_back(static_cast<uint32_t>(id),
                                          std::move(name));
    }
  }
  return std::move(entry);
}

// Allocates (once) the machine-table row for |raw_machine_id| and records it so
// later forks reuse the same row. Returns the row id.
uint32_t EnsureMachineRow(TraceProcessorContext* context,
                          int64_t raw_machine_id) {
  auto& map = context->trace_manifest_state->raw_id_to_table_id;
  if (uint32_t* row = map.Find(raw_machine_id)) {
    return *row;
  }
  uint32_t row = context->storage->mutable_machine_table()
                     ->Insert({raw_machine_id})
                     .id.value;
  map.Insert(raw_machine_id, row);
  return row;
}

}  // namespace

PerfettoManifestReader::PerfettoManifestReader(TraceProcessorContext* context,
                                               uint32_t file_id)
    : context_(context), file_id_(file_id) {}

PerfettoManifestReader::~PerfettoManifestReader() = default;

base::Status PerfettoManifestReader::Parse(TraceBlobView blob) {
  buffer_.append(reinterpret_cast<const char*>(blob.data()), blob.size());
  return base::OkStatus();
}

base::Status PerfettoManifestReader::OnPushDataToSorter() {
  auto* state = context_->trace_manifest_state.get();
  if (state->config_seen) {
    return base::ErrStatus("multiple perfetto_manifest files in archive");
  }
  state->config_seen = true;

  ASSIGN_OR_RETURN(json::Dom root, json::Parse(buffer_));
  buffer_.clear();
  if (!root.IsObject() || !root.HasMember("perfetto_manifest") ||
      !root["perfetto_manifest"].IsObject()) {
    return base::ErrStatus(
        "perfetto_manifest: expected a JSON object with a top-level "
        "perfetto_manifest key");
  }
  const json::Dom& meta = root["perfetto_manifest"];

  if (!meta.HasMember("version")) {
    return base::ErrStatus(
        "perfetto_manifest: missing required field: version");
  }
  const json::Dom& version = meta["version"];
  if (!version.IsInt() && !version.IsUint()) {
    return base::ErrStatus("perfetto_manifest: version must be an integer");
  }
  if (version.AsInt64() != 1) {
    return base::ErrStatus("perfetto_manifest: unsupported version: %" PRId64
                           ". Only version 1 is supported; set \"version\": 1.",
                           version.AsInt64());
  }

  if (meta.HasMember("trace_time")) {
    const json::Dom& trace_time = meta["trace_time"];
    if (!trace_time.IsObject()) {
      return base::ErrStatus("perfetto_manifest: trace_time must be an object");
    }
    if (!trace_time.HasMember("clock")) {
      return base::ErrStatus("perfetto_manifest: trace_time needs a clock");
    }
    ASSIGN_OR_RETURN(uint32_t clock, ParseClockName(trace_time["clock"]));
    state->trace_time_clock = clock;
    if (trace_time.HasMember("file")) {
      if (!trace_time["file"].IsString()) {
        return base::ErrStatus(
            "perfetto_manifest: trace_time: file must be a string");
      }
      state->trace_time_file = trace_time["file"].AsString();
    }
    if (trace_time.HasMember("machine")) {
      if (!trace_time["machine"].IsString()) {
        return base::ErrStatus(
            "perfetto_manifest: trace_time: machine must be a string");
      }
      state->trace_time_machine = trace_time["machine"].AsString();
    }
  }

  if (meta.HasMember("files")) {
    if (!meta["files"].IsArray()) {
      return base::ErrStatus("perfetto_manifest: files must be an array");
    }
    for (const json::Dom& file : meta["files"]) {
      ASSIGN_OR_RETURN(FileEntry entry, ParseFileEntry(file));
      state->files.push_back(std::move(entry));
    }
  }

  // Allocate one raw machine id per distinct name (the `machine` shorthand and
  // every `machines` entry share the namespace, so the same name is one
  // machine), pre-allocating and naming its row so clock references resolve to
  // it and ForkContextForTrace reuses it. Then set each file's base machine and
  // its embedded-id remap.
  auto& machine_table = *context_->storage->mutable_machine_table();
  base::FlatHashMap<std::string, int64_t> name_to_id;
  int64_t next_id = kFirstManifestMachineId;
  auto raw_id_for_name = [&](const std::string& name) {
    if (int64_t* id = name_to_id.Find(name)) {
      return *id;
    }
    int64_t id = next_id++;
    machine_table[MachineId(EnsureMachineRow(context_, id))].set_name(
        context_->storage->InternString(base::StringView(name)));
    name_to_id.Insert(name, id);
    return id;
  };
  for (FileEntry& entry : state->files) {
    if (entry.machine_name) {
      entry.machine_id = raw_id_for_name(*entry.machine_name);
    }
    for (const auto& [embedded, name] : entry.machine_mappings) {
      int64_t raw = raw_id_for_name(name);
      entry.machine_remap.Insert(embedded, raw);
      if (embedded == 0) {
        entry.machine_id = raw;
        entry.machine_name = name;
      }
    }
  }

  // Resolves a clock-reference target (a clocks is.file/is.machine, or the
  // trace_time file/machine) to a raw machine id. |file_label|/|machine_label|
  // name the two fields in error messages. The rules:
  //   - machine without file: ambiguous, rejected. A machine name alone is not
  //     a stable key because embedded ids are scoped to their trace.
  //   - file + machine: the machine must be one |ref_file| itself declares;
  //     (file, name) is the key.
  //   - file alone: only a single-machine file; a multi-machine file is several
  //     machines, so it must also name which one.
  //   - neither: |fallback| (the referencing file's own machine).
  auto resolve_ref = [&](const char* file_label, const char* machine_label,
                         const std::optional<std::string>& ref_file,
                         const std::optional<std::string>& ref_machine,
                         int64_t fallback) -> base::StatusOr<int64_t> {
    if (ref_machine && !ref_file) {
      return base::ErrStatus(
          "perfetto_manifest: %s '%s' needs %s too: a machine name alone is "
          "ambiguous because embedded machine ids are scoped to their trace.",
          machine_label, ref_machine->c_str(), file_label);
    }
    if (!ref_file) {
      return fallback;
    }
    FileEntry* r = state->FindEntry(*ref_file);
    if (!r) {
      return base::ErrStatus(
          "perfetto_manifest: %s names unknown file '%s'. It must match the "
          "`path` of an entry in the `files` array.",
          file_label, ref_file->c_str());
    }
    if (!ref_machine) {
      if (!r->machine_mappings.empty()) {
        return base::ErrStatus(
            "perfetto_manifest: %s '%s' is a multi-machine trace; also name "
            "the machine with %s.",
            file_label, ref_file->c_str(), machine_label);
      }
      return r->machine_id.value_or(0);
    }
    bool declared = r->machine_name && *r->machine_name == *ref_machine;
    for (const auto& [embedded, name] : r->machine_mappings) {
      if (name == *ref_machine) {
        declared = true;
        break;
      }
    }
    if (!declared) {
      return base::ErrStatus(
          "perfetto_manifest: %s '%s' is not a machine declared by file '%s'.",
          machine_label, ref_machine->c_str(), ref_file->c_str());
    }
    return *name_to_id.Find(*ref_machine);
  };

  // Claim the global trace time clock directly: the manifest is the first file,
  // so its claim wins over later traces. trace_time.file (+ .machine) pins it
  // to that file's (pre-allocated) machine.
  if (state->trace_time_clock) {
    ClockId trace_time = ClockId::Machine(*state->trace_time_clock);
    if (state->trace_time_file || state->trace_time_machine) {
      ASSIGN_OR_RETURN(
          int64_t raw,
          resolve_ref("trace_time: file", "trace_time: machine",
                      state->trace_time_file, state->trace_time_machine,
                      /*fallback=*/0));
      trace_time = ClockId::Machine(EnsureMachineRow(context_, raw),
                                    *state->trace_time_clock);
    }
    context_->trace_time_state->TrySetClock(trace_time, file_id_);
    context_->global_metadata_tracker->SetMetadata(
        std::nullopt, std::nullopt, metadata::trace_time_clock_id,
        Variadic::Integer(*state->trace_time_clock));
  }

  // Add each relate-override's cross-machine edge to the global clock graph
  // now, before any file is parsed (a file may reference another parsed later),
  // recording it in the clock_snapshot table as ClockTracker would. The source
  // is this file's clock; the reference is another machine's clock (is.machine
  // names it, is.file uses that file's machine), or trace time when no clock is
  // named.
  ClockId trace_time = context_->trace_time_state->clock_id;
  for (const FileEntry& entry : state->files) {
    if (!entry.clock_override || !entry.clock_override->source_clock) {
      continue;
    }
    const ClockOverride& co = *entry.clock_override;
    ClockId source = ClockId::Machine(
        EnsureMachineRow(context_, entry.machine_id.value_or(0)),
        *co.source_clock);
    ClockId ref = trace_time;
    if (co.clock) {
      ASSIGN_OR_RETURN(
          int64_t ref_raw,
          resolve_ref("clocks: is.file", "clocks: is.machine", co.ref_file,
                      co.ref_machine, entry.machine_id.value_or(0)));
      ref = ClockId::Machine(EnsureMachineRow(context_, ref_raw), *co.clock);
    }
    std::vector<ClockTimestamp> clocks = {{source, co.file_ts_ns},
                                          {ref, co.clock_ts_ns}};
    ASSIGN_OR_RETURN(uint32_t id, context_->clock_sync->AddSnapshot(clocks));
    ClockTracker::AddSnapshotToTable(context_->storage.get(),
                                     context_->clock_sync.get(), trace_time, id,
                                     clocks);
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::perfetto_manifest
