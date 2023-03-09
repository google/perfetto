/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/android_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "test/android_test_utils.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/common/perf_events.gen.h"
#include "protos/perfetto/config/process_stats/process_stats_config.gen.h"
#include "protos/perfetto/config/profiling/perf_event_config.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

// Skip these tests if the device in question doesn't have the necessary kernel
// LSM hooks in perf_event_open. This comes up when a device with an older
// kernel upgrades to R.
bool HasPerfLsmHooks() {
  return base::GetAndroidProp("sys.init.perf_lsm_hooks") == "1";
}

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

std::vector<protos::gen::TracePacket> CollectTrace(
    base::TestTaskRunner* task_runner,
    const TraceConfig& trace_config) {
  TestHelper helper(task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled(15000 /*ms*/);
  helper.ReadData();
  helper.WaitForReadData();
  return helper.trace();
}

std::vector<protos::gen::TracePacket> ProfileSystemWide(std::string app_name) {
  base::TestTaskRunner task_runner;

  // (re)start the target app's main activity
  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 10000 /*ms*/);
  }
  StartAppActivity(app_name, "BusyWaitActivity", "target.app.running",
                   &task_runner,
                   /*delay_ms=*/100);
  task_runner.RunUntilCheckpoint("target.app.running", 10000 /*ms*/);

  // build config
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(20 * 1024);
  trace_config.set_duration_ms(3000);
  trace_config.set_data_source_stop_timeout_ms(8000);
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.perf");
  ds_config->set_target_buffer(0);

  protos::gen::PerfEventConfig perf_config;
  perf_config.set_all_cpus(true);
  perf_config.set_sampling_frequency(10);  // Hz
  ds_config->set_perf_event_config_raw(perf_config.SerializeAsString());

  return CollectTrace(&task_runner, trace_config);
}

void AssertHasSampledStacksForPid(std::vector<protos::gen::TracePacket> packets,
                                  int pid) {
  uint32_t target_pid = static_cast<uint32_t>(pid);
  ASSERT_GT(packets.size(), 0u);

  int total_perf_packets = 0;
  int lost_records_packets = 0;
  int full_samples = 0;
  int target_samples = 0;
  int target_skipped_samples = 0;
  for (const auto& packet : packets) {
    if (!packet.has_perf_sample())
      continue;

    total_perf_packets++;
    EXPECT_GT(packet.timestamp(), 0u) << "all packets should have a timestamp";
    const auto& sample = packet.perf_sample();
    if (sample.has_kernel_records_lost()) {
      lost_records_packets++;
      continue;
    }
    if (sample.has_sample_skipped_reason()) {
      if (sample.pid() == target_pid)
        target_skipped_samples++;
      continue;
    }

    full_samples++;
    EXPECT_GT(sample.tid(), 0u);
    EXPECT_GT(sample.callstack_iid(), 0u);

    if (sample.pid() == target_pid)
      target_samples++;
  }

  // log summary even if successful
  base::StackString<512> log(
      "target_pid: %d, packets.size(): %zu, total_perf_packets: %d, "
      "full_samples: %d, lost_records_packets: %d, target_skipped_samples: %d",
      target_pid, packets.size(), total_perf_packets, full_samples,
      lost_records_packets, target_skipped_samples);
  PERFETTO_LOG("%s", log.c_str());

  EXPECT_GT(target_samples, 0) << log.c_str() << "\n";
}

void AssertNoStacksForPid(std::vector<protos::gen::TracePacket> packets,
                          int pid) {
  uint32_t target_pid = static_cast<uint32_t>(pid);
  // The process can still be sampled, but the stacks should be discarded
  // without unwinding.
  for (const auto& packet : packets) {
    if (packet.perf_sample().pid() == target_pid) {
      EXPECT_EQ(packet.perf_sample().callstack_iid(), 0u);
      EXPECT_TRUE(packet.perf_sample().has_sample_skipped_reason());
    }
  }
}

TEST(TracedPerfCtsTest, SystemWideDebuggableApp) {
  if (!HasPerfLsmHooks())
    GTEST_SKIP() << "skipped due to lack of perf_event_open LSM hooks";

  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  AssertHasSampledStacksForPid(packets, app_pid);
  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name);
}

TEST(TracedPerfCtsTest, SystemWideProfileableApp) {
  if (!HasPerfLsmHooks())
    GTEST_SKIP() << "skipped due to lack of perf_event_open LSM hooks";

  std::string app_name = "android.perfetto.cts.app.profileable";
  const auto& packets = ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  AssertHasSampledStacksForPid(packets, app_pid);
  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name);
}

TEST(TracedPerfCtsTest, SystemWideNonProfileableApp) {
  if (!HasPerfLsmHooks())
    GTEST_SKIP() << "skipped due to lack of perf_event_open LSM hooks";

  std::string app_name = "android.perfetto.cts.app.nonprofileable";
  const auto& packets = ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  if (!IsUserBuild())
    AssertHasSampledStacksForPid(packets, app_pid);
  else
    AssertNoStacksForPid(packets, app_pid);
  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name);
}

TEST(TracedPerfCtsTest, SystemWideReleaseApp) {
  if (!HasPerfLsmHooks())
    GTEST_SKIP() << "skipped due to lack of perf_event_open LSM hooks";

  std::string app_name = "android.perfetto.cts.app.release";
  const auto& packets = ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  if (!IsUserBuild())
    AssertHasSampledStacksForPid(packets, app_pid);
  else
    AssertNoStacksForPid(packets, app_pid);

  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name);
}

// Loads a platform process with work (we use traced_probes which runs as
// AID_NOBODY), and profiles it.
TEST(TracedPerfCtsTest, ProfilePlatformProcess) {
  if (!HasPerfLsmHooks())
    GTEST_SKIP() << "skipped due to lack of perf_event_open LSM hooks";

  int target_pid = PidForProcessName("/system/bin/traced_probes");
  ASSERT_GT(target_pid, 0) << "failed to find pid for target process";

  // Construct config.
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(64);
  trace_config.add_buffers()->set_size_kb(1024);
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

  // capture callstacks of traced_probes descheduling
  protos::gen::PerfEventConfig perf_config;
  auto* timebase = perf_config.mutable_timebase();
  timebase->set_counter(protos::gen::PerfEvents::SW_CONTEXT_SWITCHES);
  timebase->set_period(1);
  auto* callstacks = perf_config.mutable_callstack_sampling();
  auto* scope = callstacks->mutable_scope();
  scope->add_target_pid(target_pid);

  ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.perf");
  ds_config->set_target_buffer(1);
  ds_config->set_perf_event_config_raw(perf_config.SerializeAsString());

  // Collect trace.
  base::TestTaskRunner task_runner;
  auto packets = CollectTrace(&task_runner, trace_config);

  int target_pid_after = PidForProcessName("/system/bin/traced_probes");
  ASSERT_EQ(target_pid, target_pid_after) << "traced_probes died during test";

  if (!IsUserBuild())
    AssertHasSampledStacksForPid(packets, target_pid);
  else
    AssertNoStacksForPid(packets, target_pid);
}

}  // namespace
}  // namespace perfetto
