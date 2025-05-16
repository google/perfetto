/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <cinttypes>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "src/base/test/test_task_runner.h"
#include "test/android_test_utils.h"
#include "test/cts/heapprofd_test_helper.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/process_stats/process_stats_config.gen.h"
#include "protos/perfetto/config/profiling/heapprofd_config.gen.h"

namespace perfetto {
namespace {

// Size of individual (repeated) allocations done by the test apps (must be kept
// in sync with their sources).
constexpr uint64_t kTestSamplingInterval = 4096;
constexpr uint64_t kExpectedIndividualAllocSz = 4153;
// Tests rely on the sampling behaviour where allocations larger than the
// sampling interval are recorded at their actual size.
static_assert(kExpectedIndividualAllocSz > kTestSamplingInterval,
              "kTestSamplingInterval invalid");

// Activity that runs a JNI thread that repeatedly calls
// malloc(kExpectedIndividualAllocSz).
static char kMallocActivity[] = "MainActivity";

// Note that tests using AssertExpectedMallocsPresent are relying on the fact
// that callstacks can provide information about which function called
// malloc/free. This is not the case for apps running with native_bridge.
//
// For these there are 2 different stacks: native one - visible to perfetto;
// and another one for emulated architecture. Perfetto currently does not
// detect/report stack for emulated apps and the native stacktrace looks
// similar for all memory allocations initiated from emulated code.
//
// Since having perfetto handle second callstack is not a trivial change
// we disable these tests if ran on emulated architectures.
//
// See also http://b/411111586.
#define SKIP_WITH_NATIVE_BRIDGE  \
  if (RunningWithNativeBridge()) \
  GTEST_SKIP()

TEST(HeapprofdCtsTest, DebuggableAppRuntime) {
  SKIP_WITH_NATIVE_BRIDGE;  // http://b/411111586
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = ProfileRuntime(
      app_name, kMallocActivity, kTestSamplingInterval, /*heap_names=*/{});
  AssertExpectedMallocsPresent(kExpectedIndividualAllocSz, packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, DebuggableAppStartup) {
  SKIP_WITH_NATIVE_BRIDGE;  // http://b/411111586
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = ProfileStartup(
      app_name, kMallocActivity, kTestSamplingInterval, /*heap_names=*/{});
  AssertExpectedMallocsPresent(kExpectedIndividualAllocSz, packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ProfileableAppRuntime) {
  SKIP_WITH_NATIVE_BRIDGE;  // http://b/411111586
  std::string app_name = "android.perfetto.cts.app.profileable";
  const auto& packets = ProfileRuntime(
      app_name, kMallocActivity, kTestSamplingInterval, /*heap_names=*/{});
  AssertExpectedMallocsPresent(kExpectedIndividualAllocSz, packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ProfileableAppStartup) {
  SKIP_WITH_NATIVE_BRIDGE;  // http://b/411111586
  std::string app_name = "android.perfetto.cts.app.profileable";
  const auto& packets = ProfileStartup(
      app_name, kMallocActivity, kTestSamplingInterval, /*heap_names=*/{});
  AssertExpectedMallocsPresent(kExpectedIndividualAllocSz, packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ReleaseAppRuntime) {
  if (!IsUserBuild()) {
    SKIP_WITH_NATIVE_BRIDGE;  // http://b/411111586
  }
  std::string app_name = "android.perfetto.cts.app.release";
  const auto& packets = ProfileRuntime(
      app_name, kMallocActivity, kTestSamplingInterval, /*heap_names=*/{});

  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertExpectedMallocsPresent(kExpectedIndividualAllocSz, packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ReleaseAppStartup) {
  if (!IsUserBuild()) {
    SKIP_WITH_NATIVE_BRIDGE;  // http://b/411111586
  }
  std::string app_name = "android.perfetto.cts.app.release";
  const auto& packets = ProfileStartup(
      app_name, kMallocActivity, kTestSamplingInterval, /*heap_names=*/{});

  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertExpectedMallocsPresent(kExpectedIndividualAllocSz, packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, NonProfileableAppRuntime) {
  if (!IsUserBuild()) {
    SKIP_WITH_NATIVE_BRIDGE;  // http://b/411111586
  }
  std::string app_name = "android.perfetto.cts.app.nonprofileable";
  const auto& packets = ProfileRuntime(
      app_name, kMallocActivity, kTestSamplingInterval, /*heap_names=*/{});
  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertExpectedMallocsPresent(kExpectedIndividualAllocSz, packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, NonProfileableAppStartup) {
  if (!IsUserBuild()) {
    SKIP_WITH_NATIVE_BRIDGE;  // http://b/411111586
  }
  std::string app_name = "android.perfetto.cts.app.nonprofileable";
  const auto& packets = ProfileStartup(
      app_name, kMallocActivity, kTestSamplingInterval, /*heap_names=*/{});
  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertExpectedMallocsPresent(kExpectedIndividualAllocSz, packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ProfilePlatformProcess) {
  int target_pid = PidForProcessName("/system/bin/traced_probes");
  ASSERT_GT(target_pid, 0) << "failed to find pid for target process";

  // Construct config.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(20 * 1024);
  trace_config.set_duration_ms(3000);
  trace_config.set_data_source_stop_timeout_ms(8000);
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  // process.stats to cause work in traced_probes
  protos::gen::ProcessStatsConfig ps_config;
  ps_config.set_proc_stats_poll_ms(100);
  ps_config.set_record_thread_names(true);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.process_stats");
  ds_config->set_process_stats_config_raw(ps_config.SerializeAsString());

  // profile native heap of traced_probes
  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(kTestSamplingInterval);
  heapprofd_config.add_pid(static_cast<uint64_t>(target_pid));
  heapprofd_config.set_block_client(true);

  ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  // Collect trace.
  base::TestTaskRunner task_runner;
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled(15000 /*ms*/);
  helper.ReadData();
  helper.WaitForReadData();
  auto packets = helper.trace();

  int target_pid_after = PidForProcessName("/system/bin/traced_probes");
  ASSERT_EQ(target_pid, target_pid_after) << "traced_probes died during test";

  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertHasSampledAllocs(packets);
}

}  // namespace
}  // namespace perfetto
