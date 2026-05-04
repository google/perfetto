/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/traced/probes/journald/journald_data_source.h"

#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/linux/journald_config.gen.h"

using ::perfetto::protos::gen::JournaldConfig;

namespace perfetto {
namespace {

class JournaldDataSourceTest : public ::testing::Test {
 protected:
  void CreateInstance(const DataSourceConfig& cfg) {
    auto writer = std::make_unique<TraceWriterForTesting>();
    writer_raw_ = writer.get();
    data_source_ = std::make_unique<JournaldDataSource>(
        cfg, &task_runner_, /*session_id=*/0, std::move(writer));
  }

  base::TestTaskRunner task_runner_;
  std::unique_ptr<JournaldDataSource> data_source_;
  TraceWriterForTesting* writer_raw_ = nullptr;
};

// Creating a data source without any config should not crash.
TEST_F(JournaldDataSourceTest, DefaultConfig) {
  DataSourceConfig cfg;
  CreateInstance(cfg);
  EXPECT_EQ(data_source_->stats().num_total, 0u);
  EXPECT_EQ(data_source_->stats().num_failed, 0u);
  EXPECT_EQ(data_source_->stats().num_skipped, 0u);
}

// Calling Flush() before Start() (journal_ == nullptr) must not crash and must
// invoke the callback.
TEST_F(JournaldDataSourceTest, FlushWithoutStart) {
  DataSourceConfig cfg;
  CreateInstance(cfg);
  bool callback_called = false;
  data_source_->Flush(0, [&callback_called] { callback_called = true; });
  EXPECT_TRUE(callback_called);
}

// Verify that min_prio and filter fields from the config are accepted without
// error during construction.
TEST_F(JournaldDataSourceTest, ConfigParsing) {
  JournaldConfig cfg_proto;
  cfg_proto.set_min_prio(3);
  cfg_proto.add_filter_identifiers("sshd");
  cfg_proto.add_filter_identifiers("kernel");
  cfg_proto.add_filter_units("nginx.service");

  DataSourceConfig cfg;
  cfg.set_journald_config_raw(cfg_proto.SerializeAsString());
  CreateInstance(cfg);
  // Construction must succeed and stats start at zero.
  EXPECT_EQ(data_source_->stats().num_total, 0u);
}

// Verify the static descriptor has the correct name.
TEST_F(JournaldDataSourceTest, Descriptor) {
  EXPECT_STREQ(JournaldDataSource::descriptor.name, "linux.journald");
}

}  // namespace
}  // namespace perfetto
