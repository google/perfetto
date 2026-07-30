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
#include <vector>

#include "src/trace_processor/importers/common/flow_tracker.h"
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
  auto gpu_row = (*context_->storage->mutable_gpu_table())[id];

  if (!name.empty()) {
    gpu_row.set_name(context_->storage->InternString(name));
  }
  if (!vendor.empty()) {
    gpu_row.set_vendor(context_->storage->InternString(vendor));
  }
  if (!model.empty()) {
    gpu_row.set_model(context_->storage->InternString(model));
  }
  if (!architecture.empty()) {
    gpu_row.set_architecture(context_->storage->InternString(architecture));
  }
  if (!uuid.empty()) {
    gpu_row.set_uuid(context_->storage->InternString(uuid));
  }
  if (!pci_bdf.empty()) {
    gpu_row.set_pci_bdf(context_->storage->InternString(pci_bdf));
  }
  return id;
}

void GpuTracker::AddGpuRenderStageSlice(
    uint64_t event_id,
    SliceId slice_id,
    RenderStageRepresentation representation) {
  auto* gpu_slices = event_id_to_gpu_slices_.Find(event_id);
  if (!gpu_slices) {
    RenderStageSlices slices;
    slices[static_cast<size_t>(representation)] = slice_id;
    event_id_to_gpu_slices_.Insert(event_id, std::move(slices));
  } else {
    (*gpu_slices)[static_cast<size_t>(representation)] = slice_id;
  }

  auto* te_slice = event_id_to_track_event_slice_.Find(event_id);
  if (te_slice) {
    context_->flow_tracker->InsertFlow(*te_slice, slice_id);
  }

  auto* term_slice = event_id_to_terminating_slice_.Find(event_id);
  if (term_slice) {
    context_->flow_tracker->InsertFlow(slice_id, *term_slice);
  }

  auto* waiting = event_id_to_waiting_slices_.Find(event_id);
  if (waiting) {
    for (const WaitingSlice& wait_slice : *waiting) {
      if (wait_slice.representation == representation) {
        context_->flow_tracker->InsertFlow(slice_id, wait_slice.slice_id);
      }
    }
  }
}

void GpuTracker::AddRenderStageSubmission(uint64_t event_id, SliceId slice_id) {
  auto* existing = event_id_to_track_event_slice_.Find(event_id);
  if (existing) {
    *existing = slice_id;
  } else {
    event_id_to_track_event_slice_.Insert(event_id, slice_id);
  }

  auto* gpu_slices = event_id_to_gpu_slices_.Find(event_id);
  if (gpu_slices) {
    for (const std::optional<SliceId>& gpu_slice : *gpu_slices) {
      if (gpu_slice) {
        context_->flow_tracker->InsertFlow(slice_id, *gpu_slice);
      }
    }
  }
}

void GpuTracker::AddRenderStageWait(uint64_t event_id, SliceId slice_id) {
  auto* existing = event_id_to_terminating_slice_.Find(event_id);
  if (existing) {
    *existing = slice_id;
  } else {
    event_id_to_terminating_slice_.Insert(event_id, slice_id);
  }

  auto* gpu_slices = event_id_to_gpu_slices_.Find(event_id);
  if (gpu_slices) {
    for (const std::optional<SliceId>& gpu_slice : *gpu_slices) {
      if (gpu_slice) {
        context_->flow_tracker->InsertFlow(*gpu_slice, slice_id);
      }
    }
  }
}

void GpuTracker::AddPendingRenderStageQueue(std::function<void()> materialize) {
  pending_render_stage_queues_.push_back(std::move(materialize));
}

void GpuTracker::MaterializePendingRenderStageQueues() {
  std::vector<std::function<void()>> pending;
  pending.swap(pending_render_stage_queues_);
  for (auto& materialize : pending) {
    materialize();
  }
}

void GpuTracker::RegisterHardwareQueueTrack(
    uint32_t packet_sequence_id,
    uint32_t gpu_id,
    uint64_t hw_queue_iid,
    const TrackCompressor::TrackFactory& factory) {
  hardware_queue_tracks_.Insert({packet_sequence_id, gpu_id, hw_queue_iid},
                                factory);
}

void GpuTracker::RegisterLogicalQueueTrack(
    UniquePid upid,
    uint64_t logical_queue_id,
    const TrackCompressor::TrackFactory& factory) {
  logical_queue_tracks_.Insert({upid, logical_queue_id}, factory);
}

std::optional<TrackId> GpuTracker::InternHardwareQueueTrack(
    uint32_t packet_sequence_id,
    uint32_t gpu_id,
    uint64_t hw_queue_iid,
    int64_t ts,
    int64_t dur) {
  MaterializePendingRenderStageQueues();
  auto* factory =
      hardware_queue_tracks_.Find({packet_sequence_id, gpu_id, hw_queue_iid});
  if (!factory) {
    return std::nullopt;
  }
  return context_->track_compressor->Scoped(*factory, ts, dur);
}

std::optional<TrackId> GpuTracker::InternLogicalQueueTrack(
    UniquePid upid,
    uint64_t logical_queue_id,
    int64_t ts,
    int64_t dur) {
  MaterializePendingRenderStageQueues();
  auto* factory = logical_queue_tracks_.Find({upid, logical_queue_id});
  if (!factory) {
    return std::nullopt;
  }
  return context_->track_compressor->Scoped(*factory, ts, dur);
}

void GpuTracker::AddEventWait(uint64_t waited_event_id,
                              SliceId slice_id,
                              RenderStageRepresentation representation) {
  auto* gpu_slices = event_id_to_gpu_slices_.Find(waited_event_id);
  if (gpu_slices) {
    const auto& src_slice = (*gpu_slices)[static_cast<size_t>(representation)];
    if (src_slice) {
      context_->flow_tracker->InsertFlow(*src_slice, slice_id);
    }
  }

  WaitingSlice waiting_slice{slice_id, representation};
  auto* waiting = event_id_to_waiting_slices_.Find(waited_event_id);
  if (!waiting) {
    event_id_to_waiting_slices_.Insert(waited_event_id, {waiting_slice});
  } else {
    waiting->push_back(waiting_slice);
  }
}

}  // namespace perfetto::trace_processor
