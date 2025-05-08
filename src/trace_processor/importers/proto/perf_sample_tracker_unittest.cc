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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/status_macros.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/perf_events.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace_packet_defaults.gen.h"
#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"

namespace perfetto::trace_processor {
namespace {

class PerfSampleTrackerTest : public ::testing::Test {
 public:
  PerfSampleTrackerTest() {
    context.storage = std::make_shared<TraceStorage>();
    context.machine_tracker = std::make_unique<MachineTracker>(&context, 0);
    context.cpu_tracker = std::make_unique<CpuTracker>(&context);
    context.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context.storage.get());
    context.args_tracker = std::make_unique<ArgsTracker>(&context);
    context.track_tracker = std::make_unique<TrackTracker>(&context);
    context.perf_sample_tracker = std::make_unique<PerfSampleTracker>(&context);
  }

  base::StatusOr<std::optional<Variadic>> GetDimension(
      const tables::TrackTable::ConstRowReference& ref,
      const char* name) const {
    std::optional<Variadic> var;
    RETURN_IF_ERROR(context.storage->ExtractArg(
        ref.dimension_arg_set_id().value(), name, &var));
    return var;
  }

  base::StatusOr<std::optional<Variadic>> GetArg(
      const tables::TrackTable::ConstRowReference& ref,
      const char* name) const {
    std::optional<Variadic> var;
    RETURN_IF_ERROR(context.storage->ExtractArg(ref.source_arg_set_id().value(),
                                                name, &var));
    return var;
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
  const auto& track_table = context.storage->track_table();
  auto rr = track_table.FindById(track_id);

  // track exists and looks sensible
  ASSERT_TRUE(rr.has_value());

  ASSERT_OK_AND_ASSIGN(auto perf_session_id,
                       GetDimension(*rr, "perf_session_id"));
  EXPECT_EQ(perf_session_id, Variadic::Integer(stream.perf_session_id.value));

  ASSERT_OK_AND_ASSIGN(auto cpu, GetDimension(*rr, "cpu"));
  EXPECT_EQ(cpu, Variadic::Integer(cpu0));

  ASSERT_OK_AND_ASSIGN(auto is_timebase, GetArg(*rr, "is_timebase"));
  EXPECT_EQ(is_timebase, Variadic::Boolean(true));

  // Name derived from the timebase.
  std::string track_name = context.storage->GetString(rr->name()).ToStdString();
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
  const auto& track_table = context.storage->track_table();
  auto rr = track_table.FindById(track_id);

  // track exists and looks sensible
  ASSERT_TRUE(rr.has_value());

  ASSERT_OK_AND_ASSIGN(auto perf_session_id,
                       GetDimension(*rr, "perf_session_id"));
  EXPECT_EQ(perf_session_id, Variadic::Integer(stream.perf_session_id.value));

  ASSERT_OK_AND_ASSIGN(auto cpu, GetDimension(*rr, "cpu"));
  EXPECT_EQ(cpu, Variadic::Integer(cpu0));

  ASSERT_OK_AND_ASSIGN(auto is_timebase, GetArg(*rr, "is_timebase"));
  EXPECT_EQ(is_timebase, Variadic::Boolean(true));

  // Name derived from the timebase.
  std::string track_name = context.storage->GetString(rr->name()).ToStdString();
  ASSERT_EQ(track_name, "sched:sched_switch");
}

TEST_F(PerfSampleTrackerTest, UnknownCounterTreatedAsCpuClock) {
  uint32_t seq_id = 42;
  uint32_t cpu0 = 0;

  auto stream = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu0, /*nullable_defaults=*/nullptr);

  TrackId track_id = stream.timebase_track_id;
  const auto& track_table = context.storage->track_table();
  auto rr = track_table.FindById(track_id);

  // track exists and looks sensible
  ASSERT_TRUE(rr.has_value());

  ASSERT_OK_AND_ASSIGN(auto perf_session_id,
                       GetDimension(*rr, "perf_session_id"));
  EXPECT_EQ(perf_session_id, Variadic::Integer(stream.perf_session_id.value));

  ASSERT_OK_AND_ASSIGN(auto cpu, GetDimension(*rr, "cpu"));
  EXPECT_EQ(cpu, Variadic::Integer(cpu0));

  ASSERT_OK_AND_ASSIGN(auto is_timebase, GetArg(*rr, "is_timebase"));
  EXPECT_EQ(is_timebase, Variadic::Boolean(true));

  // If the trace doesn't have a PerfSampleDefaults describing the timebase
  // counter, we assume cpu-clock.
  std::string track_name = context.storage->GetString(rr->name()).ToStdString();
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
  const auto& track_table = context.storage->track_table();
  auto rr = track_table.FindById(track_id);

  // track exists and looks sensible
  ASSERT_TRUE(rr.has_value());

  ASSERT_OK_AND_ASSIGN(auto perf_session_id,
                       GetDimension(*rr, "perf_session_id"));
  EXPECT_EQ(perf_session_id, Variadic::Integer(stream.perf_session_id.value));

  ASSERT_OK_AND_ASSIGN(auto cpu, GetDimension(*rr, "cpu"));
  EXPECT_EQ(cpu, Variadic::Integer(cpu0));

  ASSERT_OK_AND_ASSIGN(auto is_timebase, GetArg(*rr, "is_timebase"));
  EXPECT_EQ(is_timebase, Variadic::Boolean(true));

  // Using the config-supplied name for the track.
  std::string track_name = context.storage->GetString(rr->name()).ToStdString();
  ASSERT_EQ(track_name, "test-name");
}

// Validate that associated counters in the description create related tracks.
TEST_F(PerfSampleTrackerTest, FollowersTracks) {
  uint32_t seq_id = 42;
  uint32_t cpu_id = 0;

  protos::gen::TracePacketDefaults defaults;
  auto* perf_defaults = defaults.mutable_perf_sample_defaults();
  perf_defaults->mutable_timebase()->set_name("leader");

  // Associate a raw event.
  auto* raw_follower = perf_defaults->add_followers();
  raw_follower->set_name("raw");
  auto* raw_event = raw_follower->mutable_raw_event();
  raw_event->set_type(8);
  raw_event->set_config(18);

  // Associate a tracepoint.
  auto* tracepoint_follower = perf_defaults->add_followers();
  tracepoint_follower->set_name("tracepoint");
  tracepoint_follower->mutable_tracepoint()->set_name("sched:sched_switch");

  // Associate a HW counter.
  auto* counter_follower = perf_defaults->add_followers();
  counter_follower->set_name("pmu");
  counter_follower->set_counter(protos::gen::PerfEvents::HW_CACHE_MISSES);

  // Serialize the packet.
  auto defaults_pb = defaults.SerializeAsString();
  protos::pbzero::TracePacketDefaults::Decoder defaults_decoder(defaults_pb);

  auto stream = context.perf_sample_tracker->GetSamplingStreamInfo(
      seq_id, cpu_id, &defaults_decoder);

  ASSERT_EQ(stream.follower_track_ids.size(), 3u);

  std::vector<TrackId> track_ids;
  track_ids.push_back(stream.timebase_track_id);
  track_ids.insert(track_ids.end(), stream.follower_track_ids.begin(),
                   stream.follower_track_ids.end());
  std::vector<std::string> track_names = {"leader", "raw", "tracepoint", "pmu"};

  ASSERT_EQ(track_ids.size(), track_names.size());

  for (size_t i = 0; i < track_ids.size(); ++i) {
    TrackId track_id = track_ids[i];
    const auto& track_table = context.storage->track_table();
    auto rr = track_table.FindById(track_id);

    // Check the track exists and looks sensible.
    ASSERT_TRUE(rr.has_value());

    std::string track_name =
        context.storage->GetString(rr->name()).ToStdString();

    ASSERT_OK_AND_ASSIGN(auto perf_session_id,
                         GetDimension(*rr, "perf_session_id"));
    EXPECT_EQ(perf_session_id, Variadic::Integer(stream.perf_session_id.value));

    ASSERT_OK_AND_ASSIGN(auto cpu, GetDimension(*rr, "cpu"));
    EXPECT_EQ(cpu, Variadic::Integer(cpu_id));

    ASSERT_OK_AND_ASSIGN(auto is_timebase, GetArg(*rr, "is_timebase"));
    EXPECT_EQ(is_timebase, Variadic::Boolean(track_name == "leader"));

    // Using the config-supplied name for the track.
    ASSERT_EQ(track_name, track_names[i]);
  }
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
      static_cast<int>(stream.perf_session_id.value));
  std::optional<int64_t> chosen_shard = context.storage->GetIndexedStats(
      stats::perf_chosen_process_shard,
      static_cast<int>(stream.perf_session_id.value));

  ASSERT_TRUE(shard_count.has_value());
  EXPECT_EQ(shard_count.value(), 8);
  ASSERT_TRUE(chosen_shard.has_value());
  EXPECT_EQ(chosen_shard.value(), 7);

  std::optional<int64_t> shard_count2 = context.storage->GetIndexedStats(
      stats::perf_process_shard_count,
      static_cast<int>(stream.perf_session_id.value));
  std::optional<int64_t> chosen_shard2 = context.storage->GetIndexedStats(
      stats::perf_chosen_process_shard,
      static_cast<int>(stream.perf_session_id.value));

  ASSERT_TRUE(shard_count2.has_value());
  EXPECT_EQ(shard_count2.value(), 8);
  ASSERT_TRUE(chosen_shard2.has_value());
  EXPECT_EQ(chosen_shard2.value(), 7);
}

}  // namespace
}  // namespace perfetto::trace_processor
