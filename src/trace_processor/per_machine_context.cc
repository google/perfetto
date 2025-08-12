/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/types/per_machine_context.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

void PerMachineContext::Init(TraceProcessorContext* context,
                             uint32_t raw_machine_id) {
  machine_tracker = std::make_unique<MachineTracker>(context, raw_machine_id);
  cpu_tracker = std::make_unique<CpuTracker>(context);
  mapping_tracker = std::make_unique<MappingTracker>(context);
  process_tracker = std::make_unique<ProcessTracker>(context);
  track_tracker = std::make_unique<TrackTracker>(context);
  sched_event_tracker = std::make_unique<SchedEventTracker>(context);
  track_compressor = std::make_unique<TrackCompressor>(context);
}

std::optional<MachineId> PerMachineContext::machine_id() const {
  return machine_tracker->machine_id();
}

}  // namespace perfetto::trace_processor
