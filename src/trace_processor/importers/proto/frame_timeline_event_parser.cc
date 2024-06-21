/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/frame_timeline_event_parser.h"

#include <cinttypes>

#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"

namespace perfetto {
namespace trace_processor {
namespace {

bool IsBadTimestamp(int64_t ts) {
  // Very small or very large timestamps are likely a mistake.
  // See b/185978397
  constexpr int64_t kBadTimestamp =
      std::numeric_limits<int64_t>::max() - (10LL * 1000 * 1000 * 1000);
  return std::abs(ts) >= kBadTimestamp;
}

}  // namespace

using ExpectedDisplayFrameStartDecoder =
    protos::pbzero::FrameTimelineEvent_ExpectedDisplayFrameStart_Decoder;
using ActualDisplayFrameStartDecoder =
    protos::pbzero::FrameTimelineEvent_ActualDisplayFrameStart_Decoder;

using ExpectedSurfaceFrameStartDecoder =
    protos::pbzero::FrameTimelineEvent_ExpectedSurfaceFrameStart_Decoder;
using ActualSurfaceFrameStartDecoder =
    protos::pbzero::FrameTimelineEvent_ActualSurfaceFrameStart_Decoder;

using FrameEndDecoder = protos::pbzero::FrameTimelineEvent_FrameEnd_Decoder;

static StringId JankTypeBitmaskToStringId(TraceProcessorContext* context,
                                          int32_t jank_type) {
  if (jank_type == FrameTimelineEvent::JANK_UNSPECIFIED)
    return context->storage->InternString("Unspecified");
  if (jank_type == FrameTimelineEvent::JANK_NONE)
    return context->storage->InternString("None");

  std::vector<std::string> jank_reasons;
  if (jank_type & FrameTimelineEvent::JANK_SF_SCHEDULING)
    jank_reasons.emplace_back("SurfaceFlinger Scheduling");
  if (jank_type & FrameTimelineEvent::JANK_PREDICTION_ERROR)
    jank_reasons.emplace_back("Prediction Error");
  if (jank_type & FrameTimelineEvent::JANK_DISPLAY_HAL)
    jank_reasons.emplace_back("Display HAL");
  if (jank_type & FrameTimelineEvent::JANK_SF_CPU_DEADLINE_MISSED)
    jank_reasons.emplace_back("SurfaceFlinger CPU Deadline Missed");
  if (jank_type & FrameTimelineEvent::JANK_SF_GPU_DEADLINE_MISSED)
    jank_reasons.emplace_back("SurfaceFlinger GPU Deadline Missed");
  if (jank_type & FrameTimelineEvent::JANK_APP_DEADLINE_MISSED)
    jank_reasons.emplace_back("App Deadline Missed");
  if (jank_type & FrameTimelineEvent::JANK_BUFFER_STUFFING)
    jank_reasons.emplace_back("Buffer Stuffing");
  if (jank_type & FrameTimelineEvent::JANK_UNKNOWN)
    jank_reasons.emplace_back("Unknown Jank");
  if (jank_type & FrameTimelineEvent::JANK_SF_STUFFING)
    jank_reasons.emplace_back("SurfaceFlinger Stuffing");
  if (jank_type & FrameTimelineEvent::JANK_DROPPED)
    jank_reasons.emplace_back("Dropped Frame");

  std::string jank_str(
      std::accumulate(jank_reasons.begin(), jank_reasons.end(), std::string(),
                      [](const std::string& l, const std::string& r) {
                        return l.empty() ? r : l + ", " + r;
                      }));
  return context->storage->InternString(base::StringView(jank_str));
}

static bool DisplayFrameJanky(int32_t jank_type) {
  if (jank_type == FrameTimelineEvent::JANK_UNSPECIFIED ||
      jank_type == FrameTimelineEvent::JANK_NONE)
    return false;

  int32_t display_frame_jank_bitmask =
      FrameTimelineEvent::JANK_SF_SCHEDULING |
      FrameTimelineEvent::JANK_PREDICTION_ERROR |
      FrameTimelineEvent::JANK_DISPLAY_HAL |
      FrameTimelineEvent::JANK_SF_CPU_DEADLINE_MISSED |
      FrameTimelineEvent::JANK_SF_GPU_DEADLINE_MISSED;
  if (jank_type & display_frame_jank_bitmask)
    return true;
  return false;
}

static bool SurfaceFrameJanky(int32_t jank_type) {
  if (jank_type == FrameTimelineEvent::JANK_UNSPECIFIED ||
      jank_type == FrameTimelineEvent::JANK_NONE)
    return false;

  int32_t surface_frame_jank_bitmask =
      FrameTimelineEvent::JANK_APP_DEADLINE_MISSED |
      FrameTimelineEvent::JANK_UNKNOWN;
  if (jank_type & surface_frame_jank_bitmask)
    return true;
  return false;
}

static bool ValidatePredictionType(TraceProcessorContext* context,
                                   int32_t prediction_type) {
  if (prediction_type >= FrameTimelineEvent::PREDICTION_VALID /*1*/ &&
      prediction_type <= FrameTimelineEvent::PREDICTION_UNKNOWN /*3*/)
    return true;
  context->storage->IncrementStats(stats::frame_timeline_event_parser_errors);
  return false;
}

static bool ValidatePresentType(TraceProcessorContext* context,
                                int32_t present_type) {
  if (present_type >= FrameTimelineEvent::PRESENT_ON_TIME /*1*/ &&
      present_type <= FrameTimelineEvent::PRESENT_UNKNOWN /*5*/)
    return true;
  context->storage->IncrementStats(stats::frame_timeline_event_parser_errors);
  return false;
}

FrameTimelineEventParser::FrameTimelineEventParser(
    TraceProcessorContext* context)
    : context_(context),
      present_type_ids_{
          {context->storage->InternString(
               "Unspecified Present") /* PRESENT_UNSPECIFIED */,
           context->storage->InternString(
               "On-time Present") /* PRESENT_ON_TIME */,
           context->storage->InternString("Late Present") /* PRESENT_LATE */,
           context->storage->InternString("Early Present") /* PRESENT_EARLY */,
           context->storage->InternString(
               "Dropped Frame") /* PRESENT_DROPPED */,
           context->storage->InternString(
               "Unknown Present") /* PRESENT_UNKNOWN */}},
      prediction_type_ids_{
          {context->storage->InternString(
               "Unspecified Prediction") /* PREDICTION_UNSPECIFIED */,
           context->storage->InternString(
               "Valid Prediction") /* PREDICTION_VALID */,
           context->storage->InternString(
               "Expired Prediction") /* PREDICTION_EXPIRED */,
           context->storage->InternString(
               "Unknown Prediction") /* PREDICTION_UNKNOWN */}},
      jank_severity_type_ids_{{context->storage->InternString("Unknown"),
                               context->storage->InternString("None"),
                               context->storage->InternString("Partial"),
                               context->storage->InternString("Full")}},
      expected_timeline_track_name_(
          context->storage->InternString("Expected Timeline")),
      actual_timeline_track_name_(
          context->storage->InternString("Actual Timeline")),
      surface_frame_token_id_(
          context->storage->InternString("Surface frame token")),
      display_frame_token_id_(
          context->storage->InternString("Display frame token")),
      present_type_id_(context->storage->InternString("Present type")),
      on_time_finish_id_(context->storage->InternString("On time finish")),
      gpu_composition_id_(context->storage->InternString("GPU composition")),
      jank_type_id_(context->storage->InternString("Jank type")),
      jank_severity_type_id_(
          context->storage->InternString("Jank severity type")),
      layer_name_id_(context->storage->InternString("Layer name")),
      prediction_type_id_(context->storage->InternString("Prediction type")),
      is_buffer_id_(context->storage->InternString("Is Buffer?")),
      jank_tag_none_id_(context->storage->InternString("No Jank")),
      jank_tag_self_id_(context->storage->InternString("Self Jank")),
      jank_tag_other_id_(context->storage->InternString("Other Jank")),
      jank_tag_dropped_id_(context->storage->InternString("Dropped Frame")),
      jank_tag_buffer_stuffing_id_(
          context->storage->InternString("Buffer Stuffing")),
      jank_tag_sf_stuffing_id_(
          context->storage->InternString("SurfaceFlinger Stuffing")) {}

void FrameTimelineEventParser::ParseExpectedDisplayFrameStart(
    int64_t timestamp,
    ConstBytes bufferBlob) {
  ExpectedDisplayFrameStartDecoder event(bufferBlob.data, bufferBlob.size);
  if (!event.has_cookie()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_token()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_pid()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  int64_t cookie = event.cookie();
  int64_t token = event.token();
  StringId name_id =
      context_->storage->InternString(base::StringView(std::to_string(token)));

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(event.pid()));
  auto expected_track_set_id =
      context_->async_track_set_tracker->InternProcessTrackSet(
          upid, expected_timeline_track_name_);
  cookie_track_set_id_map_[cookie] = expected_track_set_id;

  tables::ExpectedFrameTimelineSliceTable::Row expected_row;
  expected_row.ts = timestamp;
  expected_row.track_id =
      context_->async_track_set_tracker->Begin(expected_track_set_id, cookie);
  expected_row.name = name_id;

  expected_row.display_frame_token = token;
  expected_row.upid = upid;

  context_->slice_tracker->BeginTyped(
      context_->storage->mutable_expected_frame_timeline_slice_table(),
      expected_row, [this, token](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(display_frame_token_id_, Variadic::Integer(token));
      });
}

void FrameTimelineEventParser::ParseActualDisplayFrameStart(
    int64_t timestamp,
    ConstBytes bufferBlob) {
  ActualDisplayFrameStartDecoder event(bufferBlob.data, bufferBlob.size);
  if (!event.has_cookie()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_token()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }
  if (!event.has_pid()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  int64_t cookie = event.cookie();
  int64_t token = event.token();
  StringId name_id =
      context_->storage->InternString(base::StringView(std::to_string(token)));

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(event.pid()));
  auto actual_track_set_id =
      context_->async_track_set_tracker->InternProcessTrackSet(
          upid, actual_timeline_track_name_);
  cookie_track_set_id_map_[cookie] = actual_track_set_id;

  tables::ActualFrameTimelineSliceTable::Row actual_row;
  actual_row.ts = timestamp;
  actual_row.track_id =
      context_->async_track_set_tracker->Begin(actual_track_set_id, cookie);
  actual_row.name = name_id;
  actual_row.display_frame_token = token;
  actual_row.upid = upid;
  actual_row.on_time_finish = event.on_time_finish();
  actual_row.gpu_composition = event.gpu_composition();

  // parse present type
  StringId present_type = present_type_ids_[0];
  if (event.has_present_type() &&
      ValidatePresentType(context_, event.present_type())) {
    present_type = present_type_ids_[static_cast<size_t>(event.present_type())];
  }
  actual_row.present_type = present_type;

  // parse jank type
  StringId jank_type = JankTypeBitmaskToStringId(context_, event.jank_type());
  actual_row.jank_type = jank_type;

  // parse jank severity type
  if (event.has_jank_severity_type()) {
    actual_row.jank_severity_type = jank_severity_type_ids_[static_cast<size_t>(
        event.jank_severity_type())];
  } else {
    // NOTE: Older traces don't have this field. If JANK_NONE use
    // |severity_type| "None", and is not present, use "Unknown".
    actual_row.jank_severity_type =
        (event.jank_type() == FrameTimelineEvent::JANK_NONE)
            ? jank_severity_type_ids_[1]  /* None */
            : jank_severity_type_ids_[0]; /* Unknown */
  }
  StringId jank_severity_type = actual_row.jank_severity_type;

  // parse prediction type
  StringId prediction_type = prediction_type_ids_[0];
  if (event.has_prediction_type() &&
      ValidatePredictionType(context_, event.prediction_type())) {
    prediction_type =
        prediction_type_ids_[static_cast<size_t>(event.prediction_type())];
  }
  actual_row.prediction_type = prediction_type;

  if (DisplayFrameJanky(event.jank_type())) {
    actual_row.jank_tag = jank_tag_self_id_;
  } else if (event.jank_type() == FrameTimelineEvent::JANK_SF_STUFFING) {
    actual_row.jank_tag = jank_tag_sf_stuffing_id_;
  } else if (event.jank_type() == FrameTimelineEvent::JANK_DROPPED) {
    actual_row.jank_tag = jank_tag_dropped_id_;
  } else {
    actual_row.jank_tag = jank_tag_none_id_;
  }

  std::optional<SliceId> opt_slice_id = context_->slice_tracker->BeginTyped(
      context_->storage->mutable_actual_frame_timeline_slice_table(),
      actual_row,
      [this, token, jank_type, jank_severity_type, present_type,
       prediction_type, &event](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(display_frame_token_id_, Variadic::Integer(token));
        inserter->AddArg(present_type_id_, Variadic::String(present_type));
        inserter->AddArg(on_time_finish_id_,
                         Variadic::Integer(event.on_time_finish()));
        inserter->AddArg(gpu_composition_id_,
                         Variadic::Integer(event.gpu_composition()));
        inserter->AddArg(jank_type_id_, Variadic::String(jank_type));
        inserter->AddArg(jank_severity_type_id_,
                         Variadic::String(jank_severity_type));
        inserter->AddArg(prediction_type_id_,
                         Variadic::String(prediction_type));
      });

  // SurfaceFrames will always be parsed before the matching DisplayFrame
  // (since the app works on the frame before SurfaceFlinger does). Because
  // of this it's safe to add all the flow events here and then forget the
  // surface_slice id - we shouldn't see more surfaces_slices that should be
  // connected to this slice after this point.
  auto range = display_token_to_surface_slice_.equal_range(token);
  if (opt_slice_id) {
    for (auto it = range.first; it != range.second; ++it) {
      SliceId surface_slice = it->second;     // App
      SliceId display_slice = *opt_slice_id;  // SurfaceFlinger
      context_->flow_tracker->InsertFlow(surface_slice, display_slice);
    }
  }
  display_token_to_surface_slice_.erase(range.first, range.second);
}

void FrameTimelineEventParser::ParseExpectedSurfaceFrameStart(
    int64_t timestamp,
    ConstBytes bufferBlob) {
  ExpectedSurfaceFrameStartDecoder event(bufferBlob.data, bufferBlob.size);

  if (!event.has_cookie()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_token()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_display_frame_token()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_pid()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  int64_t cookie = event.cookie();
  int64_t token = event.token();
  int64_t display_frame_token = event.display_frame_token();
  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(event.pid()));
  auto token_set_it = expected_timeline_token_map_.find(upid);
  if (token_set_it != expected_timeline_token_map_.end()) {
    auto& token_set = token_set_it->second;
    if (token_set.find(token) != token_set.end()) {
      // If we already have an expected timeline for a token, the expectations
      // are same for all frames that use the token. No need to add duplicate
      // entries.
      return;
    }
  }
  // This is the first time we are seeing this token for this process. Add to
  // the map.
  expected_timeline_token_map_[upid].insert(token);

  StringId layer_name_id = event.has_layer_name()
                               ? context_->storage->InternString(
                                     base::StringView(event.layer_name()))
                               : kNullStringId;
  StringId name_id =
      context_->storage->InternString(base::StringView(std::to_string(token)));

  auto expected_track_set_id =
      context_->async_track_set_tracker->InternProcessTrackSet(
          upid, expected_timeline_track_name_);
  cookie_track_set_id_map_[cookie] = expected_track_set_id;

  tables::ExpectedFrameTimelineSliceTable::Row expected_row;
  expected_row.ts = timestamp;
  expected_row.track_id =
      context_->async_track_set_tracker->Begin(expected_track_set_id, cookie);
  expected_row.name = name_id;

  expected_row.surface_frame_token = token;
  expected_row.display_frame_token = display_frame_token;
  expected_row.upid = upid;
  expected_row.layer_name = layer_name_id;
  context_->slice_tracker->BeginTyped(
      context_->storage->mutable_expected_frame_timeline_slice_table(),
      expected_row,
      [this, token, layer_name_id](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(display_frame_token_id_, Variadic::Integer(token));
        inserter->AddArg(layer_name_id_, Variadic::String(layer_name_id));
      });
}

void FrameTimelineEventParser::ParseActualSurfaceFrameStart(
    int64_t timestamp,
    ConstBytes bufferBlob) {
  ActualSurfaceFrameStartDecoder event(bufferBlob.data, bufferBlob.size);

  if (!event.has_cookie()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_token()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_display_frame_token()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (!event.has_pid()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  int64_t cookie = event.cookie();
  int64_t token = event.token();
  int64_t display_frame_token = event.display_frame_token();

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(event.pid()));
  StringId layer_name_id;
  if (event.has_layer_name())
    layer_name_id =
        context_->storage->InternString(base::StringView(event.layer_name()));
  StringId name_id =
      context_->storage->InternString(base::StringView(std::to_string(token)));

  auto actual_track_set_id =
      context_->async_track_set_tracker->InternProcessTrackSet(
          upid, actual_timeline_track_name_);
  cookie_track_set_id_map_[cookie] = actual_track_set_id;

  tables::ActualFrameTimelineSliceTable::Row actual_row;
  actual_row.ts = timestamp;
  actual_row.track_id =
      context_->async_track_set_tracker->Begin(actual_track_set_id, cookie);
  actual_row.name = name_id;
  actual_row.surface_frame_token = token;
  actual_row.display_frame_token = display_frame_token;
  actual_row.upid = upid;
  actual_row.layer_name = layer_name_id;
  actual_row.on_time_finish = event.on_time_finish();
  actual_row.gpu_composition = event.gpu_composition();

  // parse present type
  StringId present_type = present_type_ids_[0];
  bool present_type_validated = false;
  if (event.has_present_type() &&
      ValidatePresentType(context_, event.present_type())) {
    present_type_validated = true;
    present_type = present_type_ids_[static_cast<size_t>(event.present_type())];
  }
  actual_row.present_type = present_type;

  // parse jank type
  StringId jank_type = JankTypeBitmaskToStringId(context_, event.jank_type());
  actual_row.jank_type = jank_type;

  // parse jank severity type
  if (event.has_jank_severity_type()) {
    actual_row.jank_severity_type = jank_severity_type_ids_[static_cast<size_t>(
        event.jank_severity_type())];
  } else {
    // NOTE: Older traces don't have this field. If JANK_NONE use
    // |severity_type| "None", and is not present, use "Unknown".
    actual_row.jank_severity_type =
        (event.jank_type() == FrameTimelineEvent::JANK_NONE)
            ? jank_severity_type_ids_[1]  /* None */
            : jank_severity_type_ids_[0]; /* Unknown */
  }
  StringId jank_severity_type = actual_row.jank_severity_type;

  // parse prediction type
  StringId prediction_type = prediction_type_ids_[0];
  if (event.has_prediction_type() &&
      ValidatePredictionType(context_, event.prediction_type())) {
    prediction_type =
        prediction_type_ids_[static_cast<size_t>(event.prediction_type())];
  }
  actual_row.prediction_type = prediction_type;

  if (SurfaceFrameJanky(event.jank_type())) {
    actual_row.jank_tag = jank_tag_self_id_;
  } else if (DisplayFrameJanky(event.jank_type())) {
    actual_row.jank_tag = jank_tag_other_id_;
  } else if (event.jank_type() == FrameTimelineEvent::JANK_BUFFER_STUFFING) {
    actual_row.jank_tag = jank_tag_buffer_stuffing_id_;
  } else if (present_type_validated &&
             event.present_type() == FrameTimelineEvent::PRESENT_DROPPED) {
    actual_row.jank_tag = jank_tag_dropped_id_;
  } else {
    actual_row.jank_tag = jank_tag_none_id_;
  }
  StringId is_buffer = context_->storage->InternString("Unspecified");
  if (event.has_is_buffer()) {
    if (event.is_buffer())
      is_buffer = context_->storage->InternString("Yes");
    else
      is_buffer = context_->storage->InternString("No");
  }

  std::optional<SliceId> opt_slice_id = context_->slice_tracker->BeginTyped(
      context_->storage->mutable_actual_frame_timeline_slice_table(),
      actual_row,
      [this, jank_type, jank_severity_type, present_type, token, layer_name_id,
       display_frame_token, prediction_type, is_buffer,
       &event](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(surface_frame_token_id_, Variadic::Integer(token));
        inserter->AddArg(display_frame_token_id_,
                         Variadic::Integer(display_frame_token));
        inserter->AddArg(layer_name_id_, Variadic::String(layer_name_id));
        inserter->AddArg(present_type_id_, Variadic::String(present_type));
        inserter->AddArg(on_time_finish_id_,
                         Variadic::Integer(event.on_time_finish()));
        inserter->AddArg(gpu_composition_id_,
                         Variadic::Integer(event.gpu_composition()));
        inserter->AddArg(jank_type_id_, Variadic::String(jank_type));
        inserter->AddArg(jank_severity_type_id_,
                         Variadic::String(jank_severity_type));
        inserter->AddArg(prediction_type_id_,
                         Variadic::String(prediction_type));
        inserter->AddArg(is_buffer_id_, Variadic::String(is_buffer));
      });

  if (opt_slice_id) {
    display_token_to_surface_slice_.emplace(display_frame_token, *opt_slice_id);
  }
}

void FrameTimelineEventParser::ParseFrameEnd(int64_t timestamp,
                                             ConstBytes bufferBlob) {
  FrameEndDecoder event(bufferBlob.data, bufferBlob.size);

  if (!event.has_cookie()) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  int64_t cookie = event.cookie();
  auto it = cookie_track_set_id_map_.find(cookie);
  if (it == cookie_track_set_id_map_.end()) {
    context_->storage->IncrementStats(stats::frame_timeline_unpaired_end_event);
    return;
  }
  auto track_set_id = it->second;
  auto track_id = context_->async_track_set_tracker->End(track_set_id, cookie);
  context_->slice_tracker->End(timestamp, track_id);
  cookie_track_set_id_map_.erase(it);
}

void FrameTimelineEventParser::ParseFrameTimelineEvent(int64_t timestamp,
                                                       ConstBytes blob) {
  protos::pbzero::FrameTimelineEvent_Decoder frame_event(blob.data, blob.size);

  // Due to platform bugs, negative timestamps can creep into into traces.
  // Ensure that it doesn't make it into the tables.
  // TODO(mayzner): remove the negative check once we have some logic handling
  // this at the sorter level.
  if (timestamp < 0 || IsBadTimestamp(timestamp)) {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
    return;
  }

  if (frame_event.has_expected_display_frame_start()) {
    ParseExpectedDisplayFrameStart(timestamp,
                                   frame_event.expected_display_frame_start());
  } else if (frame_event.has_actual_display_frame_start()) {
    ParseActualDisplayFrameStart(timestamp,
                                 frame_event.actual_display_frame_start());
  } else if (frame_event.has_expected_surface_frame_start()) {
    ParseExpectedSurfaceFrameStart(timestamp,
                                   frame_event.expected_surface_frame_start());
  } else if (frame_event.has_actual_surface_frame_start()) {
    ParseActualSurfaceFrameStart(timestamp,
                                 frame_event.actual_surface_frame_start());
  } else if (frame_event.has_frame_end()) {
    ParseFrameEnd(timestamp, frame_event.frame_end());
  } else {
    context_->storage->IncrementStats(
        stats::frame_timeline_event_parser_errors);
  }
}
}  // namespace trace_processor
}  // namespace perfetto
