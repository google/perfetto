/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/perf_sample_tracker.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/perf_events.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "protos/perfetto/trace/trace_packet_defaults.gen.h"
#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"

namespace perfetto {
namespace trace_processor {
namespace {

class PerfSampleTrackerTest : public ::testing::Test {
 public:
  PerfSampleTrackerTest() {
    context.storage.reset(new TraceStorage());
    context.track_tracker.reset(new TrackTracker(&context));
    context.perf_sample_tracker.reset(new PerfSampleTracker(&context));
  }

 protected:
  TraceProcessorContext context;
};

TEST_F(PerfSampleTrackerTest, PerCpuCounterTracks) {
  uint32_t seq_id = 42;
  uint32_t cpu0 = 0;
  uint32_t cpu1 = 1;

  auto stream = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu0, /*nullable_defaults=*/nullptr);
  auto stream2 = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu1, /*nullable_defaults=*/nullptr);

  // same session, different counter tracks
  EXPECT_EQ(stream.perf_session_id, stream2.perf_session_id);
  EXPECT_NE(stream.timebase_track_id, stream2.timebase_track_id);

  // re-querying one of the existing streams gives the same ids
  auto stream3 = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu1, /*nullable_defaults=*/nullptr);

  EXPECT_EQ(stream2.perf_session_id, stream3.perf_session_id);
  EXPECT_EQ(stream2.timebase_track_id, stream3.timebase_track_id);
}

TEST_F(PerfSampleTrackerTest, TimebaseTrackName_Counter) {
  uint32_t seq_id = 42;
  uint32_t cpu0 = 0;

  protos::gen::TracePacketDefaults defaults;
  auto* perf_defaults = defaults.mutable_perf_sample_defaults();
  perf_defaults->mutable_timebase()->set_frequency(100);
  perf_defaults->mutable_timebase()->set_counter(
      protos::gen::PerfEvents::SW_PAGE_FAULTS);
  auto defaults_pb = defaults.SerializeAsString();
  protos::pbzero::TracePacketDefaults::Decoder defaults_decoder(defaults_pb);

  auto stream = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu0, &defaults_decoder);

  TrackId track_id = stream.timebase_track_id;
  const auto& track_table = context.storage->perf_counter_track_table();
  auto row_id = track_table.id().IndexOf(track_id);

  // track exists and looks sensible
  ASSERT_TRUE(row_id.has_value());
  EXPECT_EQ(track_table.perf_session_id()[*row_id], stream.perf_session_id);
  EXPECT_EQ(track_table.cpu()[*row_id], cpu0);
  EXPECT_TRUE(track_table.is_timebase()[*row_id]);

  // Name derived from the timebase.
  std::string track_name =
      context.storage->GetString(track_table.name()[*row_id]).ToStdString();
  ASSERT_EQ(track_name, "page-faults");
}

TEST_F(PerfSampleTrackerTest, TimebaseTrackName_Tracepoint) {
  uint32_t seq_id = 42;
  uint32_t cpu0 = 0;

  protos::gen::TracePacketDefaults defaults;
  auto* perf_defaults = defaults.mutable_perf_sample_defaults();
  perf_defaults->mutable_timebase()->set_frequency(100);
  perf_defaults->mutable_timebase()->mutable_tracepoint()->set_name(
      "sched:sched_switch");
  auto defaults_pb = defaults.SerializeAsString();
  protos::pbzero::TracePacketDefaults::Decoder defaults_decoder(defaults_pb);

  auto stream = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu0, &defaults_decoder);

  TrackId track_id = stream.timebase_track_id;
  const auto& track_table = context.storage->perf_counter_track_table();
  auto row_id = track_table.id().IndexOf(track_id);

  // track exists and looks sensible
  ASSERT_TRUE(row_id.has_value());
  EXPECT_EQ(track_table.perf_session_id()[*row_id], stream.perf_session_id);
  EXPECT_EQ(track_table.cpu()[*row_id], cpu0);
  EXPECT_TRUE(track_table.is_timebase()[*row_id]);

  // Name derived from the timebase.
  std::string track_name =
      context.storage->GetString(track_table.name()[*row_id]).ToStdString();
  ASSERT_EQ(track_name, "sched:sched_switch");
}

TEST_F(PerfSampleTrackerTest, UnknownCounterTreatedAsCpuClock) {
  uint32_t seq_id = 42;
  uint32_t cpu0 = 0;

  auto stream = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu0, /*nullable_defaults=*/nullptr);

  TrackId track_id = stream.timebase_track_id;
  const auto& track_table = context.storage->perf_counter_track_table();
  auto row_id = track_table.id().IndexOf(track_id);

  // track exists and looks sensible
  ASSERT_TRUE(row_id.has_value());
  EXPECT_EQ(track_table.perf_session_id()[*row_id], stream.perf_session_id);
  EXPECT_EQ(track_table.cpu()[*row_id], cpu0);
  EXPECT_TRUE(track_table.is_timebase()[*row_id]);

  // If the trace doesn't have a PerfSampleDefaults describing the timebase
  // counter, we assume cpu-clock.
  std::string track_name =
      context.storage->GetString(track_table.name()[*row_id]).ToStdString();
  ASSERT_EQ(track_name, "cpu-clock");
}

// Like TimebaseTrackName_Counter, but with a config supplying an explicit name
// for the counter.
TEST_F(PerfSampleTrackerTest, TimebaseTrackName_ConfigSuppliedName) {
  uint32_t seq_id = 42;
  uint32_t cpu0 = 0;

  protos::gen::TracePacketDefaults defaults;
  auto* perf_defaults = defaults.mutable_perf_sample_defaults();
  perf_defaults->mutable_timebase()->set_name("test-name");
  perf_defaults->mutable_timebase()->set_frequency(100);
  perf_defaults->mutable_timebase()->set_counter(
      protos::gen::PerfEvents::SW_PAGE_FAULTS);
  auto defaults_pb = defaults.SerializeAsString();
  protos::pbzero::TracePacketDefaults::Decoder defaults_decoder(defaults_pb);

  auto stream = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu0, &defaults_decoder);

  TrackId track_id = stream.timebase_track_id;
  const auto& track_table = context.storage->perf_counter_track_table();
  auto row_id = track_table.id().IndexOf(track_id);

  // track exists and looks sensible
  ASSERT_TRUE(row_id.has_value());
  EXPECT_EQ(track_table.perf_session_id()[*row_id], stream.perf_session_id);
  EXPECT_EQ(track_table.cpu()[*row_id], cpu0);
  EXPECT_TRUE(track_table.is_timebase()[*row_id]);

  // Using the config-supplied name for the track.
  std::string track_name =
      context.storage->GetString(track_table.name()[*row_id]).ToStdString();
  ASSERT_EQ(track_name, "test-name");
}

TEST_F(PerfSampleTrackerTest, ProcessShardingStatsEntries) {
  uint32_t cpu0 = 0;
  uint32_t cpu1 = 1;

  protos::gen::TracePacketDefaults defaults;
  auto* perf_defaults = defaults.mutable_perf_sample_defaults();
  perf_defaults->mutable_timebase()->set_frequency(100);
  perf_defaults->mutable_timebase()->set_counter(
      protos::gen::PerfEvents::SW_PAGE_FAULTS);
  // shard 7/8
  perf_defaults->set_process_shard_count(8u);
  perf_defaults->set_chosen_process_shard(7u);
  auto defaults_pb = defaults.SerializeAsString();
  protos::pbzero::TracePacketDefaults::Decoder defaults_decoder(defaults_pb);

  // Two per-cpu lookups for first sequence
  auto stream = context.perf_sample_tracker->GetSamplingStreamInfo(
      /*seq_id=*/42, cpu0, &defaults_decoder);
  context.perf_sample_tracker->GetSamplingStreamInfo(
      /*seq_id=*/42, cpu1, &defaults_decoder);

  // Second sequence
  auto stream2 = context.perf_sample_tracker->GetSamplingStreamInfo(
      /*seq_id=*/100, cpu0, &defaults_decoder);
  context.perf_sample_tracker->GetSamplingStreamInfo(
      /*seq_id=*/100, cpu1, &defaults_decoder);

  EXPECT_NE(stream.perf_session_id, stream2.perf_session_id);

  std::optional<int64_t> shard_count = context.storage->GetIndexedStats(
      stats::perf_process_shard_count,
      static_cast<int>(stream.perf_session_id));
  std::optional<int64_t> chosen_shard = context.storage->GetIndexedStats(
      stats::perf_chosen_process_shard,
      static_cast<int>(stream.perf_session_id));

  ASSERT_TRUE(shard_count.has_value());
  EXPECT_EQ(shard_count.value(), 8);
  ASSERT_TRUE(chosen_shard.has_value());
  EXPECT_EQ(chosen_shard.value(), 7);

  std::optional<int64_t> shard_count2 = context.storage->GetIndexedStats(
      stats::perf_process_shard_count,
      static_cast<int>(stream.perf_session_id));
  std::optional<int64_t> chosen_shard2 = context.storage->GetIndexedStats(
      stats::perf_chosen_process_shard,
      static_cast<int>(stream.perf_session_id));

  ASSERT_TRUE(shard_count2.has_value());
  EXPECT_EQ(shard_count2.value(), 8);
  ASSERT_TRUE(chosen_shard2.has_value());
  EXPECT_EQ(chosen_shard2.value(), 7);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
