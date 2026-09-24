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

#include <optional>
#include <string>

#include "perfetto/tracing/core/data_source_config.h"
#include "protos/perfetto/config/profiling/smaps_config.gen.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

using ::testing::ElementsAre;

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

}  // namespace
}  // namespace profiling
}  // namespace perfetto
