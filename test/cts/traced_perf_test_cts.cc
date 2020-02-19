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

#include <stdlib.h>
#include <sys/types.h>

#include "perfetto/base/logging.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "test/cts/utils.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/profiling/perf_event_config.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

std::vector<protos::gen::TracePacket> ProfileSystemWide(std::string app_name) {
  base::TestTaskRunner task_runner;

  // (re)start the target app's main activity
  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 1000 /*ms*/);
  }
  StartAppActivity(app_name, "BusyWaitActivity", "target.app.running",
                   &task_runner,
                   /*delay_ms=*/100);
  task_runner.RunUntilCheckpoint("target.app.running", 1000 /*ms*/);

  // set up tracing
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.perf");
  ds_config->set_target_buffer(0);

  protos::gen::PerfEventConfig perf_config;

  perf_config.set_all_cpus(true);
  perf_config.set_sampling_frequency(10);  // Hz
  ds_config->set_perf_event_config_raw(perf_config.SerializeAsString());

  // start tracing
  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled(10000 /*ms*/);
  helper.ReadData();
  helper.WaitForReadData();

  return helper.trace();
}

void AssertHasSampledStacksForPid(std::vector<protos::gen::TracePacket> packets,
                                  int target_pid) {
  ASSERT_GT(packets.size(), 0u);

  int samples_found = 0;
  for (const auto& packet : packets) {
    if (!packet.has_perf_sample())
      continue;

    EXPECT_GT(packet.timestamp(), 0u) << "all samples should have a timestamp";
    const auto& sample = packet.perf_sample();
    if (sample.pid() != static_cast<uint32_t>(target_pid))
      continue;

    // TODO(rsavitski): include |sample.has_sample_skipped_reason| once that is
    // merged.
    if (sample.has_kernel_records_lost())
      continue;

    // A full sample
    EXPECT_GT(sample.tid(), 0u);
    EXPECT_GT(sample.callstack_iid(), 0u);
    samples_found += 1;
  }
  EXPECT_GT(samples_found, 0);
}

void AssertNoStacksForPid(std::vector<protos::gen::TracePacket> packets,
                          int target_pid) {
  for (const auto& packet : packets) {
    if (packet.perf_sample().pid() == static_cast<uint32_t>(target_pid)) {
      EXPECT_EQ(packet.perf_sample().callstack_iid(), 0u);
    }
  }
}

TEST(TracedPerfCtsTest, SystemWideDebuggableApp) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  const auto& packets = ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  AssertHasSampledStacksForPid(packets, app_pid);
  StopApp(app_name);
}

TEST(TracedPerfCtsTest, SystemWideProfileableApp) {
  std::string app_name = "android.perfetto.cts.app.profileable";
  const auto& packets = ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  AssertHasSampledStacksForPid(packets, app_pid);
  StopApp(app_name);
}

TEST(TracedPerfCtsTest, SystemWideReleaseApp) {
  std::string app_name = "android.perfetto.cts.app.release";
  const auto& packets = ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  if (IsDebuggableBuild())
    AssertHasSampledStacksForPid(packets, app_pid);
  else
    AssertNoStacksForPid(packets, app_pid);

  StopApp(app_name);
}

}  // namespace
}  // namespace perfetto
