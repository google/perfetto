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

#include "src/trace_processor/plugins/android_job_scheduler/android_job_scheduler_tracker.h"

#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/proto_to_args_parser.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_interned_data.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

AndroidJobSchedulerTracker::AndroidJobSchedulerTracker(
    TrackEventExtensionParserContext* plugin_context,
    TraceProcessorContext* context)
    : TrackEventExtensionParser(plugin_context), trace_context_(context) {
  RegisterTrackEventExtension(kJobSchedulerJobExtensionFieldId);
}

TrackEventExtensionParser::Result
AndroidJobSchedulerTracker::OnTrackEventSliceExtension(
    const TrackEventExtensionField& field,
    SliceId slice_id,
    PacketSequenceStateGeneration* sequence_state) {
  if (field.id() != kJobSchedulerJobExtensionFieldId) {
    return Result::kIgnored;
  }

  auto job_blob = field.Cast<com::android::internal::pbzero::
                                 FrameworksBaseTrackEvent::kJobSchedulerJob>();
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  ProtoJob::Decoder job(job_blob.data, job_blob.size);

  StringId job_name_id = kNullStringId;
  if (job.has_job_name_iid() && sequence_state) {
    if (auto id = sequence_state->InternedStringId(
            com::android::internal::pbzero::FrameworksBaseInternedData::
                kAndroidJobNameFieldNumber,
            job.job_name_iid())) {
      job_name_id = *id;
    }
  }

  int64_t ts = trace_context_->storage->slice_table()[slice_id].ts();
  auto* table = trace_context_->storage
                    ->mutable_android_job_scheduler_track_event_table();

  auto row_id = table->Insert({
      ts,
      slice_id,
      job.job_id(),
      job.source_uid(),
      job.has_proxy_uid() ? std::make_optional(job.proxy_uid()) : std::nullopt,
      InternEnum(
          state_cache_, ".com.android.internal.AndroidJobSchedulerJob.JobState",
          job.has_state() ? std::make_optional(job.state()) : std::nullopt,
          ProtoJob::JOB_STATE_UNKNOWN),
      InternEnum(standby_bucket_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.StandbyBucket",
                 job.has_standby_bucket()
                     ? std::make_optional(job.standby_bucket())
                     : std::nullopt,
                 ProtoJob::STANDBY_BUCKET_UNKNOWN),
      InternEnum(requested_priority_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.JobPriority",
                 job.has_requested_priority()
                     ? std::make_optional(job.requested_priority())
                     : std::nullopt,
                 ProtoJob::JOB_PRIORITY_UNKNOWN),
      InternEnum(effective_priority_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.JobPriority",
                 job.has_effective_priority()
                     ? std::make_optional(job.effective_priority())
                     : std::nullopt,
                 ProtoJob::JOB_PRIORITY_UNKNOWN),
      job.has_num_previous_attempts()
          ? std::make_optional(job.num_previous_attempts())
          : std::nullopt,
      job.has_deadline_ms() ? std::make_optional(job.deadline_ms())
                            : std::nullopt,
      job.has_delay_ms() ? std::make_optional(job.delay_ms()) : std::nullopt,
      job.has_job_start_latency_ms()
          ? std::make_optional(job.job_start_latency_ms())
          : std::nullopt,
      job.has_num_uncompleted_work_items()
          ? std::make_optional(job.num_uncompleted_work_items())
          : std::nullopt,
      InternEnum(proc_state_cache_, ".com.android.internal.ProcessStateEnum",
                 job.has_proc_state() ? std::make_optional(job.proc_state())
                                      : std::nullopt,
                 ::com::android::internal::pbzero::ProcessStateEnum::
                     PROCESS_STATE_UNKNOWN),
      InternEnum(
          internal_stop_reason_cache_,
          ".com.android.internal.AndroidJobSchedulerJob.InternalStopReason",
          job.has_internal_stop_reason()
              ? std::make_optional(job.internal_stop_reason())
              : std::nullopt,
          ProtoJob::INTERNAL_STOP_REASON_UNKNOWN),
      InternEnum(
          public_stop_reason_cache_,
          ".com.android.internal.AndroidJobSchedulerJob.PublicStopReason",
          job.has_public_stop_reason()
              ? std::make_optional(job.public_stop_reason())
              : std::nullopt,
          ProtoJob::STOP_REASON_UNDEFINED),
      job.has_periodic_job_interval_ms()
          ? std::make_optional(job.periodic_job_interval_ms())
          : std::nullopt,
      job.has_periodic_job_flex_interval_ms()
          ? std::make_optional(job.periodic_job_flex_interval_ms())
          : std::nullopt,
      job_name_id,
      job.has_num_reschedules_due_to_abandonment()
          ? std::make_optional(job.num_reschedules_due_to_abandonment())
          : std::nullopt,
      InternEnum(backoff_policy_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.BackoffPolicy",
                 job.has_back_off_policy_type()
                     ? std::make_optional(job.back_off_policy_type())
                     : std::nullopt,
                 ProtoJob::BACKOFF_POLICY_UNKNOWN),
  });

  if (job.has_job_state_flags()) {
    // The bit layout of job_state_flags matches the constraints defined in
    // frameworks/base/services/core/java/com/android/server/job/controllers/JobStatus.java
    // and documented in frameworks_base_track_event.proto.
    uint64_t flags = static_cast<uint64_t>(job.job_state_flags());
    auto rr = row_id.row_reference;
    rr.set_has_charging_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_CHARGING_CONSTRAINT) ? 1u : 0u);
    rr.set_has_battery_not_low_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_BATTERY_NOT_LOW_CONSTRAINT) ? 1u
                                                                          : 0u);
    rr.set_has_storage_not_low_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_STORAGE_NOT_LOW_CONSTRAINT) ? 1u
                                                                          : 0u);
    rr.set_has_timing_delay_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_TIMING_DELAY_CONSTRAINT) ? 1u
                                                                       : 0u);
    rr.set_has_deadline_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_DEADLINE_CONSTRAINT) ? 1u : 0u);
    rr.set_has_idle_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_IDLE_CONSTRAINT) ? 1u : 0u);
    rr.set_has_connectivity_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_CONNECTIVITY_CONSTRAINT) ? 1u
                                                                       : 0u);
    rr.set_has_content_trigger_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_CONTENT_TRIGGER_CONSTRAINT) ? 1u
                                                                          : 0u);
    rr.set_is_requested_expedited_job(
        (flags & ProtoJob::JOB_STATE_FLAG_IS_REQUESTED_EXPEDITED_JOB) ? 1u
                                                                      : 0u);
    rr.set_is_running_as_expedited_job(
        (flags & ProtoJob::JOB_STATE_FLAG_IS_RUNNING_AS_EXPEDITED_JOB) ? 1u
                                                                       : 0u);
    rr.set_is_prefetch((flags & ProtoJob::JOB_STATE_FLAG_IS_PREFETCH) ? 1u
                                                                      : 0u);
    rr.set_is_requested_as_user_initiated_job(
        (flags & ProtoJob::JOB_STATE_FLAG_IS_REQUESTED_USER_INITIATED_JOB)
            ? 1u
            : 0u);
    rr.set_is_running_as_user_initiated_job(
        (flags & ProtoJob::JOB_STATE_FLAG_IS_RUNNING_AS_USER_INITIATED_JOB)
            ? 1u
            : 0u);
    rr.set_is_periodic((flags & ProtoJob::JOB_STATE_FLAG_IS_PERIODIC) ? 1u
                                                                      : 0u);
    rr.set_has_flexibility_constraint(
        (flags & ProtoJob::JOB_STATE_FLAG_HAS_FLEXIBILITY_CONSTRAINT) ? 1u
                                                                      : 0u);
    rr.set_can_apply_transport_affinities(
        (flags & ProtoJob::JOB_STATE_FLAG_CAN_APPLY_TRANSPORT_AFFINITIES) ? 1u
                                                                          : 0u);
  }
  // Return kIgnored so that the core parser still populates the generic args
  // table for this extension. This is required to keep legacy queries
  // working, as they extract arguments from the args table.
  // TODO(sanathku): Flip this to return kHandled once legacy queries are
  // migrated to use the stdlib table instead.
  return Result::kIgnored;
}

StringId AndroidJobSchedulerTracker::InternEnum(
    DescriptorPool::CachedDescriptor& cache,
    const char* enum_name,
    std::optional<int32_t> value,
    int32_t default_value) {
  int32_t val = value.value_or(default_value);
  auto name =
      trace_context_->descriptor_pool_->FindEnumString(cache, enum_name, val);
  return trace_context_->storage->InternString(
      base::StringView(name ? *name : std::to_string(val)));
}

}  // namespace trace_processor
}  // namespace perfetto
