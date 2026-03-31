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
#include "src/trace_processor/importers/proto/track_event_parser.h"

#include <memory>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/track_event_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class AndroidJobSchedulerTrackerTest : public ::testing::Test {
 public:
  AndroidJobSchedulerTrackerTest() {
    context.storage.reset(new TraceStorage());
    context.global_args_tracker.reset(
        new GlobalArgsTracker(context.storage.get()));
    context.machine_tracker.reset(new MachineTracker(&context, 0));
    context.track_tracker.reset(new TrackTracker(&context));
    context.slice_tracker.reset(new SliceTracker(&context));
    context.event_tracker.reset(new EventTracker(&context));
    context.process_tracker.reset(new ProcessTracker(&context));
    context.mapping_tracker.reset(new MappingTracker(&context));
    context.flow_tracker.reset(new FlowTracker(&context));
    context.args_translation_table.reset(
        new ArgsTranslationTable(context.storage.get()));
    track_event_tracker.reset(new TrackEventTracker(&context));
    parser.reset(new TrackEventParser(&context, track_event_tracker.get()));
  }

  TraceProcessorContext context;
  std::unique_ptr<TrackEventTracker> track_event_tracker;
  std::unique_ptr<TrackEventParser> parser;
};

TEST_F(AndroidJobSchedulerTrackerTest, AndroidJobStateMapping) {
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidJobStateName(0),
               AndroidJobSchedulerTracker::JobStates::kFinished);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidJobStateName(1),
               AndroidJobSchedulerTracker::JobStates::kStarted);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidJobStateName(2),
               AndroidJobSchedulerTracker::JobStates::kScheduled);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidJobStateName(3),
               AndroidJobSchedulerTracker::JobStates::kCancelled);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidJobStateName(4),
               AndroidJobSchedulerTracker::JobStates::kUnknown);
}

TEST_F(AndroidJobSchedulerTrackerTest, AndroidStandbyBucketMapping) {
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(0),
               AndroidJobSchedulerTracker::StandbyBuckets::kActive);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(1),
               AndroidJobSchedulerTracker::StandbyBuckets::kWorkingSet);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(2),
               AndroidJobSchedulerTracker::StandbyBuckets::kFrequent);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(3),
               AndroidJobSchedulerTracker::StandbyBuckets::kRare);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(4),
               AndroidJobSchedulerTracker::StandbyBuckets::kNever);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(5),
               AndroidJobSchedulerTracker::StandbyBuckets::kRestricted);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(6),
               AndroidJobSchedulerTracker::StandbyBuckets::kExempted);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidStandbyBucketName(7),
               AndroidJobSchedulerTracker::StandbyBuckets::kUnknown);
}

TEST_F(AndroidJobSchedulerTrackerTest, AndroidInternalStopReasonMapping) {
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidInternalStopReasonName(-1),
               AndroidJobSchedulerTracker::InternalStopReasons::kUnknown);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidInternalStopReasonName(0),
               AndroidJobSchedulerTracker::InternalStopReasons::kCanceled);
  EXPECT_STREQ(
      AndroidJobSchedulerTracker::GetAndroidInternalStopReasonName(10),
      AndroidJobSchedulerTracker::InternalStopReasons::kSuccessfulFinish);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidInternalStopReasonName(14),
               AndroidJobSchedulerTracker::InternalStopReasons::
                   kDeviceStateBatterySaver);
}

TEST_F(AndroidJobSchedulerTrackerTest, AndroidPublicStopReasonMapping) {
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(0),
               AndroidJobSchedulerTracker::PublicStopReasons::kUndefined);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(1),
               AndroidJobSchedulerTracker::PublicStopReasons::kCancelledByApp);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(2),
               AndroidJobSchedulerTracker::PublicStopReasons::kPreempt);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(3),
               AndroidJobSchedulerTracker::PublicStopReasons::kTimeout);
  EXPECT_STREQ(
      AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(5),
      AndroidJobSchedulerTracker::PublicStopReasons::kConstraintBatteryNotLow);
  EXPECT_STREQ(
      AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(6),
      AndroidJobSchedulerTracker::PublicStopReasons::kConstraintCharging);
  EXPECT_STREQ(
      AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(16),
      AndroidJobSchedulerTracker::PublicStopReasons::kTimeoutAbandoned);
  EXPECT_STREQ(
      AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(17),
      AndroidJobSchedulerTracker::PublicStopReasons::kDeviceStateThermal);
  EXPECT_STREQ(
      AndroidJobSchedulerTracker::GetAndroidPublicStopReasonName(18),
      AndroidJobSchedulerTracker::PublicStopReasons::kDeviceStateBatterySaver);
}

TEST_F(AndroidJobSchedulerTrackerTest, AndroidBackoffPolicyMapping) {
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidBackoffPolicyName(0),
               AndroidJobSchedulerTracker::BackoffPolicies::kUnknown);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidBackoffPolicyName(1),
               AndroidJobSchedulerTracker::BackoffPolicies::kLinear);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidBackoffPolicyName(2),
               AndroidJobSchedulerTracker::BackoffPolicies::kExponential);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidBackoffPolicyName(3),
               AndroidJobSchedulerTracker::BackoffPolicies::kUnknown);
}

TEST_F(AndroidJobSchedulerTrackerTest, AndroidProcStateMapping) {
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidProcStateName(0),
               AndroidJobSchedulerTracker::ProcStates::kUnspecified);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidProcStateName(1002),
               AndroidJobSchedulerTracker::ProcStates::kTop);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidProcStateName(1020),
               AndroidJobSchedulerTracker::ProcStates::kBoundTop);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidProcStateName(1021),
               AndroidJobSchedulerTracker::ProcStates::kUnknown);
}

TEST_F(AndroidJobSchedulerTrackerTest, AndroidPriorityMapping) {
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidPriorityName(100),
               AndroidJobSchedulerTracker::Priorities::kMin);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidPriorityName(300),
               AndroidJobSchedulerTracker::Priorities::kDefault);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidPriorityName(500),
               AndroidJobSchedulerTracker::Priorities::kMax);
  EXPECT_STREQ(AndroidJobSchedulerTracker::GetAndroidPriorityName(501),
               AndroidJobSchedulerTracker::Priorities::kUnknown);
}

TEST_F(AndroidJobSchedulerTrackerTest, ParseAndroidJobSchedulerJob) {
  protozero::HeapBuffered<protozero::Message> job;
  job->AppendVarInt(AndroidJobSchedulerTracker::kJobId, 123);
  job->AppendVarInt(AndroidJobSchedulerTracker::kSourceUid, 1000);
  job->AppendVarInt(AndroidJobSchedulerTracker::kState, 1);  // state: STARTED
  job->AppendVarInt(AndroidJobSchedulerTracker::kStandbyBucket,
                    1);  // standby_bucket: WORKING_SET
  job->AppendVarInt(
      AndroidJobSchedulerTracker::kJobStateFlags,
      (1ULL << AndroidJobSchedulerTracker::kHasChargingConstraint) |
          (1ULL << AndroidJobSchedulerTracker::kHasBatteryNotLowConstraint));

  std::vector<uint8_t> blob = job.SerializeAsArray();
  parser->android_job_scheduler_tracker()->ParseAndroidJobSchedulerJob(
      1000, SliceId{0}, kNullStringId,
      protozero::ConstBytes{blob.data(), blob.size()});

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);
  auto rr = table[tables::AndroidJobSchedulerTrackEventTable::Id{0}];
  EXPECT_EQ(rr.ts(), 1000);
  EXPECT_EQ(rr.job_id(), 123);
  EXPECT_EQ(rr.uid(), 1000);
  EXPECT_STREQ(context.storage->GetString(rr.state()).c_str(),
               AndroidJobSchedulerTracker::JobStates::kStarted);
  EXPECT_STREQ(context.storage->GetString(rr.standby_bucket()).c_str(),
               AndroidJobSchedulerTracker::StandbyBuckets::kWorkingSet);
  EXPECT_EQ(rr.has_charging_constraint(), 1u);
  EXPECT_EQ(rr.has_battery_not_low_constraint(), 1u);
  EXPECT_EQ(rr.has_storage_not_low_constraint(), 0u);
}

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_FlagsUnpacking) {
  protozero::HeapBuffered<protozero::Message> job;
  job->AppendVarInt(AndroidJobSchedulerTracker::kJobId, 456);
  job->AppendVarInt(AndroidJobSchedulerTracker::kSourceUid, 2000);

  // Set all 16 mapped bits in job_state_flags:
  uint64_t flags =
      (1ULL << AndroidJobSchedulerTracker::kHasChargingConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kHasBatteryNotLowConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kHasStorageNotLowConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kHasTimingDelayConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kHasDeadlineConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kHasIdleConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kHasConnectivityConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kHasContentTriggerConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kIsRequestedExpeditedJob) |
      (1ULL << AndroidJobSchedulerTracker::kIsRunningAsExpeditedJob) |
      (1ULL << AndroidJobSchedulerTracker::kIsPrefetch) |
      (1ULL << AndroidJobSchedulerTracker::kIsRequestedAsUserInitiatedJob) |
      (1ULL << AndroidJobSchedulerTracker::kIsRunningAsUserInitiatedJob) |
      (1ULL << AndroidJobSchedulerTracker::kIsPeriodic) |
      (1ULL << AndroidJobSchedulerTracker::kHasFlexibilityConstraint) |
      (1ULL << AndroidJobSchedulerTracker::kCanApplyTransportAffinities);
  job->AppendVarInt(AndroidJobSchedulerTracker::kJobStateFlags, flags);

  std::vector<uint8_t> blob = job.SerializeAsArray();
  parser->android_job_scheduler_tracker()->ParseAndroidJobSchedulerJob(
      1000, SliceId{0}, kNullStringId,
      protozero::ConstBytes{blob.data(), blob.size()});

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);
  auto rr = table[tables::AndroidJobSchedulerTrackEventTable::Id{0}];

  // Verify all 16 boolean constraint columns are set to 1 (true):
  EXPECT_EQ(rr.has_charging_constraint(), 1u);
  EXPECT_EQ(rr.has_battery_not_low_constraint(), 1u);
  EXPECT_EQ(rr.has_storage_not_low_constraint(), 1u);
  EXPECT_EQ(rr.has_timing_delay_constraint(), 1u);
  EXPECT_EQ(rr.has_deadline_constraint(), 1u);
  EXPECT_EQ(rr.has_idle_constraint(), 1u);
  EXPECT_EQ(rr.has_connectivity_constraint(), 1u);
  EXPECT_EQ(rr.has_content_trigger_constraint(), 1u);
  EXPECT_EQ(rr.is_requested_expedited_job(), 1u);
  EXPECT_EQ(rr.is_running_as_expedited_job(), 1u);
  EXPECT_EQ(rr.is_prefetch(), 1u);
  EXPECT_EQ(rr.is_requested_as_user_initiated_job(), 1u);
  EXPECT_EQ(rr.is_running_as_user_initiated_job(), 1u);
  EXPECT_EQ(rr.is_periodic(), 1u);
  EXPECT_EQ(rr.has_flexibility_constraint(), 1u);
  EXPECT_EQ(rr.can_apply_transport_affinities(), 1u);
}

TEST_F(AndroidJobSchedulerTrackerTest, ParseAndroidJobSchedulerJob_Defaults) {
  protozero::HeapBuffered<protozero::Message> job;
  job->AppendVarInt(AndroidJobSchedulerTracker::kJobId, 789);
  job->AppendVarInt(AndroidJobSchedulerTracker::kSourceUid, 3000);
  // All other optional fields are omitted!

  std::vector<uint8_t> blob = job.SerializeAsArray();
  parser->android_job_scheduler_tracker()->ParseAndroidJobSchedulerJob(
      1000, SliceId{0}, kNullStringId,
      protozero::ConstBytes{blob.data(), blob.size()});

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);
  auto rr = table[tables::AndroidJobSchedulerTrackEventTable::Id{0}];

  // Verify default values are correctly populated:
  EXPECT_STREQ(context.storage->GetString(rr.state()).c_str(),
               AndroidJobSchedulerTracker::JobStates::kUnknown);
  EXPECT_STREQ(context.storage->GetString(rr.standby_bucket()).c_str(),
               AndroidJobSchedulerTracker::StandbyBuckets::kUnknown);
  EXPECT_STREQ(context.storage->GetString(rr.requested_priority()).c_str(),
               AndroidJobSchedulerTracker::Priorities::kUnknown);
  EXPECT_STREQ(context.storage->GetString(rr.effective_priority()).c_str(),
               AndroidJobSchedulerTracker::Priorities::kUnknown);
  EXPECT_STREQ(context.storage->GetString(rr.proc_state()).c_str(),
               AndroidJobSchedulerTracker::ProcStates::kUnknown);
  EXPECT_STREQ(context.storage->GetString(rr.internal_stop_reason()).c_str(),
               AndroidJobSchedulerTracker::InternalStopReasons::kUnknown);
  EXPECT_STREQ(context.storage->GetString(rr.public_stop_reason()).c_str(),
               AndroidJobSchedulerTracker::PublicStopReasons::kUndefined);
  EXPECT_STREQ(context.storage->GetString(rr.back_off_policy_type()).c_str(),
               AndroidJobSchedulerTracker::BackoffPolicies::kUnknown);

  // Verify optional integers are populated as std::nullopt (which translates to
  // NULL in DB):
  EXPECT_FALSE(rr.proxy_uid().has_value());
  EXPECT_FALSE(rr.num_previous_attempts().has_value());
  EXPECT_FALSE(rr.deadline_ms().has_value());
  EXPECT_FALSE(rr.delay_ms().has_value());
  EXPECT_FALSE(rr.job_start_latency_ms().has_value());
  EXPECT_FALSE(rr.num_uncompleted_work_items().has_value());
  EXPECT_FALSE(rr.periodic_job_interval_ms().has_value());
  EXPECT_FALSE(rr.periodic_job_flex_interval_ms().has_value());
  EXPECT_FALSE(rr.num_reschedules_due_to_abandonment().has_value());
}

}  // namespace
}  // namespace perfetto::trace_processor
