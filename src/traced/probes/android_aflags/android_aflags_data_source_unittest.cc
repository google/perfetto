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

#include "src/traced/probes/android_aflags/android_aflags_data_source.h"

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/android/android_aflags.gen.h"
#include "protos/perfetto/trace/android/android_aflags.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

using ::testing::HasSubstr;

class TestAndroidAflagsDataSource : public AndroidAflagsDataSource {
 public:
  TestAndroidAflagsDataSource(const DataSourceConfig& ds_config,
                              base::TaskRunner* task_runner,
                              TracingSessionID session_id,
                              std::unique_ptr<TraceWriter> writer)
      : AndroidAflagsDataSource(ds_config,
                                task_runner,
                                session_id,
                                std::move(writer)) {}

  struct FakeFlag {
    std::string pkg;
    std::string name;
    std::string namespace_val;
    std::string container;
    std::string value;
    std::string staged_value;
    uint32_t permission;
    uint32_t value_picked_from;
    uint32_t storage_backend;
  };

  // Helper to simulate subprocess completion with given flags.
  void FakeSubprocessCompletion(const std::vector<FakeFlag>& flags) {
    // Generate proto binary (using aflags tool's field IDs)
    protozero::HeapBuffered<protos::pbzero::AndroidAflags> msg;
    for (const auto& f : flags) {
      auto* flag_proto = msg->add_flags();
      flag_proto->AppendString(1, f.namespace_val);
      flag_proto->AppendString(2, f.name);
      flag_proto->AppendString(3, f.pkg);
      flag_proto->AppendString(4, f.container);
      flag_proto->AppendString(5, f.value);
      flag_proto->AppendString(6, f.staged_value);
      flag_proto->AppendVarInt(7, f.permission);
      flag_proto->AppendVarInt(8, f.value_picked_from);
      flag_proto->AppendVarInt(9, f.storage_backend);
    }
    std::vector<uint8_t> bytes = msg.SerializeAsArray();
    aflags_output_ = base::Base64Encode(bytes.data(), bytes.size());
    // FinalizeAflagsCapture() expects aflags_process_ to be set because it
    // calls Poll() and then resets it. We simulate this by using a Subprocess
    // that has already finished.
    aflags_process_.emplace(std::initializer_list<std::string>{"/bin/true"});
    aflags_process_->Call();
    FinalizeAflagsCapture();
  }

  void FakeSubprocessError(const std::string& output) {
    aflags_output_ = output;
    aflags_process_.emplace(std::initializer_list<std::string>{"/bin/false"});
    aflags_process_->Call();
    FinalizeAflagsCapture();
  }

  void FakeInvalidBase64() {
    aflags_output_ = "!!!not-base64!!!";
    aflags_process_.emplace(std::initializer_list<std::string>{"/bin/true"});
    aflags_process_->Call();
    FinalizeAflagsCapture();
  }
};

class AndroidAflagsDataSourceTest : public ::testing::Test {
 protected:
  std::unique_ptr<TestAndroidAflagsDataSource> CreateDataSource(
      const DataSourceConfig& ds_config) {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    writer_raw_ = writer.get();
    return std::unique_ptr<TestAndroidAflagsDataSource>(
        new TestAndroidAflagsDataSource(ds_config, &task_runner_, 0,
                                        std::move(writer)));
  }

  base::TestTaskRunner task_runner_;
  TraceWriterForTesting* writer_raw_ = nullptr;
};

TEST_F(AndroidAflagsDataSourceTest, EmitAflags) {
  DataSourceConfig ds_config;
  ds_config.set_name("android.aflags");

  auto ds = CreateDataSource(ds_config);

  std::vector<TestAndroidAflagsDataSource::FakeFlag> flags;
  flags.push_back({
      "com.android.settings",
      "my_flag",
      "settings_ns",
      "system",
      "enabled",
      "disabled",
      static_cast<uint32_t>(
          protos::pbzero::AndroidAflags::FLAG_PERMISSION_READ_WRITE),
      static_cast<uint32_t>(
          protos::pbzero::AndroidAflags::VALUE_PICKED_FROM_LOCAL),
      static_cast<uint32_t>(
          protos::pbzero::AndroidAflags::FLAG_STORAGE_BACKEND_ACONFIGD),
  });

  ds->FakeSubprocessCompletion(flags);

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_android_aflags());

  const auto& aflags = packet.android_aflags();
  ASSERT_EQ(aflags.flags().size(), 1u);
  EXPECT_EQ(aflags.flags()[0].pkg(), "com.android.settings");
  EXPECT_EQ(aflags.flags()[0].name(), "my_flag");
  EXPECT_EQ(aflags.flags()[0].flag_namespace(), "settings_ns");
  EXPECT_EQ(aflags.flags()[0].container(), "system");
  EXPECT_EQ(aflags.flags()[0].value(), "enabled");
  EXPECT_EQ(aflags.flags()[0].staged_value(), "disabled");
  EXPECT_EQ(aflags.flags()[0].permission(),
            protos::gen::AndroidAflags::FLAG_PERMISSION_READ_WRITE);
  EXPECT_EQ(aflags.flags()[0].value_picked_from(),
            protos::gen::AndroidAflags::VALUE_PICKED_FROM_LOCAL);
  EXPECT_EQ(aflags.flags()[0].storage_backend(),
            protos::gen::AndroidAflags::FLAG_STORAGE_BACKEND_ACONFIGD);
}

TEST_F(AndroidAflagsDataSourceTest, SubprocessErrorEmitsErrorPacket) {
  DataSourceConfig ds_config;
  ds_config.set_name("android.aflags");

  auto ds = CreateDataSource(ds_config);

  ds->FakeSubprocessError("aflags error message");

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_android_aflags());
  const auto& aflags = packet.android_aflags();
  EXPECT_TRUE(aflags.has_error());
  EXPECT_THAT(aflags.error(),
              testing::AllOf(HasSubstr("aflags failed"),
                             HasSubstr("aflags error message")));
}

TEST_F(AndroidAflagsDataSourceTest, InvalidBase64EmitsErrorPacket) {
  DataSourceConfig ds_config;
  ds_config.set_name("android.aflags");

  auto ds = CreateDataSource(ds_config);

  ds->FakeInvalidBase64();

  protos::gen::TracePacket packet = writer_raw_->GetOnlyTracePacket();
  ASSERT_TRUE(packet.has_android_aflags());
  const auto& aflags = packet.android_aflags();
  EXPECT_TRUE(aflags.has_error());
  EXPECT_THAT(aflags.error(), HasSubstr("Failed to decode aflags output"));
}

}  // namespace
}  // namespace perfetto
