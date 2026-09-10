/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/perf/perf_invocation.h"

#include <cstdint>
#include <cstring>

#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/perf/features.h"
#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/importers/perf/perf_tracker.h"
#include "src/trace_processor/importers/perf/record.h"
#include "src/trace_processor/importers/perf/record_parser.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::perf_importer {
namespace {

using ::testing::Eq;
using ::testing::NotNull;

MATCHER(IsOk, "is ok") {
  return arg.ok();
}

MATCHER_P(IsOkAndHolds, matcher, "") {
  return ExplainMatchResult(IsOk(), arg, result_listener) &&
         ExplainMatchResult(matcher, *arg, result_listener);
}

TEST(PerfInvocationTest, NoAttrBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfInvocationTest, OneAttrAndNoIdBuildSucceeds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = false;
  attr.sample_type = PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME;
  builder.AddAttrAndIds(attr, {1});

  auto session = builder.Build();
  ASSERT_TRUE(session.ok());

  EXPECT_THAT(
      (*session)->FindAttrForRecord(perf_event_header{}, TraceBlobView()),
      IsOkAndHolds(NotNull()));
}

TEST(PerfInvocationTest, MultipleAttrsAndNoIdBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_CALLCHAIN | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME;
  builder.AddAttrAndIds(attr, {1});
  builder.AddAttrAndIds(attr, {2});
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfInvocationTest, MultipleIdsSameAttrAndNoIdCanExtractAttrFromRecord) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CPU | PERF_SAMPLE_TIME;
  builder.AddAttrAndIds(attr, {1, 2, 3});

  auto session = builder.Build();
  ASSERT_TRUE(session.ok());

  perf_event_header header;
  header.type = PERF_RECORD_SAMPLE;
  auto attr_ptr = (*session)->FindAttrForRecord(header, TraceBlobView());

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->sample_type(), Eq(attr.sample_type));

  header.type = PERF_RECORD_MMAP2;
  attr_ptr = (*session)->FindAttrForRecord(header, TraceBlobView());

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->sample_type(), Eq(attr.sample_type));
}

TEST(PerfInvocationTest, NoCommonSampleIdAllBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IDENTIFIER;
  builder.AddAttrAndIds(attr, {1});
  builder.AddAttrAndIds(attr, {2});
  // Make sure sample_type is correct (i.e. the test is really testing the
  // sample_id_all).
  ASSERT_TRUE(builder.Build().ok());

  attr.sample_id_all = false;
  builder.AddAttrAndIds(attr, {3});
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfInvocationTest, NoCommonOffsetForSampleBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ID;
  builder.AddAttrAndIds(attr, {1});
  attr.sample_type |= PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {2});
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfInvocationTest, NoCommonOffsetForNonSampleBuildFails) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_ID | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  builder.AddAttrAndIds(attr, {2});
  // Make sure sample_type is correct (i.e. the test is really testing the
  // non common sample_type).
  ASSERT_TRUE(builder.Build().ok());

  attr.sample_type |= PERF_SAMPLE_IDENTIFIER;
  builder.AddAttrAndIds(attr, {3});
  EXPECT_FALSE(builder.Build().ok());
}

TEST(PerfInvocationTest,
     NoCommonOffsetForNonSampleAndNoSampleIdAllBuildSucceeds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = false;
  attr.sample_type = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  attr.sample_type |= PERF_SAMPLE_ID;
  builder.AddAttrAndIds(attr, {2});
  EXPECT_TRUE(builder.Build().ok());
}

TEST(PerfInvocationTest, MultiplesessionBuildSucceeds) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ID;
  builder.AddAttrAndIds(attr, {1});
  builder.AddAttrAndIds(attr, {2});
  EXPECT_TRUE(builder.Build().ok());
}

TEST(PerfInvocationTest, FindAttrInRecordWithId) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_ID;
  attr.read_format = 1;
  builder.AddAttrAndIds(attr, {1});
  attr.read_format = 2;
  builder.AddAttrAndIds(attr, {2});

  auto session = builder.Build();
  ASSERT_TRUE(session.ok());

  struct {
    uint64_t ip = 1234;
    uint64_t id = 2;
  } data;

  perf_event_header header;
  header.type = PERF_RECORD_SAMPLE;
  auto attr_ptr = (*session)->FindAttrForRecord(
      header, TraceBlobView(TraceBlob ::CopyFrom(&data, sizeof(data))));

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->read_format(), Eq(2u));

  header.type = PERF_RECORD_MMAP2;
  data.id = 1;
  attr_ptr = (*session)->FindAttrForRecord(
      header, TraceBlobView(TraceBlob::CopyFrom(&data, sizeof(data))));

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->read_format(), Eq(1u));
}

TEST(PerfInvocationTest, FindAttrInRecordWithIdentifier) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.global_stats_tracker =
      std::make_unique<GlobalStatsTracker>(context.storage.get());
  context.machine_tracker.reset(
      new MachineTracker(&context, kDefaultMachineId));
  context.trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{0}});
  context.stats_tracker = std::make_unique<StatsTracker>(&context);
  PerfInvocation::Builder builder(&context);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.sample_type = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_IP;
  attr.read_format = 1;
  builder.AddAttrAndIds(attr, {1});
  attr.read_format = 2;
  builder.AddAttrAndIds(attr, {2});

  auto session = builder.Build();
  ASSERT_TRUE(session.ok());

  struct {
    uint64_t identifier = 2;
    uint64_t ip = 1234;
  } sample;

  struct {
    uint64_t ip = 1234;
    uint64_t identifier = 1;
  } mmap;

  perf_event_header header;
  header.type = PERF_RECORD_SAMPLE;
  auto attr_ptr = (*session)->FindAttrForRecord(
      header, TraceBlobView(TraceBlob ::CopyFrom(&sample, sizeof(sample))));

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->read_format(), Eq(2u));

  header.type = PERF_RECORD_MMAP2;
  attr_ptr = (*session)->FindAttrForRecord(
      header, TraceBlobView(TraceBlob::CopyFrom(&mmap, sizeof(mmap))));

  ASSERT_THAT(attr_ptr, IsOkAndHolds(NotNull()));
  EXPECT_THAT((*attr_ptr)->read_format(), Eq(1u));
}

class PerfThreadScopedCounterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.cpu_tracker = std::make_unique<CpuTracker>(&context_);
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.process_tracker = std::make_unique<ProcessTracker>(&context_);
  }

  TraceProcessorContext context_;
};

TEST_F(PerfThreadScopedCounterTest, ThreadScopedCounterCreation) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  UniqueTid utid1 = context_.process_tracker->UpdateThread(101, 100);
  UniqueTid utid2 = context_.process_tracker->UpdateThread(102, 100);

  PerfCounter& counter1 = attr_ptr->GetOrCreateCounter(0, utid1);
  counter1.AddDelta(1000, 50.0);
  PerfCounter& counter2 = attr_ptr->GetOrCreateCounter(0, utid2);
  counter2.AddDelta(1005, 30.0);

  const auto& track_table = context_.storage->track_table();
  EXPECT_EQ(track_table.row_count(), 2u);
  const auto& counter_table = context_.storage->counter_table();
  EXPECT_EQ(counter_table.row_count(), 2u);
  EXPECT_NE(counter1.track_id(), counter2.track_id());
  EXPECT_EQ(counter_table[0].value(), 50.0);
  EXPECT_EQ(counter_table[1].value(), 30.0);
}

TEST_F(PerfThreadScopedCounterTest, ThreadScopedCounterAddCountMonotonicity) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  UniqueTid utid1 = context_.process_tracker->UpdateThread(101, 100);
  UniqueTid utid2 = context_.process_tracker->UpdateThread(102, 100);
  PerfCounter& counter1 = attr_ptr->GetOrCreateCounter(0, utid1);
  PerfCounter& counter2 = attr_ptr->GetOrCreateCounter(0, utid2);

  counter1.AddCount(1000, 1000.0);
  counter2.AddCount(1005, 50.0);

  const auto& counter_table = context_.storage->counter_table();
  EXPECT_EQ(counter_table.row_count(), 2u);
  EXPECT_EQ(counter_table[0].value(), 1000.0);
  EXPECT_EQ(counter_table[1].value(), 50.0);
}

TEST_F(PerfThreadScopedCounterTest,
       NonMonotonicCounterGracefulClampingAndStat) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  UniqueTid utid = context_.process_tracker->UpdateThread(101, 100);
  PerfCounter& counter = attr_ptr->GetOrCreateCounter(0, utid);
  counter.AddCount(1000, 1000.0);

  counter.AddCount(1005, 50.0);

  EXPECT_EQ(context_.stats_tracker->GetStats(stats::perf_counter_non_monotonic),
            1);
  const auto& counter_table = context_.storage->counter_table();
  EXPECT_EQ(counter_table.row_count(), 2u);
  EXPECT_EQ(counter_table[0].value(), 1000.0);
  EXPECT_EQ(counter_table[1].value(), 1000.0);
}

TEST_F(PerfThreadScopedCounterTest, ThreadScopedCounterAddCountCoreMigration) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CPU;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  UniqueTid utid = context_.process_tracker->UpdateThread(101, 100);
  PerfCounter& counter = attr_ptr->GetOrCreateCounter(0, utid);

  // Core 0 emits 1000 instructions.
  counter.AddCount(1000, 1000.0, 0);
  // Thread migrates to Core 1; Core 1 counter records 500 instructions.
  counter.AddCount(1010, 500.0, 1);
  // Thread migrates back to Core 0; Core 0 counter reaches 1200 (delta +200).
  counter.AddCount(1020, 1200.0, 0);
  // Thread migrates back to Core 1; Core 1 counter reaches 700 (delta +200).
  counter.AddCount(1030, 700.0, 1);

  const auto& counter_table = context_.storage->counter_table();
  EXPECT_EQ(counter_table.row_count(), 4u);
  EXPECT_EQ(counter_table[0].value(), 1000.0);
  EXPECT_EQ(counter_table[1].value(), 1500.0);
  EXPECT_EQ(counter_table[2].value(), 1700.0);
  EXPECT_EQ(counter_table[3].value(), 1900.0);
  EXPECT_EQ(context_.stats_tracker->GetStats(stats::perf_counter_non_monotonic),
            0);
}

TEST_F(PerfThreadScopedCounterTest,
       ThreadScopedCounterPerCpuNonMonotonicClamping) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = true;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CPU;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  UniqueTid utid = context_.process_tracker->UpdateThread(101, 100);
  PerfCounter& counter = attr_ptr->GetOrCreateCounter(0, utid);

  counter.AddCount(1000, 1000.0, 0);
  counter.AddCount(1010, 500.0, 1);
  // Core 0 counter unexpectedly regresses from 1000 to 800.
  counter.AddCount(1020, 800.0, 0);

  EXPECT_EQ(context_.stats_tracker->GetStats(stats::perf_counter_non_monotonic),
            1);
  const auto& counter_table = context_.storage->counter_table();
  EXPECT_EQ(counter_table.row_count(), 3u);
  EXPECT_EQ(counter_table[0].value(), 1000.0);
  EXPECT_EQ(counter_table[1].value(), 1500.0);
  // Regressed sample is clamped to 1000.0 (delta = 0), total remains 1500.0.
  EXPECT_EQ(counter_table[2].value(), 1500.0);
}

TEST_F(PerfThreadScopedCounterTest,
       SystemWideTraceRoutesToCpuTrackEvenWithUtid) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = false;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  (*session)->SetCmdline({"/usr/bin/perf", "record", "-a"});
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  EXPECT_FALSE(attr_ptr->is_thread_scoped());
  UniqueTid utid1 = context_.process_tracker->UpdateThread(101, 100);
  UniqueTid utid2 = context_.process_tracker->UpdateThread(102, 100);

  PerfCounter& counter1 = attr_ptr->GetOrCreateCounter(0, utid1);
  PerfCounter& counter2 = attr_ptr->GetOrCreateCounter(0, utid2);

  EXPECT_EQ(counter1.track_id(), counter2.track_id());
  const auto& track_table = context_.storage->track_table();
  EXPECT_EQ(track_table.row_count(), 1u);
}

TEST_F(PerfThreadScopedCounterTest,
       SystemWideTraceWithoutCmdlineRoutesToCpuTrack) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = false;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  EXPECT_FALSE(attr_ptr->is_thread_scoped());
  UniqueTid utid1 = context_.process_tracker->UpdateThread(101, 100);
  UniqueTid utid2 = context_.process_tracker->UpdateThread(102, 100);

  PerfCounter& counter1 = attr_ptr->GetOrCreateCounter(0, utid1);
  PerfCounter& counter2 = attr_ptr->GetOrCreateCounter(0, utid2);

  EXPECT_EQ(counter1.track_id(), counter2.track_id());
  const auto& track_table = context_.storage->track_table();
  EXPECT_EQ(track_table.row_count(), 1u);
}

TEST_F(PerfThreadScopedCounterTest, ThreadScopedTraceDetectedViaCmdlinePid) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = false;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  (*session)->SetCmdline({"/usr/bin/perf", "record", "-p", "100"});
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  EXPECT_TRUE(attr_ptr->is_thread_scoped());
  UniqueTid utid1 = context_.process_tracker->UpdateThread(101, 100);
  UniqueTid utid2 = context_.process_tracker->UpdateThread(102, 100);

  PerfCounter& counter1 = attr_ptr->GetOrCreateCounter(0, utid1);
  PerfCounter& counter2 = attr_ptr->GetOrCreateCounter(0, utid2);

  EXPECT_NE(counter1.track_id(), counter2.track_id());
  const auto& track_table = context_.storage->track_table();
  EXPECT_EQ(track_table.row_count(), 2u);
}

TEST_F(PerfThreadScopedCounterTest,
       ThreadScopedTraceDetectedViaSimpleperfMetaInfoApp) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = false;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  feature::SimpleperfMetaInfo meta_info;
  meta_info.entries.Insert("app_package_name", "com.android.chrome");
  (*session)->SetSimpleperfMetaInfo(meta_info.entries);
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  EXPECT_TRUE(attr_ptr->is_thread_scoped());
  UniqueTid utid1 = context_.process_tracker->UpdateThread(101, 100);
  UniqueTid utid2 = context_.process_tracker->UpdateThread(102, 100);

  PerfCounter& counter1 = attr_ptr->GetOrCreateCounter(0, utid1);
  PerfCounter& counter2 = attr_ptr->GetOrCreateCounter(0, utid2);

  EXPECT_NE(counter1.track_id(), counter2.track_id());
  const auto& track_table = context_.storage->track_table();
  EXPECT_EQ(track_table.row_count(), 2u);
}

TEST_F(PerfThreadScopedCounterTest,
       SystemWideTraceDetectedViaSimpleperfMetaInfoSystemWide) {
  PerfInvocation::Builder builder(&context_);
  perf_event_attr attr{};
  attr.sample_id_all = true;
  attr.inherit = false;
  attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  builder.AddAttrAndIds(attr, {1});
  auto session = builder.Build();
  ASSERT_TRUE(session.ok());
  feature::SimpleperfMetaInfo meta_info;
  meta_info.entries.Insert("system_wide_collection", "true");
  (*session)->SetSimpleperfMetaInfo(meta_info.entries);
  RefPtr<PerfEventAttr> attr_ptr = (*session)->FindAttrForEventId(1);
  ASSERT_TRUE(attr_ptr);
  attr_ptr->set_event_name("instructions");
  EXPECT_FALSE(attr_ptr->is_thread_scoped());
  UniqueTid utid1 = context_.process_tracker->UpdateThread(101, 100);
  UniqueTid utid2 = context_.process_tracker->UpdateThread(102, 100);

  PerfCounter& counter1 = attr_ptr->GetOrCreateCounter(0, utid1);
  PerfCounter& counter2 = attr_ptr->GetOrCreateCounter(0, utid2);

  EXPECT_EQ(counter1.track_id(), counter2.track_id());
  const auto& track_table = context_.storage->track_table();
  EXPECT_EQ(track_table.row_count(), 1u);
}

}  // namespace
}  // namespace perfetto::trace_processor::perf_importer
