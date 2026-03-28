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

#include "src/trace_processor/importers/common/gpu_tracker.h"

#include <cstdint>

#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/tables/metadata_tables_py.h"

namespace perfetto::trace_processor {

GpuTracker::GpuTracker(TraceProcessorContext* context) : context_(context) {}

tables::GpuTable::Id GpuTracker::GetOrCreateGpu(uint32_t gpu) {
  auto it = gpu_ids_.Find(gpu);
  if (it) {
    return *it;
  }

  auto machine_id = context_->machine_tracker->machine_id();
  tables::GpuTable::Row row;
  row.gpu = gpu;
  row.machine_id = machine_id;
  auto id = context_->storage->mutable_gpu_table()->Insert(row).id;
  gpu_ids_.Insert(gpu, id);
  return id;
}

}  // namespace perfetto::trace_processor
