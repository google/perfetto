
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
#include <sys/types.h>
#include <sys/wait.h>

#include "gtest/gtest.h"
#include "perfetto/base/logging.h"
#include "src/base/test/test_task_runner.h"
#include "test/test_helper.h"

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

// note: cannot use gtest macros due to return type
bool IsAppRunning(const std::string& name) {
  std::string cmd = "pgrep -f " + name;
  int retcode = system(cmd.c_str());
  PERFETTO_CHECK(retcode >= 0);
  int exit_status = WEXITSTATUS(retcode);
  if (exit_status == 0)
    return true;
  if (exit_status == 1)
    return false;
  PERFETTO_FATAL("unexpected exit status from system(pgrep): %d", exit_status);
}

// invokes |callback| once the target app is in the desired state
void PollRunState(bool desired_run_state,
                  base::TestTaskRunner* task_runner,
                  const std::string& name,
                  std::function<void()> callback) {
  bool app_running = IsAppRunning(name);
  if (app_running == desired_run_state) {
    callback();
    return;
  }
  task_runner->PostTask([desired_run_state, task_runner, name, callback] {
    PollRunState(desired_run_state, task_runner, name, std::move(callback));
  });
}

void StartAppActivity(const std::string& app_name,
                      const std::string& checkpoint_name,
                      base::TestTaskRunner* task_runner,
                      int delay_ms = 1) {
  std::string start_cmd = "am start " + app_name + "/.MainActivity";
  int status = system(start_cmd.c_str());
  ASSERT_TRUE(status >= 0 && WEXITSTATUS(status) == 0) << "status: " << status;

  bool desired_run_state = true;
  const auto checkpoint = task_runner->CreateCheckpoint(checkpoint_name);
  task_runner->PostDelayedTask(
      [desired_run_state, task_runner, app_name, checkpoint] {
        PollRunState(desired_run_state, task_runner, app_name,
                     std::move(checkpoint));
      },
      delay_ms);
}

void StopApp(const std::string& app_name,
             const std::string& checkpoint_name,
             base::TestTaskRunner* task_runner) {
  std::string stop_cmd = "am force-stop " + app_name;
  int status = system(stop_cmd.c_str());
  ASSERT_TRUE(status >= 0 && WEXITSTATUS(status) == 0) << "status: " << status;

  bool desired_run_state = false;
  auto checkpoint = task_runner->CreateCheckpoint(checkpoint_name);
  task_runner->PostTask([desired_run_state, task_runner, app_name, checkpoint] {
    PollRunState(desired_run_state, task_runner, app_name,
                 std::move(checkpoint));
  });
}

void TestAppRuntime(std::string app_name) {
  base::TestTaskRunner task_runner;

  // (re)start the target app's main activity
  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 1000 /*ms*/);
  }
  StartAppActivity(app_name, "target.app.running", &task_runner,
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
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  auto* heapprofd_config = ds_config->mutable_heapprofd_config();
  heapprofd_config->set_sampling_interval_bytes(kTestSamplingInterval);
  *heapprofd_config->add_process_cmdline() = app_name.c_str();
  heapprofd_config->set_all(false);

  // start tracing
  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled(4000 /*ms*/);
  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0);

  // TODO(rsavitski): assert particular stack frames once we clarify the
  // expected behaviour of unwinding native libs within an apk.
  // Until then, look for an allocation that is a multiple of the expected
  // allocation size.
  bool found_alloc = false;
  for (const auto& packet : packets) {
    for (const auto& proc_dump : packet.profile_packet().process_dumps()) {
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
  ASSERT_TRUE(found_alloc);

  std::string stop_cmd = "am force-stop " + app_name;
  system(stop_cmd.c_str());
}

void TestAppStartup(std::string app_name) {
  base::TestTaskRunner task_runner;

  if (IsAppRunning(app_name)) {
    StopApp(app_name, "old.app.stopped", &task_runner);
    task_runner.RunUntilCheckpoint("old.app.stopped", 1000 /*ms*/);
  }

  // set up tracing
  TestHelper helper(&task_runner);
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(4000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  auto* heapprofd_config = ds_config->mutable_heapprofd_config();
  heapprofd_config->set_sampling_interval_bytes(kTestSamplingInterval);
  *heapprofd_config->add_process_cmdline() = app_name.c_str();
  heapprofd_config->set_all(false);

  // start tracing
  helper.StartTracing(trace_config);

  // start app
  StartAppActivity(app_name, "target.app.running", &task_runner,
                   /*delay_ms=*/100);
  task_runner.RunUntilCheckpoint("target.app.running", 2000 /*ms*/);

  helper.WaitForTracingDisabled(8000 /*ms*/);
  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0);

  // TODO(rsavitski): assert particular stack frames once we clarify the
  // expected behaviour of unwinding native libs within an apk.
  // Until then, look for an allocation that is a multiple of the expected
  // allocation size.
  bool found_alloc = false;
  for (const auto& packet : packets) {
    for (const auto& proc_dump : packet.profile_packet().process_dumps()) {
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
  ASSERT_TRUE(found_alloc);

  std::string stop_cmd = "am force-stop " + app_name;
  system(stop_cmd.c_str());
}

TEST(HeapprofdCtsTest, DebuggableAppRuntime) {
  TestAppRuntime("android.perfetto.debuggable.app");
}

TEST(HeapprofdCtsTest, DebuggableAppStartup) {
  TestAppStartup("android.perfetto.debuggable.app");
}

}  // namespace
}  // namespace perfetto
