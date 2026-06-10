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
#include <cstdint>
#include <optional>
#include <string>

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

base::StatusOr<uint32_t> ParseClockName(const json::Dom& value) {
  if (!value.IsString()) {
    return base::ErrStatus("perfetto_metadata: clock name must be a string");
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
  return base::ErrStatus("perfetto_metadata: unknown clock name: %s",
                         name.c_str());
}

}  // namespace

PerfettoMetadataReader::PerfettoMetadataReader(TraceProcessorContext* context,
                                               uint32_t file_id)
    : context_(context), file_id_(file_id) {}

PerfettoMetadataReader::~PerfettoMetadataReader() = default;

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

  if (!meta.HasMember("version")) {
    return base::ErrStatus(
        "perfetto_metadata: missing required field: version");
  }
  const json::Dom& version = meta["version"];
  if (!version.IsInt() && !version.IsUint()) {
    return base::ErrStatus("perfetto_metadata: version must be an integer");
  }
  if (version.AsInt64() != 1) {
    return base::ErrStatus("perfetto_metadata: unsupported version: %" PRId64,
                           version.AsInt64());
  }

  if (meta.HasMember("trace_time_clock")) {
    ASSIGN_OR_RETURN(uint32_t clock,
                     ParseClockName(meta["trace_time_clock"]));
    state->trace_time_clock = clock;
  }

  if (meta.HasMember("files")) {
    if (!meta["files"].IsArray()) {
      return base::ErrStatus("perfetto_metadata: files must be an array");
    }
    for (const json::Dom& file : meta["files"]) {
      if (!file["path"].IsString()) {
        return base::ErrStatus(
            "perfetto_metadata: files entries must be objects with a string "
            "path");
      }
      state->files.push_back({file["path"].AsString()});
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

}  // namespace perfetto::trace_processor::perfetto_metadata
