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

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/storage/metadata.h"
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
  return base::ErrStatus("perfetto_manifest: unknown clock name: %s",
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

base::StatusOr<ClockOverride> ParseAnchor(const json::Dom& anchor) {
  ClockOverride result;
  ASSIGN_OR_RETURN(result.file_ts_ns, ParseAnchorTs(anchor, "ts", "ts"));
  if (!anchor.HasMember("is") || !anchor["is"].IsObject()) {
    return base::ErrStatus(
        "perfetto_manifest: anchor: missing required field: is");
  }

  const json::Dom& is = anchor["is"];
  if (is.HasMember("utc")) {
    if (is.HasMember("clock") || is.HasMember("ts")) {
      return base::ErrStatus(
          "perfetto_manifest: anchor: utc cannot be combined with clock/ts");
    }
    if (!is["utc"].IsString()) {
      return base::ErrStatus("perfetto_manifest: anchor: utc must be a string");
    }
    std::string utc = is["utc"].AsString();
    auto ts = ParseUtcTimestamp(utc);
    if (!ts) {
      return base::ErrStatus(
          "perfetto_manifest: anchor: invalid utc timestamp: %s", utc.c_str());
    }
    result.clock = protos::pbzero::BUILTIN_CLOCK_REALTIME;
    result.clock_ts_ns = *ts;
    return result;
  }
  if (!is.HasMember("clock")) {
    return base::ErrStatus(
        "perfetto_manifest: anchor: missing required field: clock");
  }
  ASSIGN_OR_RETURN(uint32_t clock, ParseClockName(is["clock"]));
  result.clock = clock;
  ASSIGN_OR_RETURN(result.clock_ts_ns, ParseAnchorTs(is, "ts", "is.ts"));
  return result;
}

base::StatusOr<ClockOverride> ParseClocks(const json::Dom& clocks) {
  // Exclusivity diagnostics: each rejected combination would override the
  // file's timeline twice.
  if (clocks.HasMember("offset_ns") && clocks.HasMember("anchor")) {
    return base::ErrStatus(
        "perfetto_manifest: offset_ns and anchor are mutually exclusive");
  }
  if (clocks.HasMember("native") && clocks.HasMember("anchor")) {
    return base::ErrStatus(
        "perfetto_manifest: native and anchor are mutually exclusive");
  }
  if (clocks.HasMember("anchor")) {
    if (!clocks["anchor"].IsObject()) {
      return base::ErrStatus("perfetto_manifest: anchor must be an object");
    }
    return ParseAnchor(clocks["anchor"]);
  }
  ClockOverride result;
  if (clocks.HasMember("native")) {
    ASSIGN_OR_RETURN(uint32_t native, ParseClockName(clocks["native"]));
    result.clock = native;
  }
  if (clocks.HasMember("offset_ns")) {
    if (!clocks["offset_ns"].IsIntegral()) {
      return base::ErrStatus("perfetto_manifest: offset_ns must be an integer");
    }
    int64_t offset_ns = clocks["offset_ns"].AsInt64();
    if (offset_ns == std::numeric_limits<int64_t>::min()) {
      return base::ErrStatus("perfetto_manifest: offset_ns is out of range");
    }
    // Snapshot timestamps must never be negative: a backwards shift is
    // expressed by mapping a later file timestamp to the clock's zero
    // instead.
    if (offset_ns < 0) {
      result.file_ts_ns = -offset_ns;
    } else {
      result.clock_ts_ns = offset_ns;
    }
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
  return entry;
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
    return base::ErrStatus("perfetto_manifest: unsupported version: %" PRId64,
                           version.AsInt64());
  }

  if (meta.HasMember("trace_time_clock")) {
    ASSIGN_OR_RETURN(uint32_t clock, ParseClockName(meta["trace_time_clock"]));
    state->trace_time_clock = clock;
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

  // This file has no per-trace context (and thus no ClockTracker), so it
  // cannot go through ClockTracker::SetGlobalClock. Claim the trace time
  // clock on the shared state directly via the same primitive, using this
  // file's id as the owner: it is unique, so no later trace file's
  // SetGlobalClock can override the choice.
  if (state->trace_time_clock) {
    context_->trace_time_state->TrySetClock(
        ClockId::Machine(*state->trace_time_clock), file_id_);
    context_->global_metadata_tracker->SetMetadata(
        std::nullopt, std::nullopt, metadata::trace_time_clock_id,
        Variadic::Integer(*state->trace_time_clock));
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::perfetto_manifest
