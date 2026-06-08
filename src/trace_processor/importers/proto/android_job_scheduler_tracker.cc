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

#include "src/trace_processor/importers/proto/android_job_scheduler_tracker.h"

#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/proto_to_args_parser.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"

namespace perfetto {
namespace trace_processor {

AndroidJobSchedulerTracker::AndroidJobSchedulerTracker(
    TraceProcessorContext* context)
    : context_(context) {}

void AndroidJobSchedulerTracker::ParseAndroidJobSchedulerJob(
    int64_t ts,
    SliceId slice_id,
    StringId job_name_id,
    const protozero::ConstBytes& blob) {
  protozero::ProtoDecoder decoder(blob.data, blob.size);
  auto* table =
      context_->storage->mutable_android_job_scheduler_track_event_table();

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
      decoder.FindField(kJobId).as_int64(),
      decoder.FindField(kSourceUid).as_int64(),
      get_opt_int(kProxyUid),
      context_->storage->InternString(GetAndroidJobStateName(
          get_opt_int(kState).value_or(kDefaultState))),  // state (UNKNOWN)
      context_->storage->InternString(GetAndroidStandbyBucketName(
          get_opt_int(kStandbyBucket)
              .value_or(kDefaultStandbyBucket))),  // standby_bucket (UNKNOWN)
      context_->storage->InternString(GetAndroidPriorityName(
          get_opt_int(kRequestedPriority)
              .value_or(kDefaultPriority))),  // requested_priority (UNKNOWN)
      context_->storage->InternString(GetAndroidPriorityName(
          get_opt_int(kEffectivePriority)
              .value_or(kDefaultPriority))),  // effective_priority (UNKNOWN)
      get_opt_int32(kNumPreviousAttempts),
      get_opt_int(kDeadlineMs),
      get_opt_int(kDelayMs),
      get_opt_int(kJobStartLatencyMs),
      get_opt_int32(kNumUncompletedWorkItems),
      context_->storage->InternString(GetAndroidProcStateName(
          get_opt_int(kProcState).value_or(kDefaultProcState))),
      context_->storage->InternString(GetAndroidInternalStopReasonName(
          get_opt_int(kInternalStopReason)
              .value_or(kDefaultInternalStopReason))),
      context_->storage->InternString(GetAndroidPublicStopReasonName(
          get_opt_int(kPublicStopReason).value_or(kDefaultPublicStopReason))),
      get_opt_int(kPeriodicJobIntervalMs),
      get_opt_int(kPeriodicJobFlexIntervalMs),
      job_name_id,
      get_opt_int32(kNumReschedulesDueToAbandonment),
      context_->storage->InternString(GetAndroidBackoffPolicyName(
          get_opt_int(kBackOffPolicyType).value_or(kDefaultBackoffPolicy))),
  });

  auto flags_field = decoder.FindField(kJobStateFlags);
  if (flags_field.valid()) {
    // The bit layout of job_state_flags matches the constraints defined in
    // frameworks/base/services/core/java/com/android/server/job/controllers/JobStatus.java
    // and documented in frameworks_base_track_event.proto.
    uint64_t flags = flags_field.as_uint64();
    auto rr = row_id.row_reference;
    rr.set_has_charging_constraint((flags >> kHasChargingConstraint) & 1);
    rr.set_has_battery_not_low_constraint(
        (flags >> kHasBatteryNotLowConstraint) & 1);
    rr.set_has_storage_not_low_constraint(
        (flags >> kHasStorageNotLowConstraint) & 1);
    rr.set_has_timing_delay_constraint((flags >> kHasTimingDelayConstraint) &
                                       1);
    rr.set_has_deadline_constraint((flags >> kHasDeadlineConstraint) & 1);
    rr.set_has_idle_constraint((flags >> kHasIdleConstraint) & 1);
    rr.set_has_connectivity_constraint((flags >> kHasConnectivityConstraint) &
                                       1);
    rr.set_has_content_trigger_constraint(
        (flags >> kHasContentTriggerConstraint) & 1);
    rr.set_is_requested_expedited_job((flags >> kIsRequestedExpeditedJob) & 1);
    rr.set_is_running_as_expedited_job((flags >> kIsRunningAsExpeditedJob) & 1);
    rr.set_is_prefetch((flags >> kIsPrefetch) & 1);
    rr.set_is_requested_as_user_initiated_job(
        (flags >> kIsRequestedAsUserInitiatedJob) & 1);
    rr.set_is_running_as_user_initiated_job(
        (flags >> kIsRunningAsUserInitiatedJob) & 1);
    rr.set_is_periodic((flags >> kIsPeriodic) & 1);
    rr.set_has_flexibility_constraint((flags >> kHasFlexibilityConstraint) & 1);
    rr.set_can_apply_transport_affinities(
        (flags >> kCanApplyTransportAffinities) & 1);
  }
}

const char* AndroidJobSchedulerTracker::GetAndroidJobStateName(int64_t state) {
  switch (state) {
    case 0:
      return JobStates::kFinished;
    case 1:
      return JobStates::kStarted;
    case 2:
      return JobStates::kScheduled;
    case 3:
      return JobStates::kCancelled;
    default:
      return JobStates::kUnknown;
  }
}

const char* AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(
    int64_t bucket) {
  switch (bucket) {
    case 0:
      return StandbyBuckets::kActive;
    case 1:
      return StandbyBuckets::kWorkingSet;
    case 2:
      return StandbyBuckets::kFrequent;
    case 3:
      return StandbyBuckets::kRare;
    case 4:
      return StandbyBuckets::kNever;
    case 5:
      return StandbyBuckets::kRestricted;
    case 6:
      return StandbyBuckets::kExempted;
    default:
      return StandbyBuckets::kUnknown;
  }
}

const char* AndroidJobSchedulerTracker::GetAndroidInternalStopReasonName(
    int64_t reason) {
  switch (reason) {
    case -1:
      return InternalStopReasons::kUnknown;
    case 0:
      return InternalStopReasons::kCanceled;
    case 1:
      return InternalStopReasons::kConstraintsNotSatisfied;
    case 2:
      return InternalStopReasons::kPreempt;
    case 3:
      return InternalStopReasons::kTimeout;
    case 4:
      return InternalStopReasons::kDeviceIdle;
    case 5:
      return InternalStopReasons::kDeviceThermal;
    case 6:
      return InternalStopReasons::kRestrictedBucket;
    case 7:
      return InternalStopReasons::kUninstall;
    case 8:
      return InternalStopReasons::kDataCleared;
    case 9:
      return InternalStopReasons::kRtcUpdated;
    case 10:
      return InternalStopReasons::kSuccessfulFinish;
    case 11:
      return InternalStopReasons::kUserUiStop;
    case 12:
      return InternalStopReasons::kAnr;
    case 13:
      return InternalStopReasons::kTimeoutAbandoned;
    case 14:
      return InternalStopReasons::kDeviceStateBatterySaver;
    default:
      return InternalStopReasons::kUnknown;
  }
}

const char* AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(
    int64_t reason) {
  switch (reason) {
    case 0:
      return PublicStopReasons::kUndefined;
    case 1:
      return PublicStopReasons::kCancelledByApp;
    case 2:
      return PublicStopReasons::kPreempt;
    case 3:
      return PublicStopReasons::kTimeout;
    case 4:
      return PublicStopReasons::kDeviceState;
    case 5:
      return PublicStopReasons::kConstraintBatteryNotLow;
    case 6:
      return PublicStopReasons::kConstraintCharging;
    case 7:
      return PublicStopReasons::kConstraintConnectivity;
    case 8:
      return PublicStopReasons::kConstraintDeviceIdle;
    case 9:
      return PublicStopReasons::kConstraintStorageNotLow;
    case 10:
      return PublicStopReasons::kQuota;
    case 11:
      return PublicStopReasons::kBackgroundRestriction;
    case 12:
      return PublicStopReasons::kAppStandby;
    case 13:
      return PublicStopReasons::kUser;
    case 14:
      return PublicStopReasons::kSystemProcessing;
    case 15:
      return PublicStopReasons::kEstimatedAppLaunchTimeChanged;
    case 16:
      return PublicStopReasons::kTimeoutAbandoned;
    case 17:
      return PublicStopReasons::kDeviceStateThermal;
    case 18:
      return PublicStopReasons::kDeviceStateBatterySaver;
    default:
      return PublicStopReasons::kUndefined;
  }
}

const char* AndroidJobSchedulerTracker::GetAndroidBackoffPolicyName(
    int64_t policy) {
  switch (policy) {
    case 0:
      return BackoffPolicies::kUnknown;
    case 1:
      return BackoffPolicies::kLinear;
    case 2:
      return BackoffPolicies::kExponential;
    default:
      return BackoffPolicies::kUnknown;
  }
}

const char* AndroidJobSchedulerTracker::GetAndroidProcStateName(int64_t state) {
  switch (state) {
    case 0:
      return ProcStates::kUnspecified;
    case 998:
      return ProcStates::kUnknownToProto;
    case 999:
      return ProcStates::kUnknown;
    case 1000:
      return ProcStates::kPersistent;
    case 1001:
      return ProcStates::kPersistentUi;
    case 1002:
      return ProcStates::kTop;
    case 1003:
      return ProcStates::kForegroundService;
    case 1004:
      return ProcStates::kBoundForegroundService;
    case 1005:
      return ProcStates::kImportantForeground;
    case 1006:
      return ProcStates::kImportantBackground;
    case 1007:
      return ProcStates::kTransientBackground;
    case 1008:
      return ProcStates::kBackup;
    case 1009:
      return ProcStates::kService;
    case 1010:
      return ProcStates::kReceiver;
    case 1011:
      return ProcStates::kTopSleeping;
    case 1012:
      return ProcStates::kHeavyWeight;
    case 1013:
      return ProcStates::kHome;
    case 1014:
      return ProcStates::kLastActivity;
    case 1015:
      return ProcStates::kCachedActivity;
    case 1016:
      return ProcStates::kCachedActivityClient;
    case 1017:
      return ProcStates::kCachedRecent;
    case 1018:
      return ProcStates::kCachedEmpty;
    case 1019:
      return ProcStates::kNonexistent;
    case 1020:
      return ProcStates::kBoundTop;
    default:
      return ProcStates::kUnknown;
  }
}

const char* AndroidJobSchedulerTracker::GetAndroidPriorityName(
    int64_t priority) {
  switch (priority) {
    case 100:
      return Priorities::kMin;
    case 200:
      return Priorities::kLow;
    case 300:
      return Priorities::kDefault;
    case 400:
      return Priorities::kHigh;
    case 500:
      return Priorities::kMax;
    default:
      return Priorities::kUnknown;
  }
}

std::optional<base::Status>
AndroidJobSchedulerTracker::MaybeParseAndroidJobName(
    const protozero::Field& field,
    util::ProtoToArgsParser::Delegate& delegate) {
  auto* decoder = delegate.GetInternedMessage(
      protos::pbzero::InternedData::kAndroidJobName, field.as_uint64());
  if (!decoder) {
    return std::nullopt;
  }

  delegate.AddString(util::ProtoToArgsParser::Key("job_scheduler_job.job_name"),
                     decoder->name());
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
