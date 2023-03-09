#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_MALI_GPU_EVENT_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_MALI_GPU_EVENT_TRACKER_H_

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class MaliGpuEventTracker {
 public:
  explicit MaliGpuEventTracker(TraceProcessorContext*);
  void ParseMaliGpuEvent(int64_t timestamp, int32_t field_id, uint32_t pid);

 private:
  TraceProcessorContext* context_;
  StringId mali_KCPU_CQS_SET_id_;
  StringId mali_KCPU_CQS_WAIT_id_;
  StringId mali_KCPU_FENCE_SIGNAL_id_;
  StringId mali_KCPU_FENCE_WAIT_id_;
  void ParseMaliKcpuFenceSignal(int64_t timestamp, TrackId track_id);
  void ParseMaliKcpuFenceWaitStart(int64_t timestamp, TrackId track_id);
  void ParseMaliKcpuFenceWaitEnd(int64_t timestamp, TrackId track_id);
  void ParseMaliKcpuCqsSet(int64_t timestamp, TrackId track_id);
  void ParseMaliKcpuCqsWaitStart(int64_t timestamp, TrackId track_id);
  void ParseMaliKcpuCqsWaitEnd(int64_t timestamp, TrackId track_id);
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_MALI_GPU_EVENT_TRACKER_H_
