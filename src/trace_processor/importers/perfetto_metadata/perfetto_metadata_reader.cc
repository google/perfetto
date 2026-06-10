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

#include "src/trace_processor/importers/perfetto_metadata/perfetto_metadata_reader.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/clock_synchronizer.h"
#include "src/trace_processor/types/trace_metadata_state.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/json_value.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"

namespace perfetto::trace_processor {
namespace {

using FileEntry = TraceMetadataState::FileEntry;
using ClocksOverride = TraceMetadataState::ClocksOverride;
using Anchor = TraceMetadataState::Anchor;

// Days from 1970-01-01 to the given civil date (Howard Hinnant's algorithm).
int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Parses an ISO-8601 UTC timestamp ("2026-06-10T10:15:30.123Z") into
// nanoseconds since the unix epoch.
std::optional<int64_t> ParseUtcTimestamp(const std::string& utc) {
  int y, mon, day, h, min;
  double sec;
  char zone;
  if (sscanf(utc.c_str(), "%d-%d-%dT%d:%d:%lf%c", &y, &mon, &day, &h, &min,
             &sec, &zone) != 7 ||
      zone != 'Z') {
    return std::nullopt;
  }
  int64_t days = DaysFromCivil(y, mon, day);
  double epoch_s = static_cast<double>(days * 86400 + h * 3600 + min * 60);
  return static_cast<int64_t>((epoch_s + sec) * 1e9);
}

base::Status CheckAllowedFields(const json::Dom& obj,
                                std::initializer_list<const char*> allowed,
                                const char* prefix) {
  for (const std::string& key : obj.GetMemberNames()) {
    bool ok = false;
    for (const char* a : allowed) {
      if (key == a) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      return base::ErrStatus("%s: unknown field: %s", prefix, key.c_str());
    }
  }
  return base::OkStatus();
}

base::StatusOr<uint32_t> ParseClockNameOrError(const json::Dom& value) {
  if (!value.IsString()) {
    return base::ErrStatus("perfetto_metadata: clock name must be a string");
  }
  std::string name = value.AsString();
  auto clock = PerfettoMetadataReader::ParseClockName(name);
  if (!clock) {
    return base::ErrStatus("perfetto_metadata: unknown clock name: %s",
                           name.c_str());
  }
  return *clock;
}

base::StatusOr<Anchor> ParseAnchor(const json::Dom& anchor) {
  RETURN_IF_ERROR(CheckAllowedFields(anchor, {"ts", "is"},
                                     "perfetto_metadata: anchor"));
  if (!anchor.HasMember("ts") || !anchor["ts"].IsNumeric()) {
    return base::ErrStatus(
        "perfetto_metadata: anchor: missing required field: ts");
  }
  if (!anchor.HasMember("is") || !anchor["is"].IsObject()) {
    return base::ErrStatus(
        "perfetto_metadata: anchor: missing required field: is");
  }
  Anchor result;
  result.file_ts = anchor["ts"].AsDouble();

  const json::Dom& is = anchor["is"];
  RETURN_IF_ERROR(CheckAllowedFields(is, {"clock", "ts", "utc"},
                                     "perfetto_metadata: anchor"));
  if (is.HasMember("utc")) {
    if (is.HasMember("clock") || is.HasMember("ts")) {
      return base::ErrStatus(
          "perfetto_metadata: anchor: utc cannot be combined with clock/ts");
    }
    if (!is["utc"].IsString()) {
      return base::ErrStatus("perfetto_metadata: anchor: utc must be a string");
    }
    std::string utc = is["utc"].AsString();
    auto ts = ParseUtcTimestamp(utc);
    if (!ts) {
      return base::ErrStatus(
          "perfetto_metadata: anchor: invalid utc timestamp: %s", utc.c_str());
    }
    result.target_clock = protos::pbzero::BUILTIN_CLOCK_REALTIME;
    result.target_ts_ns = *ts;
    return result;
  }
  if (!is.HasMember("clock")) {
    return base::ErrStatus(
        "perfetto_metadata: anchor: missing required field: clock");
  }
  if (!is.HasMember("ts") || !is["ts"].IsNumeric()) {
    return base::ErrStatus(
        "perfetto_metadata: anchor: missing required field: ts");
  }
  ASSIGN_OR_RETURN(result.target_clock, ParseClockNameOrError(is["clock"]));
  result.target_ts_ns = is["ts"].AsInt64();
  return result;
}

base::StatusOr<ClocksOverride> ParseClocks(const json::Dom& clocks) {
  RETURN_IF_ERROR(CheckAllowedFields(clocks, {"native", "offset_ns", "anchor"},
                                     "perfetto_metadata"));
  ClocksOverride result;
  if (clocks.HasMember("native")) {
    ASSIGN_OR_RETURN(uint32_t native, ParseClockNameOrError(clocks["native"]));
    result.native = native;
  }
  if (clocks.HasMember("offset_ns") && clocks.HasMember("anchor")) {
    return base::ErrStatus(
        "perfetto_metadata: offset_ns and anchor are mutually exclusive");
  }
  if (clocks.HasMember("offset_ns")) {
    if (!clocks["offset_ns"].IsNumeric()) {
      return base::ErrStatus("perfetto_metadata: offset_ns must be a number");
    }
    result.offset_ns = clocks["offset_ns"].AsInt64();
  }
  if (clocks.HasMember("anchor")) {
    if (!clocks["anchor"].IsObject()) {
      return base::ErrStatus("perfetto_metadata: anchor must be an object");
    }
    ASSIGN_OR_RETURN(result.anchor, ParseAnchor(clocks["anchor"]));
  }
  return result;
}

base::StatusOr<FileEntry> ParseFileEntry(const json::Dom& file) {
  if (!file.IsObject()) {
    return base::ErrStatus(
        "perfetto_metadata: files entries must be objects");
  }
  RETURN_IF_ERROR(CheckAllowedFields(file, {"path", "machine", "clocks"},
                                     "perfetto_metadata"));
  if (!file.HasMember("path") || !file["path"].IsString()) {
    return base::ErrStatus(
        "perfetto_metadata: missing required field: path");
  }
  FileEntry entry;
  entry.path = file["path"].AsString();
  if (file.HasMember("machine")) {
    const json::Dom& machine = file["machine"];
    if (!machine.IsObject()) {
      return base::ErrStatus("perfetto_metadata: machine must be an object");
    }
    RETURN_IF_ERROR(CheckAllowedFields(machine, {"id"}, "perfetto_metadata"));
    if (!machine.HasMember("id") || !machine["id"].IsNumeric()) {
      return base::ErrStatus(
          "perfetto_metadata: machine: missing required field: id");
    }
    entry.machine_id = static_cast<uint32_t>(machine["id"].AsUint64());
  }
  if (file.HasMember("clocks")) {
    if (!file["clocks"].IsObject()) {
      return base::ErrStatus("perfetto_metadata: clocks must be an object");
    }
    ASSIGN_OR_RETURN(entry.clocks, ParseClocks(file["clocks"]));
  }
  return entry;
}

}  // namespace

PerfettoMetadataReader::PerfettoMetadataReader(TraceProcessorContext* context)
    : context_(context) {}

PerfettoMetadataReader::~PerfettoMetadataReader() = default;

// static
std::optional<uint32_t> PerfettoMetadataReader::ParseClockName(
    const std::string& name) {
  using protos::pbzero::BuiltinClock;
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
  return std::nullopt;
}

base::Status PerfettoMetadataReader::Parse(TraceBlobView blob) {
  buffer_.append(reinterpret_cast<const char*>(blob.data()), blob.size());
  return base::OkStatus();
}

base::Status PerfettoMetadataReader::OnPushDataToSorter() {
  auto* state = context_->trace_metadata_state.get();
  if (state->config_seen) {
    return base::ErrStatus("multiple perfetto_metadata files in archive");
  }
  state->config_seen = true;

  ASSIGN_OR_RETURN(json::Dom root, json::Parse(buffer_));
  buffer_.clear();
  if (!root.IsObject() || !root.HasMember("perfetto_metadata") ||
      !root["perfetto_metadata"].IsObject()) {
    return base::ErrStatus(
        "perfetto_metadata: expected a JSON object with a top-level "
        "perfetto_metadata key");
  }
  const json::Dom& meta = root["perfetto_metadata"];

  RETURN_IF_ERROR(CheckAllowedFields(
      meta, {"version", "trace_time_clock", "files"}, "perfetto_metadata"));

  if (!meta.HasMember("version") || !meta["version"].IsNumeric()) {
    return base::ErrStatus(
        "perfetto_metadata: missing required field: version");
  }
  int64_t version = meta["version"].AsInt64();
  if (version != 1) {
    return base::ErrStatus("perfetto_metadata: unsupported version: %" PRId64,
                           version);
  }

  if (meta.HasMember("trace_time_clock")) {
    ASSIGN_OR_RETURN(uint32_t clock,
                     ParseClockNameOrError(meta["trace_time_clock"]));
    state->trace_time_clock = clock;
  }

  if (meta.HasMember("files")) {
    if (!meta["files"].IsArray()) {
      return base::ErrStatus("perfetto_metadata: files must be an array");
    }
    for (const json::Dom& file : meta["files"]) {
      ASSIGN_OR_RETURN(FileEntry entry, ParseFileEntry(file));
      if (state->FindEntry(entry.path)) {
        return base::ErrStatus(
            "perfetto_metadata: duplicate entry for path: %s",
            entry.path.c_str());
      }
      state->files.push_back(std::move(entry));
    }
  }

  // Claim the global trace time clock now, before any other archive member
  // is parsed. This file has no per-trace context (and thus no ClockTracker)
  // so write the shared TraceTimeState directly; the sentinel owner id
  // ensures no later trace file can override the choice, as SetGlobalClock
  // only allows changes by the owning trace file.
  if (state->trace_time_clock) {
    auto* trace_time = context_->trace_time_state.get();
    trace_time->clock_id = ClockId::Machine(*state->trace_time_clock);
    trace_time->trace_time_clock_owner =
        TraceMetadataState::kClockOwnerSentinel;
    context_->global_metadata_tracker->SetMetadata(
        std::nullopt, std::nullopt, metadata::trace_time_clock_id,
        Variadic::Integer(*state->trace_time_clock));
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
