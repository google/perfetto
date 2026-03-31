/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_JOB_SCHEDULER_ANDROID_JOB_SCHEDULER_TRACKER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_JOB_SCHEDULER_ANDROID_JOB_SCHEDULER_TRACKER_H_

#include <cstdint>
#include <optional>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/proto_to_args_parser.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;
class PacketSequenceStateGeneration;

class AndroidJobSchedulerTracker : public TrackEventExtensionParser {
 public:
  static constexpr uint32_t kJobSchedulerJobExtensionFieldId = 2006;

  AndroidJobSchedulerTracker(TrackEventExtensionParserContext*,
                             TraceProcessorContext*);

  Result OnTrackEventSliceExtension(
      const TrackEventExtensionField& field,
      SliceId id,
      PacketSequenceStateGeneration* sequence_state) override;

 private:
  StringId InternEnum(DescriptorPool::CachedDescriptor& cache,
                      const char* enum_name,
                      std::optional<int32_t> value,
                      int32_t default_value);

  TraceProcessorContext* const trace_context_;
  DescriptorPool::CachedDescriptor state_cache_;
  DescriptorPool::CachedDescriptor standby_bucket_cache_;
  DescriptorPool::CachedDescriptor requested_priority_cache_;
  DescriptorPool::CachedDescriptor effective_priority_cache_;
  DescriptorPool::CachedDescriptor proc_state_cache_;
  DescriptorPool::CachedDescriptor internal_stop_reason_cache_;
  DescriptorPool::CachedDescriptor public_stop_reason_cache_;
  DescriptorPool::CachedDescriptor backoff_policy_cache_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_JOB_SCHEDULER_ANDROID_JOB_SCHEDULER_TRACKER_H_
