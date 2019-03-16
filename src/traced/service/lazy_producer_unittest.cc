/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/traced/service/lazy_producer.h"

#include "src/base/test/test_task_runner.h"

#include "perfetto/tracing/core/data_source_config.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace {

constexpr const char* kDataSourceName = "android.heapprofd";
constexpr const char* kPropertyName = "persist.heapprofd.enable";

using ::testing::_;
using ::testing::InSequence;
using ::testing::Return;

class MockLazyProducer : public LazyProducer {
 public:
  MockLazyProducer(base::TaskRunner* task_runner)
      : LazyProducer(task_runner, 0, kDataSourceName, kPropertyName) {}

  MOCK_METHOD2(SetAndroidProperty,
               bool(const std::string&, const std::string&));
};

TEST(LazyProducersTest, Simple) {
  DataSourceConfig cfg;
  cfg.set_name(kDataSourceName);
  base::TestTaskRunner task_runner;
  MockLazyProducer p(&task_runner);
  InSequence s;
  EXPECT_CALL(p, SetAndroidProperty(kPropertyName, "1")).WillOnce(Return(true));
  EXPECT_CALL(p, SetAndroidProperty(kPropertyName, "0")).WillOnce(Return(true));
  p.SetupDataSource(1, cfg);
  p.StopDataSource(1);
  task_runner.RunUntilIdle();
}

TEST(LazyProducersTest, RefCount) {
  DataSourceConfig cfg;
  cfg.set_name(kDataSourceName);
  base::TestTaskRunner task_runner;
  MockLazyProducer p(&task_runner);
  InSequence s;
  EXPECT_CALL(p, SetAndroidProperty(kPropertyName, "1"))
      .WillRepeatedly(Return(true));
  p.SetupDataSource(1, cfg);
  p.SetupDataSource(2, cfg);
  p.StopDataSource(2);
  task_runner.RunUntilIdle();
  EXPECT_CALL(p, SetAndroidProperty(kPropertyName, "0")).WillOnce(Return(true));
  p.StopDataSource(1);
  task_runner.RunUntilIdle();
}

TEST(LazyProducersTest, NoFlap) {
  DataSourceConfig cfg;
  cfg.set_name(kDataSourceName);
  base::TestTaskRunner task_runner;
  MockLazyProducer p(&task_runner);
  InSequence s;
  EXPECT_CALL(p, SetAndroidProperty(kPropertyName, "1"))
      .WillRepeatedly(Return(true));
  p.SetupDataSource(1, cfg);
  p.StopDataSource(1);
  p.SetupDataSource(2, cfg);
  task_runner.RunUntilIdle();
  p.StopDataSource(2);
  EXPECT_CALL(p, SetAndroidProperty(kPropertyName, "0")).WillOnce(Return(true));
  task_runner.RunUntilIdle();
}

}  // namespace
}  // namespace perfetto
