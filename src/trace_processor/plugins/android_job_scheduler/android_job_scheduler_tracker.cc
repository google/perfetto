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
#include "perfetto/protozero/proto_decoder.h"
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
    int64_t ts,
    PacketSequenceStateGeneration* sequence_state) {
  if (field.id() != kJobSchedulerJobExtensionFieldId) {
    return Result::kIgnored;
  }

  auto job_blob = field.Cast<com::android::internal::pbzero::
                                 FrameworksBaseTrackEvent::kJobSchedulerJob>();
  protozero::ProtoDecoder decoder(job_blob.data, job_blob.size);

  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;

  StringId job_name_id = kNullStringId;
  auto job_name_iid_field = decoder.FindField(ProtoJob::kJobNameIidFieldNumber);
  if (job_name_iid_field.valid() && sequence_state) {
    // Resolve the sequence-local interned job name ID to the actual string
    // and intern it into the global string pool to get a global StringId.
    // SQL queries on this table will automatically resolve this ID back
    // to the string value.
    auto* job_name_decoder = sequence_state->LookupInternedMessage<
        com::android::internal::pbzero::FrameworksBaseInternedData::
            kAndroidJobNameFieldNumber,
        com::android::internal::pbzero::AndroidJobName>(
        job_name_iid_field.as_uint64());
    if (job_name_decoder) {
      job_name_id = trace_context_->storage->InternString(base::StringView(
          reinterpret_cast<const char*>(job_name_decoder->name().data),
          job_name_decoder->name().size));
    }
  }

  auto* table = trace_context_->storage
                    ->mutable_android_job_scheduler_track_event_table();

  auto get_opt_int = [&](uint32_t field_id) -> std::optional<int64_t> {
    auto field = decoder.FindField(field_id);
    return field.valid() ? std::make_optional(field.as_int64()) : std::nullopt;
  };

  auto get_opt_int32 = [&](uint32_t field_id) -> std::optional<int32_t> {
    auto field = decoder.FindField(field_id);
    return field.valid() ? std::make_optional(field.as_int32()) : std::nullopt;
  };

  auto row_id = table->Insert({
      ts,
      slice_id,
      decoder.FindField(ProtoJob::kJobIdFieldNumber).as_int64(),
      decoder.FindField(ProtoJob::kSourceUidFieldNumber).as_int64(),
      get_opt_int(ProtoJob::kProxyUidFieldNumber),
      InternEnum(state_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.JobState",
                 get_opt_int(ProtoJob::kStateFieldNumber),
                 ProtoJob::JOB_STATE_UNKNOWN),
      InternEnum(standby_bucket_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.StandbyBucket",
                 get_opt_int(ProtoJob::kStandbyBucketFieldNumber),
                 ProtoJob::STANDBY_BUCKET_UNKNOWN),
      InternEnum(requested_priority_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.JobPriority",
                 get_opt_int(ProtoJob::kRequestedPriorityFieldNumber),
                 ProtoJob::JOB_PRIORITY_UNKNOWN),
      InternEnum(effective_priority_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.JobPriority",
                 get_opt_int(ProtoJob::kEffectivePriorityFieldNumber),
                 ProtoJob::JOB_PRIORITY_UNKNOWN),
      get_opt_int32(ProtoJob::kNumPreviousAttemptsFieldNumber),
      get_opt_int(ProtoJob::kDeadlineMsFieldNumber),
      get_opt_int(ProtoJob::kDelayMsFieldNumber),
      get_opt_int(ProtoJob::kJobStartLatencyMsFieldNumber),
      get_opt_int32(ProtoJob::kNumUncompletedWorkItemsFieldNumber),
      InternEnum(proc_state_cache_, ".com.android.internal.ProcessStateEnum",
                 get_opt_int(ProtoJob::kProcStateFieldNumber),
                 ::com::android::internal::pbzero::ProcessStateEnum::
                     PROCESS_STATE_UNKNOWN),
      InternEnum(
          internal_stop_reason_cache_,
          ".com.android.internal.AndroidJobSchedulerJob.InternalStopReason",
          get_opt_int(ProtoJob::kInternalStopReasonFieldNumber),
          ProtoJob::INTERNAL_STOP_REASON_UNKNOWN),
      InternEnum(
          public_stop_reason_cache_,
          ".com.android.internal.AndroidJobSchedulerJob.PublicStopReason",
          get_opt_int(ProtoJob::kPublicStopReasonFieldNumber),
          ProtoJob::STOP_REASON_UNDEFINED),
      get_opt_int(ProtoJob::kPeriodicJobIntervalMsFieldNumber),
      get_opt_int(ProtoJob::kPeriodicJobFlexIntervalMsFieldNumber),
      job_name_id,
      get_opt_int32(ProtoJob::kNumReschedulesDueToAbandonmentFieldNumber),
      InternEnum(backoff_policy_cache_,
                 ".com.android.internal.AndroidJobSchedulerJob.BackoffPolicy",
                 get_opt_int(ProtoJob::kBackOffPolicyTypeFieldNumber),
                 ProtoJob::BACKOFF_POLICY_UNKNOWN),
  });

  auto flags_field = decoder.FindField(ProtoJob::kJobStateFlagsFieldNumber);
  if (flags_field.valid()) {
    // The bit layout of job_state_flags matches the constraints defined in
    // frameworks/base/services/core/java/com/android/server/job/controllers/JobStatus.java
    // and documented in frameworks_base_track_event.proto.
    uint64_t flags = flags_field.as_uint64();
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
    std::optional<int64_t> value,
    int32_t default_value) {
  int32_t val = value ? static_cast<int32_t>(*value) : default_value;
  auto name =
      trace_context_->descriptor_pool_->FindEnumString(cache, enum_name, val);
  return trace_context_->storage->InternString(
      base::StringView(name ? *name : std::to_string(val)));
}

}  // namespace trace_processor
}  // namespace perfetto
