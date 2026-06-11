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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/trace_metadata_state.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/clock_synchronizer.h"
#include "src/trace_processor/util/json_value.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"

namespace perfetto::trace_processor {
namespace {

using FileEntry = TraceMetadataState::FileEntry;
using ClocksOverride = TraceMetadataState::ClocksOverride;
using Anchor = TraceMetadataState::Anchor;

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

// Strict counterpart of json::Dom::AsInt64: rejects (rather than silently
// truncating or UB-casting) values which are fractional or outside int64
// range.
base::StatusOr<int64_t> ParseStrictInt64(const json::Dom& value,
                                         const char* what) {
  if (value.IsInt()) {
    return value.AsInt64();
  }
  if (value.IsUint()) {
    uint64_t v = value.AsUint64();
    if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return base::ErrStatus("perfetto_metadata: %s is out of range", what);
    }
    return static_cast<int64_t>(v);
  }
  if (value.IsDouble()) {
    double d = value.AsDouble();
    // 2^63 (and -2^63) are exactly representable as doubles; values >= 2^63
    // are out of range.
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0) ||
        d != std::floor(d)) {
      return base::ErrStatus(
          "perfetto_metadata: %s must be an integer in int64 range", what);
    }
    return static_cast<int64_t>(d);
  }
  return base::ErrStatus("perfetto_metadata: %s must be a number", what);
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
  RETURN_IF_ERROR(
      CheckAllowedFields(anchor, {"ts", "is"}, "perfetto_metadata: anchor"));
  if (!anchor.HasMember("ts") || !anchor["ts"].IsNumeric()) {
    return base::ErrStatus(
        "perfetto_metadata: anchor: missing required field: ts");
  }
  if (!anchor.HasMember("is") || !anchor["is"].IsObject()) {
    return base::ErrStatus(
        "perfetto_metadata: anchor: missing required field: is");
  }
  Anchor result;
  ASSIGN_OR_RETURN(result.file_ts_ns,
                   ParseStrictInt64(anchor["ts"], "anchor: ts"));

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
    result.target_clock_name = "REALTIME";
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
  result.target_clock_name = is["clock"].AsString();
  ASSIGN_OR_RETURN(result.target_ts_ns,
                   ParseStrictInt64(is["ts"], "anchor: is.ts"));
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
  // An anchor's source clock must be the file's own (per-file) clock: with a
  // "native" override the source would be a machine-shared builtin clock and
  // the anchor edge would collide with real snapshots in the shared clock
  // graph.
  if (clocks.HasMember("native") && clocks.HasMember("anchor")) {
    return base::ErrStatus(
        "perfetto_metadata: native and anchor are mutually exclusive");
  }
  if (clocks.HasMember("offset_ns")) {
    ASSIGN_OR_RETURN(int64_t offset_ns,
                     ParseStrictInt64(clocks["offset_ns"], "offset_ns"));
    result.offset_ns = offset_ns;
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
    return base::ErrStatus("perfetto_metadata: files entries must be objects");
  }
  RETURN_IF_ERROR(CheckAllowedFields(file, {"path", "machine", "clocks"},
                                     "perfetto_metadata"));
  if (!file.HasMember("path") || !file["path"].IsString()) {
    return base::ErrStatus("perfetto_metadata: missing required field: path");
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
    ASSIGN_OR_RETURN(int64_t machine_id,
                     ParseStrictInt64(machine["id"], "machine: id"));
    if (machine_id < 1 || machine_id > std::numeric_limits<uint32_t>::max()) {
      return base::ErrStatus(
          "perfetto_metadata: machine: id must be in [1, 4294967295]");
    }
    entry.machine_id = static_cast<uint32_t>(machine_id);
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
  ASSIGN_OR_RETURN(int64_t version,
                   ParseStrictInt64(meta["version"], "version"));
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
