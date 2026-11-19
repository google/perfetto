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
#include "protos/perfetto/trace/interned_data/interned_data.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

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

class TracedPerfCtsTest : public ::testing::Test {
 protected:
  // Records a trace with |trace_config|, which is then held by |helper_|.
  void CollectTrace(const TraceConfig& trace_config) {
    helper_.ConnectConsumer();
    helper_.WaitForConsumerConnect();

    helper_.StartTracing(trace_config);
    helper_.WaitForTracingDisabled(15000 /*ms*/);
    helper_.ReadData();
    helper_.WaitForReadData();
    // The helper has its own copy of the trace, release the session in traced.
    helper_.FreeBuffers();
  }

  void ProfileSystemWide(std::string app_name);

  base::TestTaskRunner task_runner_;
  TestHelper helper_{&task_runner_};
};

void TracedPerfCtsTest::ProfileSystemWide(std::string app_name) {
  // (re)start the target app's main activity
  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner_);
    task_runner_.RunUntilCheckpoint("old.app.stopped", 10000 /*ms*/);
  }
  StartAppActivity(app_name, "BusyWaitActivity", "target.app.running",
                   &task_runner_,
                   /*delay_ms=*/100);
  task_runner_.RunUntilCheckpoint("target.app.running", 10000 /*ms*/);

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

  CollectTrace(trace_config);
}

void AssertHasSampledStacksForPid(const TestHelper& helper, int pid) {
  uint32_t target_pid = static_cast<uint32_t>(pid);

  size_t total_packets = 0;
  int total_perf_packets = 0;
  int lost_records_packets = 0;
  int full_samples = 0;
  int target_samples = 0;
  int target_skipped_samples = 0;
  helper.ForEachPacket([&](const protos::gen::TracePacket& packet) {
    total_packets++;
    if (!packet.has_perf_sample())
      return;

    total_perf_packets++;
    EXPECT_GT(packet.timestamp(), 0u) << "all packets should have a timestamp";
    const auto& sample = packet.perf_sample();
    if (sample.has_kernel_records_lost()) {
      lost_records_packets++;
      return;
    }
    if (sample.has_sample_skipped_reason()) {
      if (sample.pid() == target_pid)
        target_skipped_samples++;
      return;
    }

    full_samples++;
    EXPECT_GT(sample.tid(), 0u);
    EXPECT_GT(sample.callstack_iid(), 0u);

    if (sample.pid() == target_pid)
      target_samples++;
  });
  ASSERT_GT(total_packets, 0u);

  // log summary even if successful
  base::StackString<512> log(
      "target_pid: %d, total_packets: %zu, total_perf_packets: %d, "
      "full_samples: %d, lost_records_packets: %d, target_skipped_samples: %d",
      target_pid, total_packets, total_perf_packets, full_samples,
      lost_records_packets, target_skipped_samples);
  PERFETTO_LOG("%s", log.c_str());

  EXPECT_GT(target_samples, 0) << log.c_str() << "\n";
}

void AssertNoStacksForPid(const TestHelper& helper, int pid) {
  uint32_t target_pid = static_cast<uint32_t>(pid);
  // The process can still be sampled, but the stacks should be discarded
  // without unwinding.
  helper.ForEachPacket([&](const protos::gen::TracePacket& packet) {
    if (packet.perf_sample().pid() == target_pid) {
      EXPECT_EQ(packet.perf_sample().callstack_iid(), 0u);
      EXPECT_TRUE(packet.perf_sample().has_sample_skipped_reason());
    }
  });
}

TEST_F(TracedPerfCtsTest, SystemWideDebuggableApp) {
  std::string app_name = "android.perfetto.cts.app.debuggable";
  ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  AssertHasSampledStacksForPid(helper_, app_pid);
  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name);
}

TEST_F(TracedPerfCtsTest, SystemWideProfileableApp) {
  std::string app_name = "android.perfetto.cts.app.profileable";
  ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  AssertHasSampledStacksForPid(helper_, app_pid);
  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name);
}

TEST_F(TracedPerfCtsTest, SystemWideNonProfileableApp) {
  std::string app_name = "android.perfetto.cts.app.nonprofileable";
  ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  if (!IsUserBuild())
    AssertHasSampledStacksForPid(helper_, app_pid);
  else
    AssertNoStacksForPid(helper_, app_pid);
  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name);
}

TEST_F(TracedPerfCtsTest, SystemWideReleaseApp) {
  std::string app_name = "android.perfetto.cts.app.release";
  ProfileSystemWide(app_name);
  int app_pid = PidForProcessName(app_name);
  ASSERT_GT(app_pid, 0) << "failed to find pid for target process";

  if (!IsUserBuild())
    AssertHasSampledStacksForPid(helper_, app_pid);
  else
    AssertNoStacksForPid(helper_, app_pid);

  PERFETTO_CHECK(IsAppRunning(app_name));
  StopApp(app_name);
}

// Loads a platform process with work (we use traced_probes which runs as
// AID_NOBODY), and profiles it.
TEST_F(TracedPerfCtsTest, ProfilePlatformProcess) {
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
  CollectTrace(trace_config);

  int target_pid_after = PidForProcessName("/system/bin/traced_probes");
  ASSERT_EQ(target_pid, target_pid_after) << "traced_probes died during test";

  if (!IsUserBuild())
    AssertHasSampledStacksForPid(helper_, target_pid);
  else
    AssertNoStacksForPid(helper_, target_pid);
}

TEST_F(TracedPerfCtsTest, ProfileKernelCallstack) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(3000);
  trace_config.set_data_source_stop_timeout_ms(8000);
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  // Capture context switch callstacks from the rest of the device, as they have
  // predictable function names for the test to assert.
  protos::gen::PerfEventConfig perf_config;
  auto* timebase = perf_config.mutable_timebase();
  // We only need a few samples, and the kernel will record an early sample per
  // core even at low "frequency" values.
  timebase->set_frequency(1);
  auto* tracepoint = timebase->mutable_tracepoint();
  tracepoint->set_name("sched:sched_switch");

  auto* callstacks = perf_config.mutable_callstack_sampling();
  callstacks->set_kernel_frames(true);
  callstacks->set_user_frames(protos::gen::PerfEventConfig::UNWIND_SKIP);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.perf");
  ds_config->set_perf_event_config_raw(perf_config.SerializeAsString());

  // Collect trace.
  CollectTrace(trace_config);

  // Assert that we're seeing the symbolised scheduling functions, while
  // checking that raw kernel addresses (which would reveal the KASLR offset)
  // don't leak into the trace:
  // * User frames are not unwound, so all frames are kernel frames, which are
  //   emitted without addresses.
  // * Interning ids are counters that persist for the lifetime of traced_perf,
  //   so they grow with every session it serves. But they should still be far
  //   below kernel addresses, which are in the upper half of the address space
  //   on 64-bit kernels, and above 0x80000000 with the standard 32-bit memory
  //   splits.
  constexpr uint64_t kMinKernelAddress = 0x80000000;
  size_t leaked_addresses = 0;
  size_t address_like_iids = 0;

  size_t fname_count = 0;
  bool found_schedule = false;
  helper_.ForEachPacket([&](const protos::gen::TracePacket& packet) {
    for (const auto& fname : packet.interned_data().function_names()) {
      fname_count++;
      // Match any "*schedule*" function (e.g. __schedule, schedule,
      // preempt_schedule, preempt_schedule_notrace, preempt_schedule_common,
      // preempt_schedule_irq, schedule_idle) as well as compiler-suffixed
      // variants when __schedule is inlined.
      if (fname.str().find("schedule") != std::string::npos) {
        found_schedule = true;
      }
      if (fname.iid() >= kMinKernelAddress) {
        address_like_iids++;
      }
    }
    for (const auto& mapping : packet.interned_data().mappings()) {
      if (mapping.start() || mapping.end()) {
        leaked_addresses++;
      }
    }
    for (const auto& frame : packet.interned_data().frames()) {
      if (frame.rel_pc()) {
        leaked_addresses++;
      }
      if (frame.iid() >= kMinKernelAddress) {
        address_like_iids++;
      }
    }
    if (packet.has_perf_sample() &&
        packet.perf_sample().callstack_iid() >= kMinKernelAddress) {
      address_like_iids++;
    }
  });

  // Dumps one callstack per line. Example:
  // [1] __schedule <- schedule_idle <- cpu_startup_entry <- rest_init ...
  // [2] schedule <- perf_mon_task <- kthread <- ret_from_fork
  auto callstacks_to_string = [this]() {
    std::string ret;
    int idx = 0;
    helper_.ForEachPacket([&](const protos::gen::TracePacket& packet) {
      bool first = true;
      for (const auto& fname : packet.interned_data().function_names()) {
        if (fname.str().empty())
          continue;
        if (first) {
          ret += "\n  [" + std::to_string(++idx) + "] " + fname.str();
          first = false;
        } else {
          ret += " <- " + fname.str();
        }
      }
    });
    return ret;
  };

  EXPECT_TRUE(found_schedule)
      << "Failed to find a scheduling kernel function symbol name in the "
         "profile, total functions seen: "
      << fname_count << ", callstacks:" << callstacks_to_string();

  EXPECT_EQ(leaked_addresses, 0u) << "Kernel frames with addresses.";
  EXPECT_EQ(address_like_iids, 0u)
      << "Interning ids in the kernel address range.";
}

}  // namespace
}  // namespace perfetto
