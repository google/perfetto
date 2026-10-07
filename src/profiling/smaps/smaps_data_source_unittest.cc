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

#include "src/profiling/smaps/smaps_data_source.h"

#include <sys/types.h>

#include <optional>
#include <string>
#include <vector>

#include "perfetto/tracing/core/data_source_config.h"
#include "protos/perfetto/config/profiling/smaps_config.gen.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// Mirrors the bounds in smaps_data_source.cc.
constexpr uint32_t kMinReadPeriodMs = 1000;
constexpr uint32_t kMaxReadPeriodMs = 24 * 60 * 60 * 1000;

std::optional<SmapsDataSource::Config> CreateConfig(
    const protos::gen::ProcessSmapsConfig& smaps_cfg_pb) {
  DataSourceConfig ds_config;
  ds_config.set_name(SmapsDataSource::kDataSourceName);
  ds_config.set_process_smaps_config_raw(smaps_cfg_pb.SerializeAsString());
  return SmapsDataSource::Config::Create(ds_config);
}

TEST(SmapsDataSourceConfigTest, RejectsConfigWithoutTargets) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.set_read_period_ms(2000);

  EXPECT_FALSE(CreateConfig(smaps_cfg_pb).has_value());
}

TEST(SmapsDataSourceConfigTest, KeepsTargetCmdlines) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("top");
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("/bin/e*");

  std::optional<SmapsDataSource::Config> config = CreateConfig(smaps_cfg_pb);

  ASSERT_TRUE(config.has_value());
  EXPECT_THAT(config->target_cmdlines, ElementsAre("top", "/bin/e*"));
}

TEST(SmapsDataSourceConfigTest, KeepsRecordingConfig) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("top");
  smaps_cfg_pb.mutable_smaps_config()->set_unaggregated(true);
  smaps_cfg_pb.mutable_smaps_config()->add_vma_fields(
      protos::gen::SmapsConfig::VMA_FIELD_PSS);

  std::optional<SmapsDataSource::Config> config = CreateConfig(smaps_cfg_pb);

  ASSERT_TRUE(config.has_value());
  EXPECT_TRUE(config->recording_config.unaggregated());
  EXPECT_THAT(config->recording_config.vma_fields(),
              ElementsAre(protos::gen::SmapsConfig::VMA_FIELD_PSS));
}

TEST(SmapsDataSourceConfigTest, UnsetReadPeriodMeansOneShot) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("top");

  std::optional<SmapsDataSource::Config> config = CreateConfig(smaps_cfg_pb);

  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->read_period_ms, 0u);
}

TEST(SmapsDataSourceConfigTest, KeepsInRangeReadPeriod) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("top");
  smaps_cfg_pb.set_read_period_ms(2000);

  std::optional<SmapsDataSource::Config> config = CreateConfig(smaps_cfg_pb);

  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->read_period_ms, 2000u);
}

TEST(SmapsDataSourceConfigTest, ClampsTooSmallReadPeriod) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("top");
  smaps_cfg_pb.set_read_period_ms(1);

  std::optional<SmapsDataSource::Config> config = CreateConfig(smaps_cfg_pb);

  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->read_period_ms, kMinReadPeriodMs);
}

TEST(SmapsDataSourceConfigTest, ClampsTooLargeReadPeriod) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("top");
  smaps_cfg_pb.set_read_period_ms(kMaxReadPeriodMs * 2);

  std::optional<SmapsDataSource::Config> config = CreateConfig(smaps_cfg_pb);

  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->read_period_ms, kMaxReadPeriodMs);
}

TEST(SmapsDataSourceConfigTest, UnsetMaxProcessesMeansUnlimited) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("top");

  std::optional<SmapsDataSource::Config> config = CreateConfig(smaps_cfg_pb);

  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->max_processes_per_period, 0u);
}

TEST(SmapsDataSourceConfigTest, KeepsMaxProcesses) {
  protos::gen::ProcessSmapsConfig smaps_cfg_pb;
  smaps_cfg_pb.mutable_scope()->add_target_cmdline("top");
  smaps_cfg_pb.set_max_processes_per_period(5);

  std::optional<SmapsDataSource::Config> config = CreateConfig(smaps_cfg_pb);

  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->max_processes_per_period, 5u);
}

using HashAndPid = SmapsDataSource::HashAndPid;

bool MatchAll(pid_t) {
  return true;
}

TEST(SmapsDataSourceWalkTest, UnlimitedSelectsAllMatches) {
  std::vector<HashAndPid> walk = {{1, 30}, {2, 10}, {3, 20}};
  HashAndPid last_picked{0, 0};

  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 0, &last_picked),
      ElementsAre(30, 10, 20));
  EXPECT_EQ(last_picked, HashAndPid(3, 20));
  // All matches are selected every time, regardless of |last_picked|.
  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 0, &last_picked),
      ElementsAre(30, 10, 20));
}

TEST(SmapsDataSourceWalkTest, CapRotatesThroughMatches) {
  std::vector<HashAndPid> walk = {{1, 10}, {2, 20}, {3, 30}, {4, 40}, {5, 50}};
  HashAndPid last_picked{0, 0};

  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 2, &last_picked),
      ElementsAre(10, 20));
  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 2, &last_picked),
      ElementsAre(30, 40));
  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 2, &last_picked),
      ElementsAre(50, 10));
  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 2, &last_picked),
      ElementsAre(20, 30));
}

TEST(SmapsDataSourceWalkTest, CapLargerThanMatchesSelectsEachOnce) {
  std::vector<HashAndPid> walk = {{1, 10}, {2, 20}, {3, 30}};
  HashAndPid last_picked{2, 20};

  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 10, &last_picked),
      ElementsAre(30, 10, 20));
}

TEST(SmapsDataSourceWalkTest, LastPickedOnlyAdvancesPastMatches) {
  std::vector<HashAndPid> walk = {{1, 10}, {2, 20}, {3, 30}, {4, 40}};
  auto only_20_and_40 = [](pid_t pid) { return pid == 20 || pid == 40; };
  HashAndPid last_picked{0, 0};

  EXPECT_THAT(SmapsDataSource::PickMatchingTargets(walk, only_20_and_40, 1,
                                                   &last_picked),
              ElementsAre(20));
  EXPECT_EQ(last_picked, HashAndPid(2, 20));
  EXPECT_THAT(SmapsDataSource::PickMatchingTargets(walk, only_20_and_40, 1,
                                                   &last_picked),
              ElementsAre(40));
  EXPECT_THAT(SmapsDataSource::PickMatchingTargets(walk, only_20_and_40, 1,
                                                   &last_picked),
              ElementsAre(20));
}

TEST(SmapsDataSourceWalkTest, LastPickedSurvivesExitedProcess) {
  // The previously picked process is no longer in the walk.
  std::vector<HashAndPid> walk = {{1, 10}, {2, 20}, {4, 40}};
  HashAndPid last_picked{3, 30};

  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 2, &last_picked),
      ElementsAre(40, 10));
}

TEST(SmapsDataSourceWalkTest, TiesBrokenByPid) {
  std::vector<HashAndPid> walk = {{1, 10}, {1, 20}, {1, 30}};
  HashAndPid last_picked{0, 0};

  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 1, &last_picked),
      ElementsAre(10));
  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets(walk, MatchAll, 1, &last_picked),
      ElementsAre(20));
}

TEST(SmapsDataSourceWalkTest, NoMatchesLeavesLastPicked) {
  std::vector<HashAndPid> walk = {{1, 10}, {2, 20}};
  HashAndPid last_picked{1, 10};

  EXPECT_THAT(SmapsDataSource::PickMatchingTargets(
                  walk, [](pid_t) { return false; }, 1, &last_picked),
              IsEmpty());
  EXPECT_EQ(last_picked, HashAndPid(1, 10));
  EXPECT_THAT(
      SmapsDataSource::PickMatchingTargets({}, MatchAll, 1, &last_picked),
      IsEmpty());
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
