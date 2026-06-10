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

#include "src/trace_processor/plugins/perfetto_metadata/perfetto_metadata_reader.h"

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/status.h"
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

namespace perfetto::trace_processor::perfetto_metadata {
namespace {

using FileEntry = TraceMetadataState::FileEntry;

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

base::StatusOr<FileEntry> ParseFileEntry(const json::Dom& file) {
  if (!file.IsObject()) {
    return base::ErrStatus("perfetto_metadata: files entries must be objects");
  }
  RETURN_IF_ERROR(CheckAllowedFields(file, {"path"}, "perfetto_metadata"));
  if (!file.HasMember("path") || !file["path"].IsString()) {
    return base::ErrStatus("perfetto_metadata: missing required field: path");
  }
  FileEntry entry;
  entry.path = file["path"].AsString();
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

}  // namespace perfetto::trace_processor::perfetto_metadata
