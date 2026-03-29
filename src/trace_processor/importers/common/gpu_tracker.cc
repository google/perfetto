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
#include <string_view>

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

tables::GpuTable::Id GpuTracker::SetGpuInfo(uint32_t gpu,
                                            std::string_view name,
                                            std::string_view vendor,
                                            std::string_view model,
                                            std::string_view architecture,
                                            std::string_view uuid,
                                            std::string_view pci_bdf) {
  auto id = GetOrCreateGpu(gpu);
  auto gpu_row = context_->storage->mutable_gpu_table()->FindById(id);
  PERFETTO_CHECK(gpu_row.has_value());

  if (!name.empty()) {
    gpu_row->set_name(context_->storage->InternString(name));
  }
  if (!vendor.empty()) {
    gpu_row->set_vendor(context_->storage->InternString(vendor));
  }
  if (!model.empty()) {
    gpu_row->set_model(context_->storage->InternString(model));
  }
  if (!architecture.empty()) {
    gpu_row->set_architecture(context_->storage->InternString(architecture));
  }
  if (!uuid.empty()) {
    gpu_row->set_uuid(context_->storage->InternString(uuid));
  }
  if (!pci_bdf.empty()) {
    gpu_row->set_pci_bdf(context_->storage->InternString(pci_bdf));
  }
  return id;
}

}  // namespace perfetto::trace_processor
