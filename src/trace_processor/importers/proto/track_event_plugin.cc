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

#include "src/trace_processor/importers/proto/track_event_plugin.h"

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor {

TrackEventPlugin::TrackEventPlugin(TrackEventPluginContext* context)
    : context_(context) {}

TrackEventPlugin::~TrackEventPlugin() = default;

TrackEventPlugin::Result TrackEventPlugin::OnTrackEventCounterExtension(
    const TrackEventExtensionField&,
    CounterId) {
  return Result::kIgnored;
}

TrackEventPlugin::Result TrackEventPlugin::OnTrackEventSliceExtension(
    const TrackEventExtensionField&,
    SliceId) {
  return Result::kIgnored;
}

TrackEventPlugin::Result TrackEventPlugin::OnTrackEventStateExtension(
    const TrackEventExtensionField&,
    StateId) {
  return Result::kIgnored;
}

void TrackEventPlugin::RegisterTrackEventExtension(uint32_t field_id) {
  PERFETTO_CHECK(!context_->plugins_by_field.Find(field_id));
  context_->plugins_by_field.Insert(field_id, this);
}

}  // namespace perfetto::trace_processor
