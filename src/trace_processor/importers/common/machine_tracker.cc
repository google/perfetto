/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

MachineTracker::MachineTracker(TraceProcessorContext* context,
                               uint32_t raw_machine_id)
    : context_(context) {
  auto id =
      context_->storage->mutable_machine_table()->Insert({raw_machine_id}).id;

  if (raw_machine_id)
    machine_id_ = id;
}
MachineTracker::~MachineTracker() = default;

}  // namespace perfetto::trace_processor
