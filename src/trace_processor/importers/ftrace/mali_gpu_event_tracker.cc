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
          context->storage->InternString("mali_KCPU_FENCE_WAIT")) {}

void MaliGpuEventTracker::ParseMaliGpuEvent(int64_t ts,
                                            int32_t field_id,
                                            uint32_t pid) {
  using protos::pbzero::FtraceEvent;

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

void MaliGpuEventTracker::ParseMaliKcpuCqsSet(int64_t timestamp,
                                              TrackId track_id) {
  context_->slice_tracker->Scoped(timestamp, track_id, kNullStringId,
                                  mali_KCPU_CQS_SET_id_, 0);
}

void MaliGpuEventTracker::ParseMaliKcpuCqsWaitStart(int64_t timestamp,
                                                    TrackId track_id) {
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
}  // namespace trace_processor
}  // namespace perfetto
