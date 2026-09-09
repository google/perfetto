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

#include "src/trace_redaction/merge_synthetic_sched.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {

namespace {

constexpr int32_t kSynthTgid = 9999;
constexpr int32_t kSynthCpu0 = 10000;
constexpr int32_t kSynthCpu1 = 10001;

constexpr int32_t kTargetPidA = 200;
constexpr int32_t kTargetPidB = 300;

constexpr int32_t kCpu0 = 0;
constexpr int32_t kCpu1 = 1;

constexpr auto kCommA = "comm-a";
constexpr auto kCommB = "comm-b";

}  // namespace

class MergeSyntheticSchedTest : public testing::Test {
 protected:
  void SetUp() override {
    context_.synthetic_process = std::make_unique<SyntheticProcess>(
        kSynthTgid, std::vector<int32_t>{kSynthCpu0, kSynthCpu1});

    auto* bundle = packet_.mutable_ftrace_events();
    bundle->set_cpu(kCpu0);
    compact_sched_ = bundle->mutable_compact_sched();
  }

  void AddSwitchEvent(uint64_t ts,
                      int32_t next_pid,
                      int32_t prev_state,
                      int32_t prio,
                      uint32_t comm) {
    compact_sched_->add_switch_timestamp(ts);
    compact_sched_->add_switch_next_pid(next_pid);
    compact_sched_->add_switch_prev_state(prev_state);
    compact_sched_->add_switch_next_prio(prio);
    compact_sched_->add_switch_next_comm_index(comm);
  }

  protos::gen::TracePacket packet_;
  protos::gen::FtraceEventBundle::CompactSched* compact_sched_ = nullptr;
  Context context_;
  MergeSyntheticSched merge_;
};

TEST(SyntheticProcessValidatorTest, ReturnsErrorIfSyntheticProcessMissing) {
  SyntheticProcessValidator validator;
  Context context;
  context.synthetic_process.reset();

  auto status = validator.Validate(context);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.message(),
            "SyntheticProcessValidator: missing synthetic process.");
}

TEST(SyntheticProcessValidatorTest, ReturnsErrorIfSyntheticProcessTidsEmpty) {
  SyntheticProcessValidator validator;
  Context context;
  context.synthetic_process =
      std::make_unique<SyntheticProcess>(kSynthTgid, std::vector<int32_t>{});

  auto status = validator.Validate(context);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(
      status.message(),
      "SyntheticProcessValidator: no synthetic threads in synthetic process.");
}

TEST(SyntheticProcessValidatorTest, ReturnsOkIfSyntheticProcessValid) {
  SyntheticProcessValidator validator;
  Context context;
  context.synthetic_process = std::make_unique<SyntheticProcess>(
      kSynthTgid, std::vector<int32_t>{kSynthCpu0, kSynthCpu1});

  ASSERT_OK(validator.Validate(context));
}

TEST_F(MergeSyntheticSchedTest, ReturnsErrorIfCpuOutOfBounds) {
  AddSwitchEvent(100, kSynthCpu0, 0, 120, 0);

  packet_.mutable_ftrace_events()->set_cpu(99);

  auto buffer = packet_.SerializeAsString();
  auto status = merge_.Transform(context_, &buffer);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(
      status.message(),
      "MergeSyntheticSched: cpu index out of bounds for synthetic process.");
}

TEST_F(MergeSyntheticSchedTest, PassesThroughNonFtracePacket) {
  protos::gen::TracePacket non_ftrace_packet;
  non_ftrace_packet.set_timestamp(12345);

  auto buffer = non_ftrace_packet.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  EXPECT_EQ(result.timestamp(), 12345u);
  EXPECT_FALSE(result.has_ftrace_events());
}

TEST_F(MergeSyntheticSchedTest, PassesThroughFtraceWithoutCompactSched) {
  protos::gen::TracePacket ftrace_packet;
  auto* bundle = ftrace_packet.mutable_ftrace_events();
  bundle->set_cpu(kCpu0);
  auto* evt = bundle->add_event();
  evt->set_timestamp(500);

  auto buffer = ftrace_packet.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  ASSERT_TRUE(result.has_ftrace_events());
  EXPECT_EQ(result.ftrace_events().cpu(), static_cast<uint32_t>(kCpu0));
  EXPECT_FALSE(result.ftrace_events().has_compact_sched());
  EXPECT_EQ(result.ftrace_events().event_size(), 1);
}

TEST_F(MergeSyntheticSchedTest, PreservesWakingEventsAndInternTable) {
  compact_sched_->add_intern_table(kCommA);
  compact_sched_->add_intern_table(kCommB);

  compact_sched_->add_waking_timestamp(100);
  compact_sched_->add_waking_pid(200);
  compact_sched_->add_waking_target_cpu(1);
  compact_sched_->add_waking_prio(120);
  compact_sched_->add_waking_comm_index(0);
  compact_sched_->add_waking_common_flags(0);

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  ASSERT_TRUE(result.has_ftrace_events());
  ASSERT_TRUE(result.ftrace_events().has_compact_sched());

  const auto& sched = result.ftrace_events().compact_sched();
  ASSERT_EQ(sched.intern_table_size(), 2);
  EXPECT_EQ(sched.intern_table().at(0), kCommA);
  EXPECT_EQ(sched.intern_table().at(1), kCommB);

  ASSERT_EQ(sched.waking_timestamp_size(), 1);
  EXPECT_EQ(sched.waking_timestamp().at(0), 100u);
  EXPECT_EQ(sched.waking_pid().at(0), 200);
  EXPECT_EQ(sched.waking_target_cpu().at(0), 1);
  EXPECT_EQ(sched.waking_prio().at(0), 120);
  EXPECT_EQ(sched.waking_comm_index().at(0), 0u);
}

TEST_F(MergeSyntheticSchedTest, SingleSyntheticEventPreserved) {
  AddSwitchEvent(100, kTargetPidA, 0, 100, 0);
  AddSwitchEvent(50, kSynthCpu0, 1, 120, 1);
  AddSwitchEvent(60, kTargetPidB, 2, 100, 0);

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  const auto& sched = result.ftrace_events().compact_sched();

  ASSERT_EQ(sched.switch_timestamp_size(), 3);
  EXPECT_EQ(sched.switch_timestamp().at(0), 100u);
  EXPECT_EQ(sched.switch_timestamp().at(1), 50u);
  EXPECT_EQ(sched.switch_timestamp().at(2), 60u);

  EXPECT_EQ(sched.switch_next_pid().at(0), kTargetPidA);
  EXPECT_EQ(sched.switch_next_pid().at(1), kSynthCpu0);
  EXPECT_EQ(sched.switch_next_pid().at(2), kTargetPidB);
}

TEST_F(MergeSyntheticSchedTest, ConsecutiveSyntheticEventsInMiddleCoalesced) {
  AddSwitchEvent(10, kTargetPidA, 0, 100, 0);
  AddSwitchEvent(20, kSynthCpu0, 1, 120, 1);
  AddSwitchEvent(30, kSynthCpu0, 2, 120, 1);  // duplicate
  AddSwitchEvent(40, kSynthCpu0, 3, 120, 1);  // duplicate
  AddSwitchEvent(50, kTargetPidB, 0, 100, 0);

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  const auto& sched = result.ftrace_events().compact_sched();

  ASSERT_EQ(sched.switch_timestamp_size(), 3);
  ASSERT_EQ(sched.switch_next_pid_size(), 3);
  ASSERT_EQ(sched.switch_prev_state_size(), 3);
  ASSERT_EQ(sched.switch_next_prio_size(), 3);
  ASSERT_EQ(sched.switch_next_comm_index_size(), 3);

  // Event 0: TargetPidA
  EXPECT_EQ(sched.switch_timestamp().at(0), 10u);
  EXPECT_EQ(sched.switch_next_pid().at(0), kTargetPidA);
  EXPECT_EQ(sched.switch_prev_state().at(0), 0);
  EXPECT_EQ(sched.switch_next_prio().at(0), 100);
  EXPECT_EQ(sched.switch_next_comm_index().at(0), 0u);

  // Event 1: First Synth event preserved (with state 1, prio 120, comm 1)
  EXPECT_EQ(sched.switch_timestamp().at(1), 20u);
  EXPECT_EQ(sched.switch_next_pid().at(1), kSynthCpu0);
  EXPECT_EQ(sched.switch_prev_state().at(1), 1);
  EXPECT_EQ(sched.switch_next_prio().at(1), 120);
  EXPECT_EQ(sched.switch_next_comm_index().at(1), 1u);

  // Event 2: TargetPidB with accumulated deltas 50 + 30 + 40 = 120
  EXPECT_EQ(sched.switch_timestamp().at(2), 120u);
  EXPECT_EQ(sched.switch_next_pid().at(2), kTargetPidB);
  EXPECT_EQ(sched.switch_prev_state().at(2), 0);
  EXPECT_EQ(sched.switch_next_prio().at(2), 100);
  EXPECT_EQ(sched.switch_next_comm_index().at(2), 0u);
}

TEST_F(MergeSyntheticSchedTest, ConsecutiveSyntheticEventsAtStartCoalesced) {
  AddSwitchEvent(1000, kSynthCpu0, 1, 120, 1);
  AddSwitchEvent(25, kSynthCpu0, 2, 120, 1);  // duplicate
  AddSwitchEvent(50, kTargetPidB, 0, 100, 0);

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  const auto& sched = result.ftrace_events().compact_sched();

  ASSERT_EQ(sched.switch_timestamp_size(), 2);
  ASSERT_EQ(sched.switch_next_pid_size(), 2);

  // Event 0: Synth event at start
  EXPECT_EQ(sched.switch_timestamp().at(0), 1000u);
  EXPECT_EQ(sched.switch_next_pid().at(0), kSynthCpu0);
  EXPECT_EQ(sched.switch_prev_state().at(0), 1);

  // Event 1: TargetPidB with accumulated delta 50 + 25 = 75
  EXPECT_EQ(sched.switch_timestamp().at(1), 75u);
  EXPECT_EQ(sched.switch_next_pid().at(1), kTargetPidB);
}

TEST_F(MergeSyntheticSchedTest, ConsecutiveSyntheticEventsAtEndCoalesced) {
  AddSwitchEvent(100, kTargetPidA, 0, 100, 0);
  AddSwitchEvent(20, kSynthCpu0, 1, 120, 1);
  AddSwitchEvent(30, kSynthCpu0, 2, 120, 1);  // duplicate
  AddSwitchEvent(40, kSynthCpu0, 3, 120, 1);  // duplicate

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  const auto& sched = result.ftrace_events().compact_sched();

  ASSERT_EQ(sched.switch_timestamp_size(), 2);
  ASSERT_EQ(sched.switch_next_pid_size(), 2);

  EXPECT_EQ(sched.switch_timestamp().at(0), 100u);
  EXPECT_EQ(sched.switch_next_pid().at(0), kTargetPidA);

  EXPECT_EQ(sched.switch_timestamp().at(1), 20u);
  EXPECT_EQ(sched.switch_next_pid().at(1), kSynthCpu0);
  EXPECT_EQ(sched.switch_prev_state().at(1), 1);
}

TEST_F(MergeSyntheticSchedTest, AllSyntheticEventsBundleCoalesced) {
  AddSwitchEvent(100, kSynthCpu0, 1, 120, 1);
  AddSwitchEvent(50, kSynthCpu0, 2, 120, 1);
  AddSwitchEvent(75, kSynthCpu0, 3, 120, 1);

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  const auto& sched = result.ftrace_events().compact_sched();

  ASSERT_EQ(sched.switch_timestamp_size(), 1);
  ASSERT_EQ(sched.switch_next_pid_size(), 1);

  EXPECT_EQ(sched.switch_timestamp().at(0), 100u);
  EXPECT_EQ(sched.switch_next_pid().at(0), kSynthCpu0);
  EXPECT_EQ(sched.switch_prev_state().at(0), 1);
}

TEST_F(MergeSyntheticSchedTest, NonSyntheticConsecutivePidsAreNotCoalesced) {
  AddSwitchEvent(10, kTargetPidA, 0, 100, 0);
  AddSwitchEvent(20, kTargetPidA, 0, 100, 0);
  AddSwitchEvent(30, kTargetPidA, 0, 100, 0);

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  const auto& sched = result.ftrace_events().compact_sched();

  ASSERT_EQ(sched.switch_timestamp_size(), 3);
  EXPECT_EQ(sched.switch_timestamp().at(0), 10u);
  EXPECT_EQ(sched.switch_timestamp().at(1), 20u);
  EXPECT_EQ(sched.switch_timestamp().at(2), 30u);
  EXPECT_EQ(sched.switch_next_pid().at(0), kTargetPidA);
  EXPECT_EQ(sched.switch_next_pid().at(1), kTargetPidA);
  EXPECT_EQ(sched.switch_next_pid().at(2), kTargetPidA);
}

TEST_F(MergeSyntheticSchedTest, MultipleSyntheticChainsCoalesced) {
  AddSwitchEvent(10, kSynthCpu0, 1, 120, 1);
  AddSwitchEvent(20, kSynthCpu0, 2, 120, 1);   // duplicate chain 1
  AddSwitchEvent(30, kTargetPidA, 0, 100, 0);  // break 1 (+20 -> 50)
  AddSwitchEvent(40, kSynthCpu0, 3, 120, 1);
  AddSwitchEvent(50, kSynthCpu0, 4, 120, 1);   // duplicate chain 2
  AddSwitchEvent(60, kTargetPidB, 0, 100, 0);  // break 2 (+50 -> 110)

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  const auto& sched = result.ftrace_events().compact_sched();

  ASSERT_EQ(sched.switch_timestamp_size(), 4);
  ASSERT_EQ(sched.switch_next_pid_size(), 4);

  EXPECT_EQ(sched.switch_timestamp().at(0), 10u);
  EXPECT_EQ(sched.switch_next_pid().at(0), kSynthCpu0);

  EXPECT_EQ(sched.switch_timestamp().at(1), 50u);
  EXPECT_EQ(sched.switch_next_pid().at(1), kTargetPidA);

  EXPECT_EQ(sched.switch_timestamp().at(2), 40u);
  EXPECT_EQ(sched.switch_next_pid().at(2), kSynthCpu0);

  EXPECT_EQ(sched.switch_timestamp().at(3), 110u);
  EXPECT_EQ(sched.switch_next_pid().at(3), kTargetPidB);
}

TEST_F(MergeSyntheticSchedTest,
       SyntheticPidOnDifferentCpuNotTreatedAsSynthetic) {
  // Bundle is configured on CPU 1.
  packet_.mutable_ftrace_events()->set_cpu(kCpu1);

  // kSynthCpu0 is the synthetic thread for CPU 0, NOT CPU 1 (which is
  // kSynthCpu1).
  AddSwitchEvent(10, kSynthCpu0, 0, 100, 0);
  AddSwitchEvent(20, kSynthCpu0, 0, 100, 0);

  auto buffer = packet_.SerializeAsString();
  ASSERT_OK(merge_.Transform(context_, &buffer));

  protos::gen::TracePacket result;
  ASSERT_TRUE(result.ParseFromString(buffer));
  const auto& sched = result.ftrace_events().compact_sched();

  ASSERT_EQ(sched.switch_timestamp_size(), 2);
  EXPECT_EQ(sched.switch_timestamp().at(0), 10u);
  EXPECT_EQ(sched.switch_timestamp().at(1), 20u);
  EXPECT_EQ(sched.switch_next_pid().at(0), kSynthCpu0);
  EXPECT_EQ(sched.switch_next_pid().at(1), kSynthCpu0);
}

}  // namespace perfetto::trace_redaction
