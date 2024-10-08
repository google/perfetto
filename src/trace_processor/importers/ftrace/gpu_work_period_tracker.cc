/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/ftrace/gpu_work_period_tracker.h"

#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/common/gpu_counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"

namespace perfetto {
namespace trace_processor {

GpuWorkPeriodTracker::GpuWorkPeriodTracker(TraceProcessorContext* context)
    : context_(context),
      gpu_work_period_id_(context->storage->InternString("GPU Work Period")) {}

void GpuWorkPeriodTracker::ParseGpuWorkPeriodEvent(int64_t timestamp,
                                                   protozero::ConstBytes blob) {
  protos::pbzero::GpuWorkPeriodFtraceEvent::Decoder evt(blob.data, blob.size);

  tables::GpuWorkPeriodTrackTable::Row track(gpu_work_period_id_);
  track.uid = static_cast<int32_t>(evt.uid());
  track.gpu_id = evt.gpu_id();
  track.machine_id = context_->machine_id();
  TrackId track_id =
      context_->track_tracker->LegacyInternGpuWorkPeriodTrack(track);

  const int64_t duration =
      static_cast<int64_t>(evt.end_time_ns() - evt.start_time_ns());
  const int64_t active_duration =
      static_cast<int64_t>(evt.total_active_duration_ns());
  const double active_percent = 100.0 * (static_cast<double>(active_duration) /
                                         static_cast<double>(duration));

  base::StackString<255> entry_name("%%%.2f", active_percent);
  StringId entry_name_id =
      context_->storage->InternString(entry_name.string_view());

  tables::SliceTable::Row row;
  row.ts = timestamp;
  row.dur = duration;
  row.track_id = track_id;
  row.category = kNullStringId;
  row.name = entry_name_id;
  row.thread_ts = timestamp;
  row.thread_dur = active_duration;
  context_->slice_tracker->ScopedTyped(context_->storage->mutable_slice_table(),
                                       row);
}

}  // namespace trace_processor
}  // namespace perfetto
