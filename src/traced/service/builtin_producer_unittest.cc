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

#include "src/traced/service/builtin_producer.h"

#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/android/android_sdk_sysprop_guard_config.gen.h"

namespace perfetto {
namespace {

constexpr char kHeapprofdDataSourceName[] = "android.heapprofd";
constexpr char kTracedPerfDataSourceName[] = "linux.perf";
constexpr char kLazyHeapprofdPropertyName[] = "traced.lazy.heapprofd";
constexpr char kLazyTracedPerfPropertyName[] = "traced.lazy.traced_perf";

constexpr char kAndroidSdkSyspropGuardDataSourceName[] =
    "android.sdk_sysprop_guard";
constexpr char kPerfettoSdkSyspropGuardGenerationPropertyName[] =
    "debug.tracing.ctl.perfetto.sdk_sysprop_guard_generation";
constexpr char kHwuiSkiaBroadTracingPropertyName[] =
    "debug.tracing.ctl.hwui.skia_tracing_enabled";
constexpr char kHwuiSkiaUsePerfettoPropertyName[] =
    "debug.tracing.ctl.hwui.skia_use_perfetto_track_events";
constexpr char kHwuiSkiaPropertyPackageSeparator[] = ".";
constexpr char kSurfaceFlingerSkiaBroadTracingPropertyName[] =
    "debug.tracing.ctl.renderengine.skia_tracing_enabled";
constexpr char kSurfaceFlingerSkiaUsePerfettoPropertyName[] =
    "debug.tracing.ctl.renderengine.skia_use_perfetto_track_events";

using ::testing::_;
using ::testing::InvokeWithoutArgs;
using ::testing::Mock;
using ::testing::Return;
using ::testing::StrictMock;

class MockBuiltinProducer : public BuiltinProducer {
 public:
  MockBuiltinProducer(base::TaskRunner* task_runner)
      : BuiltinProducer(task_runner, /*lazy_stop_delay_ms=*/0) {}

  MOCK_METHOD(bool,
              SetAndroidProperty,
              (const std::string&, const std::string&),
              (override));
};

TEST(BuiltinProducerTest, LazyHeapprofdSimple) {
  DataSourceConfig cfg;
  cfg.set_name(kHeapprofdDataSourceName);
  base::TestTaskRunner task_runner;
  auto done = task_runner.CreateCheckpoint("done");
  StrictMock<MockBuiltinProducer> p(&task_runner);
  testing::InSequence s;
  EXPECT_CALL(p, SetAndroidProperty(kLazyHeapprofdPropertyName, "1"))
      .WillOnce(Return(true));
  EXPECT_CALL(p, SetAndroidProperty(kLazyHeapprofdPropertyName, ""))
      .WillOnce(InvokeWithoutArgs([&done]() {
        done();
        return true;
      }));
  p.SetupDataSource(1, cfg);
  p.StopDataSource(1);
  task_runner.RunUntilCheckpoint("done");
}

TEST(BuiltinProducerTest, LazyTracedPerfSimple) {
  DataSourceConfig cfg;
  cfg.set_name(kTracedPerfDataSourceName);
  base::TestTaskRunner task_runner;
  auto done = task_runner.CreateCheckpoint("done");
  StrictMock<MockBuiltinProducer> p(&task_runner);
  testing::InSequence s;
  EXPECT_CALL(p, SetAndroidProperty(kLazyTracedPerfPropertyName, "1"))
      .WillOnce(Return(true));
  EXPECT_CALL(p, SetAndroidProperty(kLazyTracedPerfPropertyName, ""))
      .WillOnce(InvokeWithoutArgs([&done]() {
        done();
        return true;
      }));
  p.SetupDataSource(1, cfg);
  p.StopDataSource(1);
  task_runner.RunUntilCheckpoint("done");
}

TEST(BuiltinProducerTest, LazyHeapprofdRefCount) {
  DataSourceConfig cfg;
  cfg.set_name(kHeapprofdDataSourceName);
  base::TestTaskRunner task_runner;
  auto done = task_runner.CreateCheckpoint("done");
  StrictMock<MockBuiltinProducer> p(&task_runner);
  testing::InSequence s;
  EXPECT_CALL(p, SetAndroidProperty(kLazyHeapprofdPropertyName, "1"))
      .WillRepeatedly(Return(true));
  p.SetupDataSource(1, cfg);
  p.SetupDataSource(2, cfg);
  p.StopDataSource(2);
  task_runner.RunUntilIdle();
  EXPECT_CALL(p, SetAndroidProperty(kLazyHeapprofdPropertyName, ""))
      .WillOnce(InvokeWithoutArgs([&done]() {
        done();
        return true;
      }));
  p.StopDataSource(1);
  task_runner.RunUntilCheckpoint("done");
}

TEST(BuiltinProducerTest, LazyHeapprofdNoFlap) {
  DataSourceConfig cfg;
  cfg.set_name(kHeapprofdDataSourceName);
  base::TestTaskRunner task_runner;
  auto done = task_runner.CreateCheckpoint("done");
  StrictMock<MockBuiltinProducer> p(&task_runner);
  testing::InSequence s;
  EXPECT_CALL(p, SetAndroidProperty(kLazyHeapprofdPropertyName, "1"))
      .WillRepeatedly(Return(true));
  p.SetupDataSource(1, cfg);
  p.StopDataSource(1);
  p.SetupDataSource(2, cfg);
  task_runner.RunUntilIdle();
  p.StopDataSource(2);
  EXPECT_CALL(p, SetAndroidProperty(kLazyHeapprofdPropertyName, ""))
      .WillOnce(InvokeWithoutArgs([&done]() {
        done();
        return true;
      }));
  task_runner.RunUntilCheckpoint("done");
}

TEST(BuiltinProducerTest, LazyRefCountsIndependent) {
  DataSourceConfig cfg_perf;
  cfg_perf.set_name(kTracedPerfDataSourceName);
  DataSourceConfig cfg_heap;
  cfg_heap.set_name(kHeapprofdDataSourceName);

  base::TestTaskRunner task_runner;
  StrictMock<MockBuiltinProducer> p(&task_runner);
  testing::InSequence s;

  // start one instance of both types of sources
  EXPECT_CALL(p, SetAndroidProperty(kLazyHeapprofdPropertyName, "1"))
      .WillOnce(Return(true));
  EXPECT_CALL(p, SetAndroidProperty(kLazyTracedPerfPropertyName, "1"))
      .WillOnce(Return(true));
  p.SetupDataSource(1, cfg_heap);
  p.SetupDataSource(2, cfg_perf);
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(&p);

  // stop heapprofd source
  EXPECT_CALL(p, SetAndroidProperty(kLazyHeapprofdPropertyName, ""))
      .WillOnce(Return(true));
  p.StopDataSource(1);
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(&p);

  // stop traced_perf source
  EXPECT_CALL(p, SetAndroidProperty(kLazyTracedPerfPropertyName, ""))
      .WillOnce(Return(true));
  p.StopDataSource(2);
  task_runner.RunUntilIdle();
  Mock::VerifyAndClearExpectations(&p);
}

class AndroidSdkSyspropGuardParameterizedTestFixture
    : public ::testing::TestWithParam<bool> {
 public:
  static constexpr int ITERATIONS = 3;
};

TEST_P(AndroidSdkSyspropGuardParameterizedTestFixture, SurfaceFlinger) {
  bool should_enable = GetParam();

  // Set SF flag in config
  protos::gen::AndroidSdkSyspropGuardConfig sysprop_guard;
  sysprop_guard.set_surfaceflinger_skia_track_events(should_enable);

  base::TestTaskRunner task_runner;
  StrictMock<MockBuiltinProducer> p(&task_runner);
  DataSourceConfig cfg;
  cfg.set_name(kAndroidSdkSyspropGuardDataSourceName);
  cfg.set_android_sdk_sysprop_guard_config_raw(
      sysprop_guard.SerializeAsString());

  // Expect SF props set
  EXPECT_CALL(p, SetAndroidProperty(kSurfaceFlingerSkiaBroadTracingPropertyName,
                                    "true"))
      .Times(should_enable ? 1 : 0)
      .WillOnce(Return(true));
  EXPECT_CALL(
      p, SetAndroidProperty(kSurfaceFlingerSkiaUsePerfettoPropertyName, "true"))
      .Times(should_enable ? 1 : 0)
      .WillOnce(Return(true));
  EXPECT_CALL(p, SetAndroidProperty(
                     kPerfettoSdkSyspropGuardGenerationPropertyName, "1"))
      .Times(should_enable ? 1 : 0)
      .WillOnce(Return(true));

  // Sysprops should only be set once given the same config
  for (int i = 0; i < ITERATIONS; i++) {
    p.SetupDataSource(1, cfg);
    p.StopDataSource(1);
    task_runner.RunUntilIdle();
  }
  Mock::VerifyAndClearExpectations(&p);
}

TEST_P(AndroidSdkSyspropGuardParameterizedTestFixture, HwuiGlobal) {
  bool should_enable = GetParam();

  // Set HWUI flag in config.
  // The package filter is left BLANK so this applies GLOBALLY.
  protos::gen::AndroidSdkSyspropGuardConfig sysprop_guard;
  sysprop_guard.set_hwui_skia_track_events(should_enable);

  base::TestTaskRunner task_runner;
  StrictMock<MockBuiltinProducer> p(&task_runner);
  DataSourceConfig cfg;
  cfg.set_name(kAndroidSdkSyspropGuardDataSourceName);
  cfg.set_android_sdk_sysprop_guard_config_raw(
      sysprop_guard.SerializeAsString());

  // Expect GLOBAL props set for HWUI.
  EXPECT_CALL(p, SetAndroidProperty(kHwuiSkiaBroadTracingPropertyName, "true"))
      .Times(should_enable ? 1 : 0)
      .WillOnce(Return(true));
  EXPECT_CALL(p, SetAndroidProperty(kHwuiSkiaUsePerfettoPropertyName, "true"))
      .Times(should_enable ? 1 : 0)
      .WillOnce(Return(true));
  EXPECT_CALL(p, SetAndroidProperty(
                     kPerfettoSdkSyspropGuardGenerationPropertyName, "1"))
      .Times(should_enable ? 1 : 0)
      .WillOnce(Return(true));

  // Sysprops should only be set once given the same config
  for (int i = 0; i < ITERATIONS; i++) {
    p.SetupDataSource(1, cfg);
    p.StopDataSource(1);
    task_runner.RunUntilIdle();
  }
  Mock::VerifyAndClearExpectations(&p);
}

TEST_P(AndroidSdkSyspropGuardParameterizedTestFixture, HwuiPackageFiltered) {
  bool should_enable = GetParam();

  std::string packages[] = {"test1", "com.android.systemui", "test3"};
  // Set HWUI flag in config. Package filter left blank.
  // The package filter is SET so this applies SELECTIVELY.
  protos::gen::AndroidSdkSyspropGuardConfig sysprop_guard;
  sysprop_guard.set_hwui_skia_track_events(should_enable);
  for (std::string package : packages) {
    sysprop_guard.add_hwui_package_name_filter(package);
  }

  base::TestTaskRunner task_runner;
  StrictMock<MockBuiltinProducer> p(&task_runner);
  DataSourceConfig cfg;
  cfg.set_name(kAndroidSdkSyspropGuardDataSourceName);
  cfg.set_android_sdk_sysprop_guard_config_raw(
      sysprop_guard.SerializeAsString());

  // Expect APP-SPECIFIC props set for HWUI.
  for (std::string package : packages) {
    EXPECT_CALL(
        p, SetAndroidProperty(kHwuiSkiaBroadTracingPropertyName +
                                  (kHwuiSkiaPropertyPackageSeparator + package),
                              "true"))
        .Times(should_enable ? 1 : 0)
        .WillOnce(Return(true));
    EXPECT_CALL(
        p, SetAndroidProperty(kHwuiSkiaUsePerfettoPropertyName +
                                  (kHwuiSkiaPropertyPackageSeparator + package),
                              "true"))
        .Times(should_enable ? 1 : 0)
        .WillOnce(Return(true));
  }
  EXPECT_CALL(p, SetAndroidProperty(
                     kPerfettoSdkSyspropGuardGenerationPropertyName, "1"))
      .Times(should_enable ? 1 : 0)
      .WillOnce(Return(true));

  // Sysprops should only be set once given the same config
  for (int i = 0; i < ITERATIONS; i++) {
    p.SetupDataSource(1, cfg);
    p.StopDataSource(1);
    task_runner.RunUntilIdle();
  }
  Mock::VerifyAndClearExpectations(&p);
}

INSTANTIATE_TEST_SUITE_P(BuiltinProducerTest,
                         AndroidSdkSyspropGuardParameterizedTestFixture,
                         testing::Values(true, false));

}  // namespace
}  // namespace perfetto
