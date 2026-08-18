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
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"
#include "src/trace_processor/importers/proto/android_extension.descriptor.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/util/descriptors.h"

#include <memory>
#include <vector>

#include "perfetto/protozero/proto_utils.h"
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
    context.descriptor_pool_.reset(new DescriptorPool());
    context.descriptor_pool_->AddFromFileDescriptorSet(
        kAndroidExtensionDescriptor.data(), kAndroidExtensionDescriptor.size());
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
    tracker.reset(
        new AndroidJobSchedulerTracker(&extension_parser_context, &context));
  }

  TraceProcessorContext context;
  TrackEventExtensionParserContext extension_parser_context;
  std::unique_ptr<AndroidJobSchedulerTracker> tracker;
};

TEST_F(AndroidJobSchedulerTrackerTest, ParseAndroidJobSchedulerJob) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 123);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 1000);
  job->AppendVarInt(ProtoJob::kStateFieldNumber, ProtoJob::JOB_STATE_STARTED);
  job->AppendVarInt(ProtoJob::kStandbyBucketFieldNumber,
                    ProtoJob::STANDBY_BUCKET_WORKING_SET);
  job->AppendVarInt(
      ProtoJob::kJobStateFlagsFieldNumber,
      ProtoJob::JOB_STATE_FLAG_HAS_CHARGING_CONSTRAINT |
          ProtoJob::JOB_STATE_FLAG_HAS_BATTERY_NOT_LOW_CONSTRAINT);
  job->AppendVarInt(ProtoJob::kInternalStopReasonFieldNumber,
                    ProtoJob::INTERNAL_STOP_REASON_CANCELLED);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 1000;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 1000;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);
  auto rr = table[tables::AndroidJobSchedulerTrackEventTable::Id{0}];
  EXPECT_EQ(rr.ts(), 1000);
  EXPECT_EQ(rr.job_id(), 123);
  EXPECT_EQ(rr.uid(), 1000);
  EXPECT_STREQ(context.storage->GetString(rr.state()).c_str(),
               ProtoJob::JobState_Name(ProtoJob::JOB_STATE_STARTED));
  EXPECT_STREQ(
      context.storage->GetString(rr.standby_bucket()).c_str(),
      ProtoJob::StandbyBucket_Name(ProtoJob::STANDBY_BUCKET_WORKING_SET));
  EXPECT_EQ(rr.has_charging_constraint(), 1u);
  EXPECT_EQ(rr.has_battery_not_low_constraint(), 1u);
  EXPECT_EQ(rr.has_storage_not_low_constraint(), 0u);
  EXPECT_STREQ(context.storage->GetString(rr.internal_stop_reason()).c_str(),
               ProtoJob::InternalStopReason_Name(
                   ProtoJob::INTERNAL_STOP_REASON_CANCELLED));
}

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_FlagsUnpacking) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 456);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 2000);

  // Set all 16 mapped bits in job_state_flags:
  uint64_t flags = ProtoJob::JOB_STATE_FLAG_HAS_CHARGING_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_HAS_BATTERY_NOT_LOW_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_HAS_STORAGE_NOT_LOW_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_HAS_TIMING_DELAY_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_HAS_DEADLINE_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_HAS_IDLE_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_HAS_CONNECTIVITY_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_HAS_CONTENT_TRIGGER_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_IS_REQUESTED_EXPEDITED_JOB |
                   ProtoJob::JOB_STATE_FLAG_IS_RUNNING_AS_EXPEDITED_JOB |
                   ProtoJob::JOB_STATE_FLAG_IS_PREFETCH |
                   ProtoJob::JOB_STATE_FLAG_IS_REQUESTED_USER_INITIATED_JOB |
                   ProtoJob::JOB_STATE_FLAG_IS_RUNNING_AS_USER_INITIATED_JOB |
                   ProtoJob::JOB_STATE_FLAG_IS_PERIODIC |
                   ProtoJob::JOB_STATE_FLAG_HAS_FLEXIBILITY_CONSTRAINT |
                   ProtoJob::JOB_STATE_FLAG_CAN_APPLY_TRANSPORT_AFFINITIES;
  job->AppendVarInt(ProtoJob::kJobStateFlagsFieldNumber, flags);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 1000;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 1000;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

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
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 789);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 3000);
  // All other optional fields are omitted!

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 1000;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 1000;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);
  auto rr = table[tables::AndroidJobSchedulerTrackEventTable::Id{0}];

  // Verify default values are correctly populated:
  using com::android::internal::pbzero::ProcessStateEnum;
  using com::android::internal::pbzero::ProcessStateEnum_Name;
  EXPECT_STREQ(context.storage->GetString(rr.state()).c_str(),
               ProtoJob::JobState_Name(ProtoJob::JOB_STATE_UNKNOWN));
  EXPECT_STREQ(context.storage->GetString(rr.standby_bucket()).c_str(),
               ProtoJob::StandbyBucket_Name(ProtoJob::STANDBY_BUCKET_UNKNOWN));
  EXPECT_STREQ(context.storage->GetString(rr.requested_priority()).c_str(),
               ProtoJob::JobPriority_Name(ProtoJob::JOB_PRIORITY_UNKNOWN));
  EXPECT_STREQ(context.storage->GetString(rr.effective_priority()).c_str(),
               ProtoJob::JobPriority_Name(ProtoJob::JOB_PRIORITY_UNKNOWN));
  EXPECT_STREQ(context.storage->GetString(rr.proc_state()).c_str(),
               ProcessStateEnum_Name(ProcessStateEnum::PROCESS_STATE_UNKNOWN));
  EXPECT_STREQ(context.storage->GetString(rr.internal_stop_reason()).c_str(),
               ProtoJob::InternalStopReason_Name(
                   ProtoJob::INTERNAL_STOP_REASON_UNKNOWN));
  EXPECT_STREQ(
      context.storage->GetString(rr.public_stop_reason()).c_str(),
      ProtoJob::PublicStopReason_Name(ProtoJob::STOP_REASON_UNDEFINED));
  EXPECT_STREQ(context.storage->GetString(rr.back_off_policy_type()).c_str(),
               ProtoJob::BackoffPolicy_Name(ProtoJob::BACKOFF_POLICY_UNKNOWN));

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

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_PendingReasons) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 101);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 1000);
  job->AppendVarInt(ProtoJob::kStateFieldNumber, ProtoJob::JOB_STATE_STARTED);

  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber,
                    ProtoJob::PENDING_JOB_REASON_CONSTRAINT_CHARGING);
  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber,
                    ProtoJob::PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE);

  job->AppendVarInt(ProtoJob::kPendingDurationsMsFieldNumber, 1500);
  job->AppendVarInt(ProtoJob::kPendingDurationsMsFieldNumber, 4200);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 5000;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 5000;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);

  const auto& pending_table =
      context.storage
          ->android_job_scheduler_pending_reasons_track_event_table();
  ASSERT_EQ(pending_table.row_count(), 2u);

  auto r0 = pending_table
      [tables::AndroidJobSchedulerPendingReasonsTrackEventTable::Id{0}];
  EXPECT_EQ(r0.slice_id(), slice_id);
  EXPECT_EQ(r0.reason_index(), 0);
  EXPECT_STREQ(context.storage->GetString(r0.pending_reason()).c_str(),
               ProtoJob::PendingJobReason_Name(
                   ProtoJob::PENDING_JOB_REASON_CONSTRAINT_CHARGING));
  EXPECT_EQ(r0.pending_duration_ms(), 1500);

  auto r1 = pending_table
      [tables::AndroidJobSchedulerPendingReasonsTrackEventTable::Id{1}];
  EXPECT_EQ(r1.slice_id(), slice_id);
  EXPECT_EQ(r1.reason_index(), 1);
  EXPECT_STREQ(context.storage->GetString(r1.pending_reason()).c_str(),
               ProtoJob::PendingJobReason_Name(
                   ProtoJob::PENDING_JOB_REASON_CONSTRAINT_DEVICE_IDLE));
  EXPECT_EQ(r1.pending_duration_ms(), 4200);
}

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_PendingReasonsCancelled) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 102);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 1000);
  job->AppendVarInt(ProtoJob::kStateFieldNumber, ProtoJob::JOB_STATE_CANCELLED);

  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber,
                    ProtoJob::PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY);
  job->AppendVarInt(ProtoJob::kPendingDurationsMsFieldNumber, 8000);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 5500;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 5500;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);

  const auto& pending_table =
      context.storage
          ->android_job_scheduler_pending_reasons_track_event_table();
  ASSERT_EQ(pending_table.row_count(), 1u);

  auto r0 = pending_table
      [tables::AndroidJobSchedulerPendingReasonsTrackEventTable::Id{0}];
  EXPECT_EQ(r0.slice_id(), slice_id);
  EXPECT_EQ(r0.reason_index(), 0);
  EXPECT_STREQ(context.storage->GetString(r0.pending_reason()).c_str(),
               ProtoJob::PendingJobReason_Name(
                   ProtoJob::PENDING_JOB_REASON_CONSTRAINT_CONNECTIVITY));
  EXPECT_EQ(r0.pending_duration_ms(), 8000);
}

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_PendingReasonsIgnoredOnFinished) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 103);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 1000);
  job->AppendVarInt(ProtoJob::kStateFieldNumber, ProtoJob::JOB_STATE_FINISHED);

  // Even if pending reasons were populated, they should be ignored on FINISHED.
  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber,
                    ProtoJob::PENDING_JOB_REASON_CONSTRAINT_CHARGING);
  job->AppendVarInt(ProtoJob::kPendingDurationsMsFieldNumber, 1500);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 5600;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 5600;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);

  const auto& pending_table =
      context.storage
          ->android_job_scheduler_pending_reasons_track_event_table();
  EXPECT_EQ(pending_table.row_count(), 0u);
}

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_PendingReasonsMismatchedLengths) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 202);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 1000);
  job->AppendVarInt(ProtoJob::kStateFieldNumber, ProtoJob::JOB_STATE_STARTED);

  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber,
                    ProtoJob::PENDING_JOB_REASON_CONSTRAINT_CHARGING);
  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber,
                    ProtoJob::PENDING_JOB_REASON_CONSTRAINT_BATTERY_NOT_LOW);

  // Only 1 duration provided for 2 reasons
  job->AppendVarInt(ProtoJob::kPendingDurationsMsFieldNumber, 1200);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 6000;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 6000;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);

  const auto& pending_table =
      context.storage
          ->android_job_scheduler_pending_reasons_track_event_table();
  ASSERT_EQ(pending_table.row_count(), 2u);

  auto r0 = pending_table
      [tables::AndroidJobSchedulerPendingReasonsTrackEventTable::Id{0}];
  EXPECT_EQ(r0.pending_duration_ms(), 1200);

  auto r1 = pending_table
      [tables::AndroidJobSchedulerPendingReasonsTrackEventTable::Id{1}];
  EXPECT_EQ(r1.pending_duration_ms(), 0);
}

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_PendingReasonsUnknownEnum) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 303);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 1000);
  job->AppendVarInt(ProtoJob::kStateFieldNumber, ProtoJob::JOB_STATE_STARTED);

  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber, 999);
  job->AppendVarInt(ProtoJob::kPendingDurationsMsFieldNumber, 500);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 7000;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 7000;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);

  const auto& pending_table =
      context.storage
          ->android_job_scheduler_pending_reasons_track_event_table();
  ASSERT_EQ(pending_table.row_count(), 1u);

  auto r0 = pending_table
      [tables::AndroidJobSchedulerPendingReasonsTrackEventTable::Id{0}];
  EXPECT_STREQ(context.storage->GetString(r0.pending_reason()).c_str(), "999");
  EXPECT_EQ(r0.pending_duration_ms(), 500);
}

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_NoPendingReasons) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 404);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 1000);
  job->AppendVarInt(ProtoJob::kStateFieldNumber, ProtoJob::JOB_STATE_STARTED);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 8000;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 8000;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& pending_table =
      context.storage
          ->android_job_scheduler_pending_reasons_track_event_table();
  EXPECT_EQ(pending_table.row_count(), 0u);
}

TEST_F(AndroidJobSchedulerTrackerTest,
       ParseAndroidJobSchedulerJob_PendingReasonsExecutingAndInvalidJobId) {
  protozero::HeapBuffered<protozero::Message> job;
  using ProtoJob = ::com::android::internal::pbzero::AndroidJobSchedulerJob;
  job->AppendVarInt(ProtoJob::kJobIdFieldNumber, 505);
  job->AppendVarInt(ProtoJob::kSourceUidFieldNumber, 1000);
  job->AppendVarInt(ProtoJob::kStateFieldNumber, ProtoJob::JOB_STATE_STARTED);

  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber,
                    ProtoJob::PENDING_JOB_REASON_EXECUTING);
  job->AppendVarInt(ProtoJob::kPendingReasonsFieldNumber,
                    ProtoJob::PENDING_JOB_REASON_INVALID_JOB_ID);

  job->AppendVarInt(ProtoJob::kPendingDurationsMsFieldNumber, 1200);
  job->AppendVarInt(ProtoJob::kPendingDurationsMsFieldNumber, 3400);

  std::vector<uint8_t> blob = job.SerializeAsArray();

  protozero::Field field;
  field.initialize(AndroidJobSchedulerTracker::kJobSchedulerJobExtensionFieldId,
                   static_cast<uint8_t>(
                       protozero::proto_utils::ProtoWireType::kLengthDelimited),
                   reinterpret_cast<uint64_t>(blob.data()),
                   static_cast<uint32_t>(blob.size()));
  TrackEventExtensionField extension_field(field);

  tables::SliceTable::Row slice_row;
  slice_row.ts = 9000;
  auto slice_id = context.storage->mutable_slice_table()->Insert(slice_row).id;

  TrackEventFieldContext event;
  event.ts = 9000;
  event.row_kind = TrackEventFieldContext::RowKind::kSlice;
  event.row_id = slice_id.value;
  tracker->OnTrackEventField(extension_field, event);

  const auto& table =
      context.storage->android_job_scheduler_track_event_table();
  ASSERT_EQ(table.row_count(), 1u);

  const auto& pending_table =
      context.storage
          ->android_job_scheduler_pending_reasons_track_event_table();
  ASSERT_EQ(pending_table.row_count(), 2u);

  auto r0 = pending_table
      [tables::AndroidJobSchedulerPendingReasonsTrackEventTable::Id{0}];
  EXPECT_STREQ(context.storage->GetString(r0.pending_reason()).c_str(),
               "PENDING_JOB_REASON_EXECUTING");
  EXPECT_EQ(r0.pending_duration_ms(), 1200);

  auto r1 = pending_table
      [tables::AndroidJobSchedulerPendingReasonsTrackEventTable::Id{1}];
  EXPECT_STREQ(context.storage->GetString(r1.pending_reason()).c_str(),
               "PENDING_JOB_REASON_INVALID_JOB_ID");
  EXPECT_EQ(r1.pending_duration_ms(), 3400);
}

}  // namespace
}  // namespace perfetto::trace_processor
