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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_JOB_SCHEDULER_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_JOB_SCHEDULER_TRACKER_H_

#include <cstdint>
#include <optional>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/proto_to_args_parser.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class AndroidJobSchedulerTracker {
 public:
  static constexpr uint32_t kJobSchedulerJobExtensionFieldId = 2006;

  // Field IDs for AndroidJobSchedulerJob proto.
  enum JobFieldIds {
    kJobId = 1,
    kSourceUid = 2,
    kProxyUid = 3,
    kState = 4,
    kStandbyBucket = 5,
    kRequestedPriority = 6,
    kEffectivePriority = 7,
    kNumPreviousAttempts = 8,
    kDeadlineMs = 9,
    kDelayMs = 10,
    kJobStartLatencyMs = 11,
    kNumUncompletedWorkItems = 12,
    kProcState = 13,
    kPeriodicJobIntervalMs = 14,
    kPeriodicJobFlexIntervalMs = 15,
    kNumReschedulesDueToAbandonment = 16,
    kBackOffPolicyType = 17,
    kInternalStopReason = 18,
    kPublicStopReason = 19,
    kJobStateFlags = 20,
    kJobNameIid = 21,
  };

  // Bit positions for job_state_flags bitmask.
  enum JobStateFlags {
    kHasChargingConstraint = 0,
    kHasBatteryNotLowConstraint = 1,
    kHasStorageNotLowConstraint = 2,
    kHasTimingDelayConstraint = 3,
    kHasDeadlineConstraint = 4,
    kHasIdleConstraint = 5,
    kHasConnectivityConstraint = 6,
    kHasContentTriggerConstraint = 7,
    kIsRequestedExpeditedJob = 8,
    kIsRunningAsExpeditedJob = 9,
    kIsPrefetch = 10,
    // Bits 11-18 are "satisfied" constraints on the device.
    kIsRequestedAsUserInitiatedJob = 19,
    kIsRunningAsUserInitiatedJob = 20,
    kIsPeriodic = 21,
    kHasFlexibilityConstraint = 22,
    kCanApplyTransportAffinities = 24,
  };

  // Fallback values for optional fields when they are missing in the trace.
  // These match the "unknown" or default states.
  static constexpr int64_t kDefaultState = 4;           // UNKNOWN
  static constexpr int64_t kDefaultStandbyBucket = -1;  // UNKNOWN
  static constexpr int64_t kDefaultPriority = 501;      // PRIORITY_UNKNOWN
  static constexpr int64_t kDefaultProcState = 999;     // PROCESS_STATE_UNKNOWN
  static constexpr int64_t kDefaultInternalStopReason =
      -1;  // INTERNAL_STOP_REASON_UNKNOWN
  static constexpr int64_t kDefaultPublicStopReason =
      0;                                               // STOP_REASON_UNDEFINED
  static constexpr int64_t kDefaultBackoffPolicy = 0;  // UNKNOWN_POLICY

  struct JobStates {
    static constexpr char kFinished[] = "FINISHED";
    static constexpr char kStarted[] = "STARTED";
    static constexpr char kScheduled[] = "SCHEDULED";
    static constexpr char kCancelled[] = "CANCELLED";
    static constexpr char kUnknown[] = "UNKNOWN";
  };

  struct StandbyBuckets {
    static constexpr char kActive[] = "ACTIVE";
    static constexpr char kWorkingSet[] = "WORKING_SET";
    static constexpr char kFrequent[] = "FREQUENT";
    static constexpr char kRare[] = "RARE";
    static constexpr char kNever[] = "NEVER";
    static constexpr char kRestricted[] = "RESTRICTED";
    static constexpr char kExempted[] = "EXEMPTED";
    static constexpr char kUnknown[] = "UNKNOWN";
  };

  struct InternalStopReasons {
    static constexpr char kUnknown[] = "INTERNAL_STOP_REASON_UNKNOWN";
    static constexpr char kCanceled[] = "INTERNAL_STOP_REASON_CANCELED";
    static constexpr char kConstraintsNotSatisfied[] =
        "INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED";
    static constexpr char kPreempt[] = "INTERNAL_STOP_REASON_PREEMPT";
    static constexpr char kTimeout[] = "INTERNAL_STOP_REASON_TIMEOUT";
    static constexpr char kDeviceIdle[] = "INTERNAL_STOP_REASON_DEVICE_IDLE";
    static constexpr char kDeviceThermal[] =
        "INTERNAL_STOP_REASON_DEVICE_THERMAL";
    static constexpr char kRestrictedBucket[] =
        "INTERNAL_STOP_REASON_RESTRICTED_BUCKET";
    static constexpr char kUninstall[] = "INTERNAL_STOP_REASON_UNINSTALL";
    static constexpr char kDataCleared[] = "INTERNAL_STOP_REASON_DATA_CLEARED";
    static constexpr char kRtcUpdated[] = "INTERNAL_STOP_REASON_RTC_UPDATED";
    static constexpr char kSuccessfulFinish[] =
        "INTERNAL_STOP_REASON_SUCCESSFUL_FINISH";
    static constexpr char kUserUiStop[] = "INTERNAL_STOP_REASON_USER_UI_STOP";
    static constexpr char kAnr[] = "INTERNAL_STOP_REASON_ANR";
    static constexpr char kTimeoutAbandoned[] =
        "INTERNAL_STOP_REASON_TIMEOUT_ABANDONED";
    static constexpr char kDeviceStateBatterySaver[] =
        "INTERNAL_STOP_REASON_DEVICE_STATE_BATTERY_SAVER";
  };

  struct PublicStopReasons {
    static constexpr char kUndefined[] = "STOP_REASON_UNDEFINED";
    static constexpr char kCancelledByApp[] = "STOP_REASON_CANCELLED_BY_APP";
    static constexpr char kPreempt[] = "STOP_REASON_PREEMPT";
    static constexpr char kTimeout[] = "STOP_REASON_TIMEOUT";
    static constexpr char kDeviceState[] = "STOP_REASON_DEVICE_STATE";
    static constexpr char kConstraintBatteryNotLow[] =
        "STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW";
    static constexpr char kConstraintCharging[] =
        "STOP_REASON_CONSTRAINT_CHARGING";
    static constexpr char kConstraintConnectivity[] =
        "STOP_REASON_CONSTRAINT_CONNECTIVITY";
    static constexpr char kConstraintDeviceIdle[] =
        "STOP_REASON_CONSTRAINT_DEVICE_IDLE";
    static constexpr char kConstraintStorageNotLow[] =
        "STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW";
    static constexpr char kQuota[] = "STOP_REASON_QUOTA";
    static constexpr char kBackgroundRestriction[] =
        "STOP_REASON_BACKGROUND_RESTRICTION";
    static constexpr char kAppStandby[] = "STOP_REASON_APP_STANDBY";
    static constexpr char kUser[] = "STOP_REASON_USER";
    static constexpr char kSystemProcessing[] = "STOP_REASON_SYSTEM_PROCESSING";
    static constexpr char kEstimatedAppLaunchTimeChanged[] =
        "STOP_REASON_ESTIMATED_APP_LAUNCH_TIME_CHANGED";
    static constexpr char kTimeoutAbandoned[] = "STOP_REASON_TIMEOUT_ABANDONED";
    static constexpr char kDeviceStateThermal[] =
        "STOP_REASON_DEVICE_STATE_THERMAL";
    static constexpr char kDeviceStateBatterySaver[] =
        "STOP_REASON_DEVICE_STATE_BATTERY_SAVER";
  };

  struct BackoffPolicies {
    static constexpr char kUnknown[] = "UNKNOWN_POLICY";
    static constexpr char kLinear[] = "LINEAR";
    static constexpr char kExponential[] = "EXPONENTIAL";
  };

  struct ProcStates {
    static constexpr char kUnspecified[] = "PROCESS_STATE_UNSPECIFIED";
    static constexpr char kUnknownToProto[] = "PROCESS_STATE_UNKNOWN_TO_PROTO";
    static constexpr char kUnknown[] = "PROCESS_STATE_UNKNOWN";
    static constexpr char kPersistent[] = "PROCESS_STATE_PERSISTENT";
    static constexpr char kPersistentUi[] = "PROCESS_STATE_PERSISTENT_UI";
    static constexpr char kTop[] = "PROCESS_STATE_TOP";
    static constexpr char kForegroundService[] =
        "PROCESS_STATE_FOREGROUND_SERVICE";
    static constexpr char kBoundForegroundService[] =
        "PROCESS_STATE_BOUND_FOREGROUND_SERVICE";
    static constexpr char kImportantForeground[] =
        "PROCESS_STATE_IMPORTANT_FOREGROUND";
    static constexpr char kImportantBackground[] =
        "PROCESS_STATE_IMPORTANT_BACKGROUND";
    static constexpr char kTransientBackground[] =
        "PROCESS_STATE_TRANSIENT_BACKGROUND";
    static constexpr char kBackup[] = "PROCESS_STATE_BACKUP";
    static constexpr char kService[] = "PROCESS_STATE_SERVICE";
    static constexpr char kReceiver[] = "PROCESS_STATE_RECEIVER";
    static constexpr char kTopSleeping[] = "PROCESS_STATE_TOP_SLEEPING";
    static constexpr char kHeavyWeight[] = "PROCESS_STATE_HEAVY_WEIGHT";
    static constexpr char kHome[] = "PROCESS_STATE_HOME";
    static constexpr char kLastActivity[] = "PROCESS_STATE_LAST_ACTIVITY";
    static constexpr char kCachedActivity[] = "PROCESS_STATE_CACHED_ACTIVITY";
    static constexpr char kCachedActivityClient[] =
        "PROCESS_STATE_CACHED_ACTIVITY_CLIENT";
    static constexpr char kCachedRecent[] = "PROCESS_STATE_CACHED_RECENT";
    static constexpr char kCachedEmpty[] = "PROCESS_STATE_CACHED_EMPTY";
    static constexpr char kNonexistent[] = "PROCESS_STATE_NONEXISTENT";
    static constexpr char kBoundTop[] = "PROCESS_STATE_BOUND_TOP";
  };

  struct Priorities {
    static constexpr char kMin[] = "PRIORITY_MIN";
    static constexpr char kLow[] = "PRIORITY_LOW";
    static constexpr char kDefault[] = "PRIORITY_DEFAULT";
    static constexpr char kHigh[] = "PRIORITY_HIGH";
    static constexpr char kMax[] = "PRIORITY_MAX";
    static constexpr char kUnknown[] = "PRIORITY_UNKNOWN";
  };

  explicit AndroidJobSchedulerTracker(TraceProcessorContext*);

  void ParseAndroidJobSchedulerJob(int64_t ts,
                                   SliceId slice_id,
                                   StringId job_name_id,
                                   const protozero::ConstBytes&);

  static const char* GetAndroidJobStateName(int64_t state);
  static const char* GetAndroidStandbyBucketName(int64_t bucket);
  static const char* GetAndroidInternalStopReasonName(int64_t reason);
  static const char* GetAndroidPublicStopReasonName(int64_t reason);
  static const char* GetAndroidBackoffPolicyName(int64_t policy);
  static const char* GetAndroidProcStateName(int64_t state);
  static const char* GetAndroidPriorityName(int64_t priority);

  static std::optional<base::Status> MaybeParseAndroidJobName(
      const protozero::Field& field,
      util::ProtoToArgsParser::Delegate& delegate);

 private:
  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_JOB_SCHEDULER_TRACKER_H_
