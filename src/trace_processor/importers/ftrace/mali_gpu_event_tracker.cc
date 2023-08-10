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

#include "src/trace_processor/importers/ftrace/mali_gpu_event_tracker.h"

#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/mali.pbzero.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"

namespace perfetto {
namespace trace_processor {

MaliGpuEventTracker::MaliGpuEventTracker(TraceProcessorContext* context)
    : context_(context),
      mali_KCPU_CQS_SET_id_(
          context->storage->InternString("mali_KCPU_CQS_SET")),
      mali_KCPU_CQS_WAIT_id_(
          context->storage->InternString("mali_KCPU_CQS_WAIT")),
      mali_KCPU_FENCE_SIGNAL_id_(
          context->storage->InternString("mali_KCPU_FENCE_SIGNAL")),
      mali_KCPU_FENCE_WAIT_id_(
          context->storage->InternString("mali_KCPU_FENCE_WAIT")),
      mali_CSF_INTERRUPT_id_(
          context->storage->InternString("mali_CSF_INTERRUPT")),
      mali_CSF_INTERRUPT_info_val_id_(
          context->storage->InternString("info_val")) {}

void MaliGpuEventTracker::ParseMaliGpuEvent(int64_t ts,
                                            uint32_t field_id,
                                            uint32_t pid) {
  using protos::pbzero::FtraceEvent;

  // It seems like it is not correct to add to add any of these slices
  // in the normal thread slice track since they are not guaranteed to
  // be correctly nested with respect to atrace events.
  // For now just disable all mali events by early returning here.
  // TODO(b/294866695): Consider how to best visualise these events.
  if (ts != 0) {
    return;
  }

  UniqueTid utid = context_->process_tracker->GetOrCreateThread(pid);
  TrackId track_id = context_->track_tracker->InternThreadTrack(utid);

  switch (field_id) {
    case FtraceEvent::kMaliMaliKCPUCQSSETFieldNumber: {
      ParseMaliKcpuCqsSet(ts, track_id);
      break;
    }
    case FtraceEvent::kMaliMaliKCPUCQSWAITSTARTFieldNumber: {
      ParseMaliKcpuCqsWaitStart(ts, track_id);
      break;
    }
    case FtraceEvent::kMaliMaliKCPUCQSWAITENDFieldNumber: {
      ParseMaliKcpuCqsWaitEnd(ts, track_id);
      break;
    }
    case FtraceEvent::kMaliMaliKCPUFENCESIGNALFieldNumber: {
      ParseMaliKcpuFenceSignal(ts, track_id);
      break;
    }
    case FtraceEvent::kMaliMaliKCPUFENCEWAITSTARTFieldNumber: {
      ParseMaliKcpuFenceWaitStart(ts, track_id);
      break;
    }
    case FtraceEvent::kMaliMaliKCPUFENCEWAITENDFieldNumber: {
      ParseMaliKcpuFenceWaitEnd(ts, track_id);
      break;
    }
    default:
      PERFETTO_DFATAL("Unexpected field id");
      break;
  }
}

void MaliGpuEventTracker::ParseMaliGpuIrqEvent(int64_t ts,
                                               uint32_t field_id,
                                               uint32_t cpu,
                                               protozero::ConstBytes blob) {
  using protos::pbzero::FtraceEvent;

  // Since these events are called from an interrupt context they cannot be
  // associated to a single process or thread. Add to a custom Mali Irq track
  // instead.
  base::StackString<255> track_name("Mali Irq Cpu %d", cpu);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  TrackId track_id =
      context_->track_tracker->InternCpuTrack(track_name_id, cpu);

  switch (field_id) {
    case FtraceEvent::kMaliMaliCSFINTERRUPTSTARTFieldNumber: {
      ParseMaliCSFInterruptStart(ts, track_id, blob);
      break;
    }
    case FtraceEvent::kMaliMaliCSFINTERRUPTENDFieldNumber: {
      ParseMaliCSFInterruptEnd(ts, track_id, blob);
      break;
    }
    default:
      PERFETTO_DFATAL("Unexpected field id");
      break;
  }
}

void MaliGpuEventTracker::ParseMaliKcpuCqsSet(int64_t timestamp,
                                              TrackId track_id) {
  context_->slice_tracker->Scoped(timestamp, track_id, kNullStringId,
                                  mali_KCPU_CQS_SET_id_, 0);
}

PERFETTO_NORETURN void MaliGpuEventTracker::ParseMaliKcpuCqsWaitStart(
    int64_t timestamp,
    TrackId track_id) {
  // TODO(b/294866695): Remove
  PERFETTO_FATAL("This causes incorrectly nested slices at present.");
  context_->slice_tracker->Begin(timestamp, track_id, kNullStringId,
                                 mali_KCPU_CQS_WAIT_id_);
}

void MaliGpuEventTracker::ParseMaliKcpuCqsWaitEnd(int64_t timestamp,
                                                  TrackId track_id) {
  context_->slice_tracker->End(timestamp, track_id, kNullStringId,
                               mali_KCPU_CQS_WAIT_id_);
}

void MaliGpuEventTracker::ParseMaliKcpuFenceSignal(int64_t timestamp,
                                                   TrackId track_id) {
  context_->slice_tracker->Scoped(timestamp, track_id, kNullStringId,
                                  mali_KCPU_FENCE_SIGNAL_id_, 0);
}

void MaliGpuEventTracker::ParseMaliKcpuFenceWaitStart(int64_t timestamp,
                                                      TrackId track_id) {
  context_->slice_tracker->Begin(timestamp, track_id, kNullStringId,
                                 mali_KCPU_FENCE_WAIT_id_);
}

void MaliGpuEventTracker::ParseMaliKcpuFenceWaitEnd(int64_t timestamp,
                                                    TrackId track_id) {
  context_->slice_tracker->End(timestamp, track_id, kNullStringId,
                               mali_KCPU_FENCE_WAIT_id_);
}

void MaliGpuEventTracker::ParseMaliCSFInterruptStart(
    int64_t timestamp,
    TrackId track_id,
    protozero::ConstBytes blob) {
  protos::pbzero::MaliMaliCSFINTERRUPTSTARTFtraceEvent::Decoder evt(blob.data,
                                                                    blob.size);
  auto args_inserter = [this, &evt](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(mali_CSF_INTERRUPT_info_val_id_,
                     Variadic::UnsignedInteger(evt.info_val()));
  };

  context_->slice_tracker->Begin(timestamp, track_id, kNullStringId,
                                 mali_CSF_INTERRUPT_id_, args_inserter);
}

void MaliGpuEventTracker::ParseMaliCSFInterruptEnd(int64_t timestamp,
                                                   TrackId track_id,
                                                   protozero::ConstBytes blob) {
  protos::pbzero::MaliMaliCSFINTERRUPTSTARTFtraceEvent::Decoder evt(blob.data,
                                                                    blob.size);
  auto args_inserter = [this, &evt](ArgsTracker::BoundInserter* inserter) {
    inserter->AddArg(mali_CSF_INTERRUPT_info_val_id_,
                     Variadic::UnsignedInteger(evt.info_val()));
  };

  context_->slice_tracker->End(timestamp, track_id, kNullStringId,
                               mali_CSF_INTERRUPT_id_, args_inserter);
}
}  // namespace trace_processor
}  // namespace perfetto
