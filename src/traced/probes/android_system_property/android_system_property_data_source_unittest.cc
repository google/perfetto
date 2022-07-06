/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/traced/probes/android_system_property/android_system_property_data_source.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/android/android_system_property_config.gen.h"
#include "protos/perfetto/trace/android/android_system_property.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

using ::perfetto::protos::gen::AndroidSystemPropertyConfig;

using ::testing::AnyOf;
using ::testing::ElementsAre;
using ::testing::Return;

namespace perfetto {
namespace {

class TestAndroidSystemPropertyDataSource
    : public AndroidSystemPropertyDataSource {
 public:
  TestAndroidSystemPropertyDataSource(base::TaskRunner* task_runner,
                                      const DataSourceConfig& config,
                                      std::unique_ptr<TraceWriter> writer)
      : AndroidSystemPropertyDataSource(task_runner,
                                        config,
                                        /* session_id */ 0,
                                        std::move(writer)) {}

  MOCK_METHOD1(ReadProperty,
               const base::Optional<std::string>(const std::string&));
};

class AndroidSystemPropertyDataSourceTest : public ::testing::Test {
 protected:
  std::unique_ptr<TestAndroidSystemPropertyDataSource>
  CreateAndroidSystemPropertyDataSource(const DataSourceConfig& config) {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    writer_raw_ = writer.get();
    auto instance = std::unique_ptr<TestAndroidSystemPropertyDataSource>(
        new TestAndroidSystemPropertyDataSource(&task_runner_, config,
                                                std::move(writer)));
    return instance;
  }

  base::TestTaskRunner task_runner_;
  TraceWriterForTesting* writer_raw_ = nullptr;
};

DataSourceConfig BuildConfig(const std::vector<std::string>& property_names) {
  DataSourceConfig ds_config;
  AndroidSystemPropertyConfig cfg;
  for (auto name : property_names) {
    cfg.add_property_name(name);
  }
  ds_config.set_android_system_property_config_raw(cfg.SerializeAsString());
  return ds_config;
}

TEST_F(AndroidSystemPropertyDataSourceTest, Success) {
  auto data_source = CreateAndroidSystemPropertyDataSource(BuildConfig(
      {"debug.tracing.screen_state", "debug.tracing.screen_brightness"}));
  EXPECT_CALL(*data_source, ReadProperty("debug.tracing.screen_state"))
      .WillOnce(Return(base::make_optional("2")));
  EXPECT_CALL(*data_source, ReadProperty("debug.tracing.screen_brightness"))
      .WillOnce(Return(base::make_optional("0.123456")));
  data_source->Start();

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  EXPECT_TRUE(packet.has_android_system_property());
  auto properties = packet.android_system_property();
  EXPECT_EQ(properties.values_size(), 2);

  EXPECT_EQ(properties.values()[0].name(), "debug.tracing.screen_state");
  EXPECT_EQ(properties.values()[0].value(), "2");
  EXPECT_EQ(properties.values()[1].name(), "debug.tracing.screen_brightness");
  EXPECT_EQ(properties.values()[1].value(), "0.123456");
}

TEST_F(AndroidSystemPropertyDataSourceTest, NotPermitted) {
  auto data_source = CreateAndroidSystemPropertyDataSource(
      BuildConfig({"something.with.wrong.prefix"}));
  EXPECT_CALL(*data_source, ReadProperty("something.with.wrong.prefix"))
      .Times(0);
  data_source->Start();

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  EXPECT_TRUE(packet.has_android_system_property());
  auto properties = packet.android_system_property();
  EXPECT_EQ(properties.values_size(), 0);
}

TEST_F(AndroidSystemPropertyDataSourceTest, Failure) {
  auto data_source = CreateAndroidSystemPropertyDataSource(BuildConfig(
      {"debug.tracing.screen_state", "debug.tracing.screen_brightness"}));
  EXPECT_CALL(*data_source, ReadProperty("debug.tracing.screen_state"))
      .WillOnce(Return(base::nullopt));
  EXPECT_CALL(*data_source, ReadProperty("debug.tracing.screen_brightness"))
      .WillOnce(Return(base::nullopt));
  data_source->Start();

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  auto properties = packet.android_system_property();
  EXPECT_EQ(properties.values_size(), 0);
}

// TODO(simonmacm) test poll_ms
}  // namespace
}  // namespace perfetto
