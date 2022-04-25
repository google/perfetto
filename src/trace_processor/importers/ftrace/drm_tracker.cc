/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/importers/ftrace/drm_tracker.h"
#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/trace/ftrace/dma_fence.pbzero.h"
#include "protos/perfetto/trace/ftrace/drm.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/gpu_scheduler.pbzero.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"

namespace perfetto {
namespace trace_processor {

namespace {

// There are meta-fences such as fence arrays or fence chains where a fence is
// a container of other fences.  These fences are on "unbound" timelines which
// are often dynamically created.  We want to ignore these timelines to avoid
// having tons of tracks for them.
constexpr char kUnboundFenceTimeline[] = "unbound";

}  // namespace

DrmTracker::DrmTracker(TraceProcessorContext* context)
    : context_(context),
      vblank_slice_signal_id_(context->storage->InternString("signal")),
      vblank_slice_deliver_id_(context->storage->InternString("deliver")),
      vblank_arg_seqno_id_(context->storage->InternString("vblank seqno")),
      sched_slice_schedule_id_(context->storage->InternString("drm_sched_job")),
      sched_slice_job_id_(context->storage->InternString("job")),
      sched_arg_ring_id_(context->storage->InternString("gpu sched ring")),
      sched_arg_job_id_(context->storage->InternString("gpu sched job")),
      fence_slice_fence_id_(context->storage->InternString("fence")),
      fence_slice_wait_id_(context->storage->InternString("dma_fence_wait")),
      fence_arg_context_id_(context->storage->InternString("fence context")),
      fence_arg_seqno_id_(context->storage->InternString("fence seqno")) {}

void DrmTracker::ParseDrm(int64_t timestamp,
                          int32_t field_id,
                          uint32_t pid,
                          protozero::ConstBytes blob) {
  using protos::pbzero::FtraceEvent;

  switch (field_id) {
    case FtraceEvent::kDrmVblankEventFieldNumber: {
      protos::pbzero::DrmVblankEventFtraceEvent::Decoder evt(blob.data,
                                                             blob.size);
      DrmVblankEvent(timestamp, evt.crtc(), evt.seq());
      break;
    }
    case FtraceEvent::kDrmVblankEventDeliveredFieldNumber: {
      protos::pbzero::DrmVblankEventDeliveredFtraceEvent::Decoder evt(
          blob.data, blob.size);
      DrmVblankEventDelivered(timestamp, evt.crtc(), evt.seq());
      break;
    }

    case FtraceEvent::kDrmSchedJobFieldNumber: {
      protos::pbzero::DrmSchedJobFtraceEvent::Decoder evt(blob.data, blob.size);
      DrmSchedJob(timestamp, pid, evt.name(), evt.id());
      break;
    }
    case FtraceEvent::kDrmRunJobFieldNumber: {
      protos::pbzero::DrmRunJobFtraceEvent::Decoder evt(blob.data, blob.size);
      DrmRunJob(timestamp, evt.name(), evt.id(), evt.fence());
      break;
    }
    case FtraceEvent::kDrmSchedProcessJobFieldNumber: {
      protos::pbzero::DrmSchedProcessJobFtraceEvent::Decoder evt(blob.data,
                                                                 blob.size);
      DrmSchedProcessJob(timestamp, evt.fence());
      break;
    }
    case FtraceEvent::kDmaFenceInitFieldNumber: {
      protos::pbzero::DmaFenceInitFtraceEvent::Decoder evt(blob.data,
                                                           blob.size);
      DmaFenceInit(timestamp, evt.timeline(), evt.context(), evt.seqno());
      break;
    }
    case FtraceEvent::kDmaFenceEmitFieldNumber: {
      protos::pbzero::DmaFenceEmitFtraceEvent::Decoder evt(blob.data,
                                                           blob.size);
      DmaFenceEmit(timestamp, evt.timeline(), evt.context(), evt.seqno());
      break;
    }
    case FtraceEvent::kDmaFenceSignaledFieldNumber: {
      protos::pbzero::DmaFenceSignaledFtraceEvent::Decoder evt(blob.data,
                                                               blob.size);
      DmaFenceSignaled(timestamp, evt.timeline(), evt.context(), evt.seqno());
      break;
    }
    case FtraceEvent::kDmaFenceWaitStartFieldNumber: {
      protos::pbzero::DmaFenceWaitStartFtraceEvent::Decoder evt(blob.data,
                                                                blob.size);
      DmaFenceWaitStart(timestamp, pid, evt.context(), evt.seqno());
      break;
    }
    case FtraceEvent::kDmaFenceWaitEndFieldNumber: {
      DmaFenceWaitEnd(timestamp, pid);
      break;
    }
    default:
      PERFETTO_DFATAL("Unexpected field id");
      break;
  }
}

TrackId DrmTracker::InternVblankTrack(int32_t crtc) {
  base::StackString<256> track_name("vblank-%d", crtc);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  return context_->track_tracker->InternGpuTrack(
      tables::GpuTrackTable::Row(track_name_id));
}

void DrmTracker::DrmVblankEvent(int64_t timestamp,
                                int32_t crtc,
                                uint32_t seqno) {
  TrackId track_id = InternVblankTrack(crtc);
  auto args_inserter = [this, seqno](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(vblank_arg_seqno_id_, Variadic::UnsignedInteger(seqno));
  };

  context_->slice_tracker->Scoped(timestamp, track_id, kNullStringId,
                                  vblank_slice_signal_id_, 0, args_inserter);
}

void DrmTracker::DrmVblankEventDelivered(int64_t timestamp,
                                         int32_t crtc,
                                         uint32_t seqno) {
  TrackId track_id = InternVblankTrack(crtc);
  auto args_inserter = [this, seqno](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(vblank_arg_seqno_id_, Variadic::UnsignedInteger(seqno));
  };

  context_->slice_tracker->Scoped(timestamp, track_id, kNullStringId,
                                  vblank_slice_deliver_id_, 0, args_inserter);
}

DrmTracker::SchedRing& DrmTracker::GetSchedRingByName(base::StringView name) {
  auto* iter = sched_rings_.Find(name);
  if (iter)
    return **iter;

  // intern a gpu track
  base::StackString<64> track_name("sched-%.*s", int(name.size()), name.data());
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  TrackId track_id = context_->track_tracker->InternGpuTrack(
      tables::GpuTrackTable::Row(track_name_id));

  // no std::make_unique until C++14..
  auto ring = std::unique_ptr<SchedRing>(new SchedRing());
  ring->track_id = track_id;

  SchedRing& ret = *ring;
  sched_rings_.Insert(name, std::move(ring));

  return ret;
}

void DrmTracker::BeginSchedRingSlice(int64_t timestamp, SchedRing& ring) {
  PERFETTO_DCHECK(!ring.running_jobs.empty());
  uint64_t job_id = ring.running_jobs.front();

  auto args_inserter = [this, job_id](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(sched_arg_job_id_, Variadic::UnsignedInteger(job_id));
  };

  base::Optional<SliceId> slice_id =
      context_->slice_tracker->Begin(timestamp, ring.track_id, kNullStringId,
                                     sched_slice_job_id_, args_inserter);

  if (slice_id) {
    SliceId* out_slice_id = ring.out_slice_ids.Find(job_id);
    if (out_slice_id) {
      context_->flow_tracker->InsertFlow(*out_slice_id, *slice_id);
      ring.out_slice_ids.Erase(job_id);
    }
  }
}

void DrmTracker::DrmSchedJob(int64_t timestamp,
                             uint32_t pid,
                             base::StringView name,
                             uint64_t job_id) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  StringId ring_id = context_->storage->InternString(name);
  auto args_inserter = [this, ring_id,
                        job_id](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(sched_arg_ring_id_, Variadic::String(ring_id));
    inserter->AddArg(sched_arg_job_id_, Variadic::UnsignedInteger(job_id));
  };

  base::Optional<SliceId> slice_id = context_->slice_tracker->Scoped(
      timestamp, track_id, kNullStringId, sched_slice_schedule_id_, 0,
      args_inserter);

  if (slice_id) {
    SchedRing& ring = GetSchedRingByName(name);
    ring.out_slice_ids[job_id] = *slice_id;
  }
}

void DrmTracker::DrmRunJob(int64_t timestamp,
                           base::StringView name,
                           uint64_t job_id,
                           uint64_t fence_id) {
  SchedRing& ring = GetSchedRingByName(name);

  ring.running_jobs.push_back(job_id);
  sched_pending_fences_.Insert(fence_id, &ring);

  if (ring.running_jobs.size() == 1)
    BeginSchedRingSlice(timestamp, ring);
}

void DrmTracker::DrmSchedProcessJob(int64_t timestamp, uint64_t fence_id) {
  // look up ring using fence_id
  auto* iter = sched_pending_fences_.Find(fence_id);
  if (!iter)
    return;
  SchedRing& ring = **iter;
  sched_pending_fences_.Erase(fence_id);

  ring.running_jobs.pop_front();
  context_->slice_tracker->End(timestamp, ring.track_id);

  if (!ring.running_jobs.empty())
    BeginSchedRingSlice(timestamp, ring);
}

DrmTracker::FenceTimeline& DrmTracker::GetFenceTimelineByContext(
    uint32_t context,
    base::StringView name) {
  auto* iter = fence_timelines_.Find(context);
  if (iter)
    return **iter;

  // intern a gpu track
  base::StackString<64> track_name("fence-%.*s-%u", int(name.size()),
                                   name.data(), context);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  TrackId track_id = context_->track_tracker->InternGpuTrack(
      tables::GpuTrackTable::Row(track_name_id));

  // no std::make_unique until C++14..
  auto timeline = std::unique_ptr<FenceTimeline>(new FenceTimeline());
  timeline->track_id = track_id;

  FenceTimeline& ret = *timeline;
  fence_timelines_.Insert(context, std::move(timeline));

  return ret;
}

void DrmTracker::BeginFenceTimelineSlice(int64_t timestamp,
                                         const FenceTimeline& timeline) {
  PERFETTO_DCHECK(!timeline.pending_fences.empty());
  uint32_t seqno = timeline.pending_fences.front();

  auto args_inserter = [this, seqno](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(fence_arg_seqno_id_, Variadic::UnsignedInteger(seqno));
  };

  context_->slice_tracker->Begin(timestamp, timeline.track_id, kNullStringId,
                                 fence_slice_fence_id_, args_inserter);
}

void DrmTracker::DmaFenceInit(int64_t timestamp,
                              base::StringView name,
                              uint32_t context,
                              uint32_t seqno) {
  if (name == kUnboundFenceTimeline)
    return;

  FenceTimeline& timeline = GetFenceTimelineByContext(context, name);
  // ignore dma_fence_init when the timeline has dma_fence_emit
  if (timeline.has_dma_fence_emit)
    return;

  timeline.pending_fences.push_back(seqno);

  if (timeline.pending_fences.size() == 1)
    BeginFenceTimelineSlice(timestamp, timeline);
}

void DrmTracker::DmaFenceEmit(int64_t timestamp,
                              base::StringView name,
                              uint32_t context,
                              uint32_t seqno) {
  if (name == kUnboundFenceTimeline)
    return;

  FenceTimeline& timeline = GetFenceTimelineByContext(context, name);

  // Most timelines do not have dma_fence_emit and we rely on the less
  // accurate dma_fence_init instead.  But for those who do, we will switch to
  // dma_fence_emit.
  if (!timeline.has_dma_fence_emit) {
    timeline.has_dma_fence_emit = true;

    if (!timeline.pending_fences.empty()) {
      context_->slice_tracker->End(timestamp, timeline.track_id);
      timeline.pending_fences.clear();
    }
  }

  timeline.pending_fences.push_back(seqno);

  if (timeline.pending_fences.size() == 1)
    BeginFenceTimelineSlice(timestamp, timeline);
}

void DrmTracker::DmaFenceSignaled(int64_t timestamp,
                                  base::StringView name,
                                  uint32_t context,
                                  uint32_t seqno) {
  if (name == kUnboundFenceTimeline)
    return;

  FenceTimeline& timeline = GetFenceTimelineByContext(context, name);
  if (timeline.pending_fences.empty() ||
      seqno < timeline.pending_fences.front()) {
    return;
  }

  timeline.pending_fences.pop_front();
  context_->slice_tracker->End(timestamp, timeline.track_id);

  if (!timeline.pending_fences.empty())
    BeginFenceTimelineSlice(timestamp, timeline);
}

void DrmTracker::DmaFenceWaitStart(int64_t timestamp,
                                   uint32_t pid,
                                   uint32_t context,
                                   uint32_t seqno) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);
  auto args_inserter = [this, context,
                        seqno](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(fence_arg_context_id_, Variadic::UnsignedInteger(context));
    inserter->AddArg(fence_arg_seqno_id_, Variadic::UnsignedInteger(seqno));
  };

  context_->slice_tracker->Begin(timestamp, track_id, kNullStringId,
                                 fence_slice_wait_id_, args_inserter);
}

void DrmTracker::DmaFenceWaitEnd(int64_t timestamp, uint32_t pid) {
  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);

  context_->slice_tracker->End(timestamp, track_id);
}

}  // namespace trace_processor
}  // namespace perfetto
