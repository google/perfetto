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

#include "src/trace_processor/importers/ftrace/pixel_display_tracker.h"

#include <cmath>
#include <cstdint>

#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/ftrace/dpu.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

PixelDisplayTracker::PixelDisplayTracker(TraceProcessorContext* context)
    : context_(context),
      frame_start_timeout_name_(
          context->storage->InternString("frame_start_timeout")) {}

void PixelDisplayTracker::ParseDpuDispFrameStartTimeout(
    int64_t timestamp,
    protozero::ConstBytes blob) {
  protos::pbzero::DpuDispFrameStartTimeoutFtraceEvent::Decoder ex(blob);
  static constexpr auto kBluePrint = tracks::SliceBlueprint(
      "disp_frame_start_timeout",
      tracks::DimensionBlueprints(
          tracks::UintDimensionBlueprint("panel_index")),
      tracks::FnNameBlueprint([](uint32_t panel_index) {
        return base::StackString<256>("frame_start_timeout[%u]", panel_index);
      }));

  TrackId track_id = context_->track_tracker->InternTrack(
      kBluePrint, tracks::Dimensions(ex.display_id()));
  StringId slice_name_id = frame_start_timeout_name_;

  context_->slice_tracker->Scoped(
      timestamp, track_id, kNullStringId, slice_name_id, 0,
      [&](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(
            context_->storage->InternString(base::StringView("output_id")),
            Variadic::Integer(ex.output_id()));
        inserter->AddArg(
            context_->storage->InternString(base::StringView("frames_pending")),
            Variadic::Integer(ex.frames_pending()));
        inserter->AddArg(
            context_->storage->InternString(base::StringView("te_count")),
            Variadic::Integer(ex.te_count()));
      });
}

void PixelDisplayTracker::ParseDpuDispFrameDoneTimeout(
    int64_t timestamp,
    protozero::ConstBytes blob) {
  protos::pbzero::DpuDispFrameDoneTimeoutFtraceEvent::Decoder ex(blob);
  static constexpr auto kBluePrint = tracks::SliceBlueprint(
      "disp_frame_done_timeout",
      tracks::DimensionBlueprints(
          tracks::UintDimensionBlueprint("panel_index")),
      tracks::FnNameBlueprint([](uint32_t panel_index) {
        return base::StackString<256>("frame_done_timeout[%u]", panel_index);
      }));

  TrackId track_id = context_->track_tracker->InternTrack(
      kBluePrint, tracks::Dimensions(ex.display_id()));
  StringId slice_name_id = frame_start_timeout_name_;

  context_->slice_tracker->Scoped(
      timestamp, track_id, kNullStringId, slice_name_id, 0,
      [&](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(
            context_->storage->InternString(base::StringView("output_id")),
            Variadic::Integer(ex.output_id()));
        inserter->AddArg(
            context_->storage->InternString(base::StringView("frames_pending")),
            Variadic::Integer(ex.frames_pending()));
        inserter->AddArg(
            context_->storage->InternString(base::StringView("te_count")),
            Variadic::Integer(ex.te_count()));
        inserter->AddArg(
            context_->storage->InternString(base::StringView("during_disable")),
            Variadic::Integer(ex.during_disable()));
      });
}

}  // namespace perfetto::trace_processor