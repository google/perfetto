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

#include <stdlib.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <random>

#include "perfetto/base/logging.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "test/android_test_utils.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/process_stats/process_stats_config.gen.h"
#include "protos/perfetto/config/profiling/heapprofd_config.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

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
// Activity that runs a java thread that repeatedly constructs small java
// objects.
static char kJavaAllocActivity[] = "JavaAllocActivity";

std::string RandomSessionName() {
  std::random_device rd;
  std::default_random_engine generator(rd());
  std::uniform_int_distribution<> distribution('a', 'z');

  constexpr size_t kSessionNameLen = 20;
  std::string result(kSessionNameLen, '\0');
  for (size_t i = 0; i < kSessionNameLen; ++i)
    result[i] = static_cast<char>(distribution(generator));
  return result;
}

// Starts the activity `activity` of the app `app_name` and later starts
// recording a trace with the allocations in `heap_names`.
//
// `heap_names` is a list of the heap names whose allocations will be recorded.
// An empty list means that only the allocations in the default malloc heap
// ("libc.malloc") are recorded.
//
// Returns the recorded trace.
std::vector<protos::gen::TracePacket> ProfileRuntime(
    const std::string& app_name,
    const std::string& activity,
    const std::vector<std::string>& heap_names) {
  base::TestTaskRunner task_runner;

  // (re)start the target app's main activity
  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 10000 /*ms*/);
  }
  StartAppActivity(app_name, activity, "target.app.running", &task_runner,
                   /*delay_ms=*/100);
  task_runner.RunUntilCheckpoint("target.app.running", 10000 /*ms*/);

  // set up tracing
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(4000);
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(kTestSamplingInterval);
  heapprofd_config.add_process_cmdline(app_name.c_str());
  heapprofd_config.set_block_client(true);
  heapprofd_config.set_all(false);
  for (const std::string& heap_name : heap_names) {
    heapprofd_config.add_heaps(heap_name);
  }
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  // start tracing
  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();
  helper.ReadData();
  helper.WaitForReadData();

  return helper.trace();
}

// Starts recording a trace with the allocations in `heap_names` and later
// starts the activity `activity` of the app `app_name`
//
// `heap_names` is a list of the heap names whose allocations will be recorded.
// An empty list means that only the allocation in the default malloc heap
// ("libc.malloc") are recorded.
//
// Returns the recorded trace.
std::vector<protos::gen::TracePacket> ProfileStartup(
    const std::string& app_name,
    const std::string& activity,
    const std::vector<std::string>& heap_names,
    const bool enable_extra_guardrails = false) {
  base::TestTaskRunner task_runner;

  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 10000 /*ms*/);
  }

  // set up tracing
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(4000);
  trace_config.set_enable_extra_guardrails(enable_extra_guardrails);
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(kTestSamplingInterval);
  heapprofd_config.add_process_cmdline(app_name.c_str());
  heapprofd_config.set_block_client(true);
  heapprofd_config.set_all(false);
  for (const std::string& heap_name : heap_names) {
    heapprofd_config.add_heaps(heap_name);
  }
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  // start tracing
  helper.StartTracing(trace_config);

  // start app
  StartAppActivity(app_name, activity, "target.app.running", &task_runner,
                   /*delay_ms=*/100);
  task_runner.RunUntilCheckpoint("target.app.running", 10000 /*ms*/);

  helper.WaitForTracingDisabled();
  helper.ReadData();
  helper.WaitForReadData();

  return helper.trace();
}

// Check that `packets` contain some allocations performed by kMallocActivity.
void AssertExpectedMallocsPresent(
    const std::vector<protos::gen::TracePacket>& packets) {
  ASSERT_GT(packets.size(), 0u);

  // TODO(rsavitski): assert particular stack frames once we clarify the
  // expected behaviour of unwinding native libs within an apk.
  // Until then, look for an allocation that is a multiple of the expected
  // allocation size.
  bool found_alloc = false;
  bool found_proc_dump = false;
  for (const auto& packet : packets) {
    for (const auto& proc_dump : packet.profile_packet().process_dumps()) {
      found_proc_dump = true;
      for (const auto& sample : proc_dump.samples()) {
        if (sample.self_allocated() > 0 &&
            sample.self_allocated() % kExpectedIndividualAllocSz == 0) {
          found_alloc = true;

          EXPECT_TRUE(sample.self_freed() > 0 &&
                      sample.self_freed() % kExpectedIndividualAllocSz == 0)
              << "self_freed: " << sample.self_freed();
        }
      }
    }
  }
  ASSERT_TRUE(found_proc_dump);
  ASSERT_TRUE(found_alloc);
}

void AssertHasSampledAllocs(
    const std::vector<protos::gen::TracePacket>& packets) {
  ASSERT_GT(packets.size(), 0u);

  bool found_alloc = false;
  bool found_proc_dump = false;
  for (const auto& packet : packets) {
    for (const auto& proc_dump : packet.profile_packet().process_dumps()) {
      found_proc_dump = true;
      for (const auto& sample : proc_dump.samples()) {
        if (sample.self_allocated() > 0) {
          found_alloc = true;
        }
      }
    }
  }
  ASSERT_TRUE(found_proc_dump);
  ASSERT_TRUE(found_alloc);
}

void AssertNoProfileContents(
    const std::vector<protos::gen::TracePacket>& packets) {
  // If profile packets are present, they must be empty.
  for (const auto& packet : packets) {
    ASSERT_EQ(packet.profile_packet().process_dumps_size(), 0);
  }
}

TEST(HeapprofdCtsTest, DebuggableAppRuntime) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets =
      ProfileRuntime(app_name, kMallocActivity, /*heap_names=*/{});
  AssertExpectedMallocsPresent(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, DebuggableAppStartup) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets =
      ProfileStartup(app_name, kMallocActivity, /*heap_names=*/{});
  AssertExpectedMallocsPresent(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ProfileableAppRuntime) {
  std::string app_name = "android.perfetto.cts.app.profileable";
  const auto& packets =
      ProfileRuntime(app_name, kMallocActivity, /*heap_names=*/{});
  AssertExpectedMallocsPresent(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ProfileableAppStartup) {
  std::string app_name = "android.perfetto.cts.app.profileable";
  const auto& packets =
      ProfileStartup(app_name, kMallocActivity, /*heap_names=*/{});
  AssertExpectedMallocsPresent(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ReleaseAppRuntime) {
  std::string app_name = "android.perfetto.cts.app.release";
  const auto& packets =
      ProfileRuntime(app_name, kMallocActivity, /*heap_names=*/{});

  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertExpectedMallocsPresent(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, ReleaseAppStartup) {
  std::string app_name = "android.perfetto.cts.app.release";
  const auto& packets =
      ProfileStartup(app_name, kMallocActivity, /*heap_names=*/{});

  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertExpectedMallocsPresent(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, NonProfileableAppRuntime) {
  std::string app_name = "android.perfetto.cts.app.nonprofileable";
  const auto& packets =
      ProfileRuntime(app_name, kMallocActivity, /*heap_names=*/{});
  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertExpectedMallocsPresent(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, NonProfileableAppStartup) {
  std::string app_name = "android.perfetto.cts.app.nonprofileable";
  const auto& packets =
      ProfileStartup(app_name, kMallocActivity, /*heap_names=*/{});
  if (IsUserBuild())
    AssertNoProfileContents(packets);
  else
    AssertExpectedMallocsPresent(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, JavaHeapRuntime) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = ProfileRuntime(app_name, kJavaAllocActivity,
                                       /*heap_names=*/{"com.android.art"});
  AssertHasSampledAllocs(packets);
  StopApp(app_name);
}

TEST(HeapprofdCtsTest, JavaHeapStartup) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = ProfileStartup(app_name, kJavaAllocActivity,
                                       /*heap_names=*/{"com.android.art"});
  AssertHasSampledAllocs(packets);
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
