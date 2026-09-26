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
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/proto_to_args_parser.h"

#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;
class PacketSequenceStateGeneration;

class AndroidJobSchedulerTracker : public TrackEventExtensionParser {
 public:
  static constexpr uint32_t kJobSchedulerJobExtensionFieldId = ::com::android::
      internal::pbzero::FrameworksBaseTrackEvent::kJobSchedulerJobFieldNumber;

  AndroidJobSchedulerTracker(TrackEventExtensionParserContext*,
                             TraceProcessorContext*);

  Result OnTrackEventField(const TrackEventExtensionField& field,
                           const TrackEventFieldContext& event) override;

 private:
  // Caches both the protobuf descriptor and the resolved StringId per integer
  // enum value.
  //
  // In steady-state trace processing, each JobScheduler event queries ~10+
  // enums (state, priorities, bucket, stop reasons, pending reasons). Without
  // this cache, every event would:
  //   1) Incur heap allocation from DescriptorPool::FindEnumString returning
  //      std::optional<std::string> by value.
  //   2) Incur a second heap copy via the ternary (*name vs fallback).
  //   3) Compute MurmurHash64 and probe the StringPool hash table.
  //
  // Since enums have a tiny domain of values (~5-25 per enum), caching
  // int32_t -> StringId turns steady-state lookups into allocation-free,
  // string-hash-free O(1) integer table lookups.
  struct EnumCache {
    DescriptorPool::CachedDescriptor descriptor;
    base::FlatHashMap<int32_t, StringId> string_ids;
  };

  StringId InternEnum(EnumCache& cache,
                      const char* enum_name,
                      std::optional<int32_t> value,
                      int32_t default_value);

  TraceProcessorContext* const trace_context_;
  EnumCache state_cache_;
  EnumCache standby_bucket_cache_;
  EnumCache requested_priority_cache_;
  EnumCache effective_priority_cache_;
  EnumCache proc_state_cache_;
  EnumCache internal_stop_reason_cache_;
  EnumCache public_stop_reason_cache_;
  EnumCache backoff_policy_cache_;
  EnumCache pending_reason_cache_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_JOB_SCHEDULER_ANDROID_JOB_SCHEDULER_TRACKER_H_
