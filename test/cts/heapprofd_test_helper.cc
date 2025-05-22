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

#include <stdlib.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <random>
#include <string>
#include <string_view>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/android_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "src/base/test/test_task_runner.h"
#include "src/base/test/tmp_dir_tree.h"
#include "test/android_test_utils.h"
#include "test/cts/heapprofd_test_helper.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/profiling/heapprofd_config.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto {
namespace {

// Path in the app external directory where the app writes an interation
// counter. It is used to wait for the test apps to actually perform
// allocations.
constexpr std::string_view kReportCyclePath = "report_cycle.txt";

// Asks FileContentProvider.java inside the app to read a file.
class ContentProviderReader {
 public:
  explicit ContentProviderReader(const std::string& app,
                                 const std::string& path) {
    tmp_dir_.TrackFile("contents.txt");
    tempfile_ = tmp_dir_.AbsolutePath("contents.txt");

    std::optional<int32_t> sdk =
        base::StringToInt32(base::GetAndroidProp("ro.build.version.sdk"));
    bool multiuser_support = sdk && *sdk >= 34;
    cmd_ = "content read";
    if (multiuser_support) {
      // This is required only starting from android U.
      cmd_ += " --user `am get-current-user`";
    }
    cmd_ += std::string(" --uri content://") + app + std::string("/") + path;
    cmd_ += " >" + tempfile_;
  }

  std::optional<int64_t> ReadInt64() {
    if (system(cmd_.c_str()) != 0) {
      return std::nullopt;
    }
    return ReadInt64FromFile(tempfile_);
  }

 private:
  std::optional<int64_t> ReadInt64FromFile(const std::string& path) {
    std::string contents;
    if (!base::ReadFile(path, &contents)) {
      return std::nullopt;
    }
    return base::StringToInt64(contents);
  }

  base::TmpDirTree tmp_dir_;
  std::string tempfile_;
  std::string cmd_;
};

bool WaitForAppAllocationCycle(const std::string& app_name, size_t timeout_ms) {
  const size_t sleep_per_attempt_us = 100 * 1000;
  const size_t max_attempts = timeout_ms * 1000 / sleep_per_attempt_us;

  ContentProviderReader app_reader(app_name, std::string(kReportCyclePath));

  for (size_t attempts = 0; attempts < max_attempts;) {
    int64_t first_value;
    for (; attempts < max_attempts; attempts++) {
      std::optional<int64_t> val = app_reader.ReadInt64();
      if (val) {
        first_value = *val;
        break;
      }
      base::SleepMicroseconds(sleep_per_attempt_us);
    }

    for (; attempts < max_attempts; attempts++) {
      std::optional<int64_t> val = app_reader.ReadInt64();
      if (!val || *val < first_value) {
        break;
      }
      if (*val >= first_value + 2) {
        // We've observed the counter being incremented twice. We can be sure
        // that the app has gone through a full allocation cycle.
        return true;
      }
      base::SleepMicroseconds(sleep_per_attempt_us);
    }
  }
  return false;
}

}  // namespace

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
    uint64_t sampling_interval,
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
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(sampling_interval);
  heapprofd_config.add_process_cmdline(app_name.c_str());
  heapprofd_config.set_block_client(true);
  heapprofd_config.set_all(false);
  for (const std::string& heap_name : heap_names) {
    heapprofd_config.add_heaps(heap_name);
  }
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  // start tracing
  helper.StartTracing(trace_config);

  EXPECT_TRUE(WaitForAppAllocationCycle(app_name, /*timeout_ms=*/10000));

  helper.DisableTracing();
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
    uint64_t sampling_interval,
    const std::vector<std::string>& heap_names,
    const bool enable_extra_guardrails) {
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
  trace_config.set_enable_extra_guardrails(enable_extra_guardrails);
  trace_config.set_unique_session_name(RandomSessionName().c_str());

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(sampling_interval);
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

  EXPECT_TRUE(WaitForAppAllocationCycle(app_name, /*timeout_ms=*/10000));

  helper.DisableTracing();
  helper.WaitForTracingDisabled();
  helper.ReadData();
  helper.WaitForReadData();

  return helper.trace();
}

void AssertExpectedMallocsPresent(
    uint64_t expected_individual_alloc_sz,
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
            sample.self_allocated() % expected_individual_alloc_sz == 0) {
          found_alloc = true;

          EXPECT_TRUE(sample.self_freed() > 0 &&
                      sample.self_freed() % expected_individual_alloc_sz == 0)
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

// Copied from
// https://source.corp.google.com/h/googleplex-android/platform/superproject/main/+/main:system/libbase/include/android-base/macros.h;l=137
// Current ABI string
#if defined(__arm__)
#define ABI_STRING "arm"
#elif defined(__aarch64__)
#define ABI_STRING "arm64"
#elif defined(__i386__)
#define ABI_STRING "x86"
#elif defined(__riscv)
#define ABI_STRING "riscv64"
#elif defined(__x86_64__)
#define ABI_STRING "x86_64"
#endif

bool RunningWithNativeBridge() {
  static const prop_info* pi =
      __system_property_find("ro.dalvik.vm.isa." ABI_STRING);
  return pi != nullptr;
}

}  // namespace perfetto
