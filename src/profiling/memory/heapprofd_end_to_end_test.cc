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

// End to end tests for heapprofd.
// None of these tests currently pass on non-Android, but we still build most
// of it as a best-effort way to maintain the out-of-tree build.

#include <atomic>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/tracing/ipc/default_socket.h"
#include "perfetto/profiling/memory/client_ext.h"
#include "src/base/test/test_task_runner.h"
#include "src/profiling/memory/heapprofd_producer.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/system_properties.h>
#endif

#include "protos/perfetto/config/profiling/heapprofd_config.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"

namespace perfetto {
namespace profiling {
namespace {

constexpr useconds_t kMsToUs = 1000;

constexpr auto kTracingDisabledTimeoutMs = 30000;
constexpr auto kWaitForReadDataTimeoutMs = 10000;

using ::testing::AnyOf;
using ::testing::Bool;
using ::testing::Eq;

constexpr const char* kHeapprofdModeProperty = "heapprofd.userdebug.mode";

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

std::string ReadProperty(const std::string& name, std::string def) {
  const prop_info* pi = __system_property_find(name.c_str());
  if (pi) {
    __system_property_read_callback(
        pi,
        [](void* cookie, const char*, const char* value, uint32_t) {
          *reinterpret_cast<std::string*>(cookie) = value;
        },
        &def);
  }
  return def;
}

int SetModeProperty(std::string* value) {
  if (value) {
    __system_property_set(kHeapprofdModeProperty, value->c_str());
    delete value;
  }
  return 0;
}

base::ScopedResource<std::string*, SetModeProperty, nullptr> EnableFork() {
  std::string prev_property_value = ReadProperty(kHeapprofdModeProperty, "");
  __system_property_set(kHeapprofdModeProperty, "fork");
  return base::ScopedResource<std::string*, SetModeProperty, nullptr>(
      new std::string(prev_property_value));
}

base::ScopedResource<std::string*, SetModeProperty, nullptr> DisableFork() {
  std::string prev_property_value = ReadProperty(kHeapprofdModeProperty, "");
  __system_property_set(kHeapprofdModeProperty, "");
  return base::ScopedResource<std::string*, SetModeProperty, nullptr>(
      new std::string(prev_property_value));
}

void CustomAllocateAndFree(size_t bytes) {
  HeapprofdHeapInfo info{"test", nullptr};
  static uint32_t heap_id = heapprofd_register_heap(&info, sizeof(info));
  heapprofd_report_allocation(heap_id, 0x1234abc, bytes);
  heapprofd_report_free(heap_id, 0x1234abc);
}

#else
std::string ReadProperty(const std::string&, std::string) {
  PERFETTO_FATAL("Only works on Android.");
}

int SetModeProperty(std::string*) {
  PERFETTO_FATAL("Only works on Android.");
}

base::ScopedResource<std::string*, SetModeProperty, nullptr> EnableFork() {
  PERFETTO_FATAL("Only works on Android.");
}

base::ScopedResource<std::string*, SetModeProperty, nullptr> DisableFork() {
  PERFETTO_FATAL("Only works on Android.");
}

void __attribute__((noreturn)) CustomAllocateAndFree(size_t) {
  PERFETTO_FATAL("Only works on Android.");
}

#endif

constexpr size_t kStartupAllocSize = 10;

void AllocateAndFree(size_t bytes) {
  // This volatile is needed to prevent the compiler from trying to be
  // helpful and compiling a "useless" malloc + free into a noop.
  volatile char* x = static_cast<char*>(malloc(bytes));
  if (x) {
    x[1] = 'x';
    free(const_cast<char*>(x));
  }
}

void __attribute__((noreturn))
ContinuousMalloc(size_t malloc_bytes, size_t custom_bytes) {
  for (;;) {
    // We need to run malloc(0) even if we want to test the custom allocator,
    // as the init mechanism assumes the application uses malloc.
    AllocateAndFree(malloc_bytes);
    if (custom_bytes)
      CustomAllocateAndFree(custom_bytes);
    usleep(10 * kMsToUs);
  }
}

base::Subprocess ForkContinuousMalloc(size_t malloc_bytes,
                                      size_t custom_bytes = 0) {
  base::Subprocess proc;
  proc.args.entrypoint_for_testing = [malloc_bytes, custom_bytes] {
    ContinuousMalloc(malloc_bytes, custom_bytes);
  };
  proc.Start();
  return proc;
}

void __attribute__((constructor)) RunContinuousMalloc() {
  if (getenv("HEAPPROFD_TESTING_RUN_MALLOC") != nullptr)
    ContinuousMalloc(kStartupAllocSize, 0);
}

std::unique_ptr<TestHelper> GetHelper(base::TestTaskRunner* task_runner) {
  std::unique_ptr<TestHelper> helper(new TestHelper(task_runner));
  helper->ConnectConsumer();
  helper->WaitForConsumerConnect();
  return helper;
}

std::string FormatHistogram(const protos::gen::ProfilePacket_Histogram& hist) {
  std::string out;
  std::string prev_upper_limit = "-inf";
  for (const auto& bucket : hist.buckets()) {
    std::string upper_limit;
    if (bucket.max_bucket())
      upper_limit = "inf";
    else
      upper_limit = std::to_string(bucket.upper_limit());

    out += "[" + prev_upper_limit + ", " + upper_limit +
           "]: " + std::to_string(bucket.count()) + "; ";
    prev_upper_limit = std::move(upper_limit);
  }
  return out + "\n";
}

std::string FormatStats(const protos::gen::ProfilePacket_ProcessStats& stats) {
  return std::string("unwinding_errors: ") +
         std::to_string(stats.unwinding_errors()) + "\n" +
         "heap_samples: " + std::to_string(stats.heap_samples()) + "\n" +
         "map_reparses: " + std::to_string(stats.map_reparses()) + "\n" +
         "unwinding_time_us: " + FormatHistogram(stats.unwinding_time_us());
}

std::string TestSuffix(const ::testing::TestParamInfo<bool>& info) {
  return info.param ? "ForkMode" : "CentralMode";
}

class HeapprofdEndToEnd : public ::testing::TestWithParam<bool> {
 public:
  HeapprofdEndToEnd() {
    // This is not needed for correctness, but works around a init behavior that
    // makes this test take much longer. If persist.heapprofd.enable is set to 0
    // and then set to 1 again too quickly, init decides that the service is
    // "restarting" and waits before restarting it.
    usleep(50000);
    bool should_fork = GetParam();
    if (should_fork) {
      fork_prop_ = EnableFork();
      PERFETTO_CHECK(ReadProperty(kHeapprofdModeProperty, "") == "fork");
    } else {
      fork_prop_ = DisableFork();
      PERFETTO_CHECK(ReadProperty(kHeapprofdModeProperty, "") == "");
    }
  }

 protected:
  base::TestTaskRunner task_runner;
  base::ScopedResource<std::string*, SetModeProperty, nullptr> fork_prop_{
      nullptr};

  std::unique_ptr<TestHelper> Trace(const TraceConfig& trace_config) {
    auto helper = GetHelper(&task_runner);

    helper->StartTracing(trace_config);
    helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);

    helper->ReadData();
    helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);
    return helper;
  }

  void PrintStats(TestHelper* helper) {
    const auto& packets = helper->trace();
    for (const protos::gen::TracePacket& packet : packets) {
      for (const auto& dump : packet.profile_packet().process_dumps()) {
        // protobuf uint64 does not like the PRIu64 formatter.
        PERFETTO_LOG("Stats for %s: %s", std::to_string(dump.pid()).c_str(),
                     FormatStats(dump.stats()).c_str());
      }
    }
  }

  void ValidateSampleSizes(TestHelper* helper,
                           uint64_t pid,
                           uint64_t alloc_size,
                           const std::string& heap_name = "") {
    const auto& packets = helper->trace();
    for (const protos::gen::TracePacket& packet : packets) {
      for (const auto& dump : packet.profile_packet().process_dumps()) {
        if (dump.pid() != pid ||
            (!heap_name.empty() && heap_name != dump.heap_name())) {
          continue;
        }
        for (const auto& sample : dump.samples()) {
          EXPECT_EQ(sample.self_allocated() % alloc_size, 0u);
          EXPECT_EQ(sample.self_freed() % alloc_size, 0u);
          EXPECT_THAT(sample.self_allocated() - sample.self_freed(),
                      AnyOf(Eq(0u), Eq(alloc_size)));
        }
      }
    }
  }

  void ValidateFromStartup(TestHelper* helper,
                           uint64_t pid,
                           bool from_startup) {
    const auto& packets = helper->trace();
    for (const protos::gen::TracePacket& packet : packets) {
      for (const auto& dump : packet.profile_packet().process_dumps()) {
        if (dump.pid() != pid)
          continue;
        EXPECT_EQ(dump.from_startup(), from_startup);
      }
    }
  }

  void ValidateRejectedConcurrent(TestHelper* helper,
                                  uint64_t pid,
                                  bool rejected_concurrent) {
    const auto& packets = helper->trace();
    for (const protos::gen::TracePacket& packet : packets) {
      for (const auto& dump : packet.profile_packet().process_dumps()) {
        if (dump.pid() != pid)
          continue;
        EXPECT_EQ(dump.rejected_concurrent(), rejected_concurrent);
      }
    }
  }

  void ValidateNoSamples(TestHelper* helper, uint64_t pid) {
    const auto& packets = helper->trace();
    size_t samples = 0;
    for (const protos::gen::TracePacket& packet : packets) {
      for (const auto& dump : packet.profile_packet().process_dumps()) {
        if (dump.pid() != pid)
          continue;
        samples += dump.samples().size();
      }
    }
    EXPECT_EQ(samples, 0u);
  }

  void ValidateHasSamples(TestHelper* helper,
                          uint64_t pid,
                          const std::string& heap_name) {
    const auto& packets = helper->trace();
    ASSERT_GT(packets.size(), 0u);
    size_t profile_packets = 0;
    size_t samples = 0;
    uint64_t last_allocated = 0;
    uint64_t last_freed = 0;
    for (const protos::gen::TracePacket& packet : packets) {
      for (const auto& dump : packet.profile_packet().process_dumps()) {
        if (dump.pid() != pid || dump.heap_name() != heap_name)
          continue;
        for (const auto& sample : dump.samples()) {
          last_allocated = sample.self_allocated();
          last_freed = sample.self_freed();
          samples++;
        }
        profile_packets++;
      }
    }
    EXPECT_GT(profile_packets, 0u) << heap_name;
    EXPECT_GT(samples, 0u) << heap_name;
    EXPECT_GT(last_allocated, 0u) << heap_name;
    EXPECT_GT(last_freed, 0u) << heap_name;
  }

  void ValidateOnlyPID(TestHelper* helper, uint64_t pid) {
    size_t dumps = 0;
    const auto& packets = helper->trace();
    for (const protos::gen::TracePacket& packet : packets) {
      for (const auto& dump : packet.profile_packet().process_dumps()) {
        EXPECT_EQ(dump.pid(), pid);
        dumps++;
      }
    }
    EXPECT_GT(dumps, 0u);
  }
};

// This checks that the child is still running (to ensure it didn't crash
// unxpectedly) and then kills it.
void KillAssertRunning(base::Subprocess* child) {
  ASSERT_EQ(child->Poll(), base::Subprocess::kRunning);
  child->KillAndWaitForTermination();
}

TEST_P(HeapprofdEndToEnd, MallocDisabled) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousMalloc(kAllocSize);
  const auto pid = child.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  heapprofd_config.add_heaps("invalid");
  auto* cont_config = heapprofd_config.mutable_continuous_dump_config();
  cont_config->set_dump_phase_ms(0);
  cont_config->set_dump_interval_ms(100);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());

  ValidateNoSamples(helper.get(), static_cast<uint64_t>(pid));
  KillAssertRunning(&child);
}
TEST_P(HeapprofdEndToEnd, Smoke) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousMalloc(kAllocSize);
  const auto pid = child.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  auto* cont_config = heapprofd_config.mutable_continuous_dump_config();
  cont_config->set_dump_phase_ms(0);
  cont_config->set_dump_interval_ms(100);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "malloc");
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid), kAllocSize);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, SmokeCustomAndMalloc) {
  constexpr size_t kCustomAllocSize = 1024;
  constexpr size_t kAllocSize = 7;

  base::Subprocess child = ForkContinuousMalloc(kAllocSize, kCustomAllocSize);
  const auto pid = child.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  heapprofd_config.add_heaps("test");
  heapprofd_config.add_heaps("malloc");
  auto* cont_config = heapprofd_config.mutable_continuous_dump_config();
  cont_config->set_dump_phase_ms(0);
  cont_config->set_dump_interval_ms(100);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "test");
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "malloc");
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid),
                      kCustomAllocSize, "test");
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid), kAllocSize,
                      "malloc");

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, AccurateCustom) {
  base::Subprocess child;
  child.args.entrypoint_for_testing = [] {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    static std::atomic<bool> initialized{false};
    HeapprofdHeapInfo info{"test", [](bool) { initialized = true; }};

    static uint32_t heap_id = heapprofd_register_heap(&info, sizeof(info));
    // heapprofd_client needs malloc to see the signal.
    while (!initialized)
      AllocateAndFree(1);
    // We call the callback before setting enabled=true on the heap, so we
    // wait a bit for the assignment to happen.
    usleep(100000);
    heapprofd_report_allocation(heap_id, 0x1, 10u);
    heapprofd_report_free(heap_id, 0x1);
    heapprofd_report_allocation(heap_id, 0x2, 15u);
    heapprofd_report_allocation(heap_id, 0x3, 15u);
    heapprofd_report_free(heap_id, 0x2);

    // Wait around so we can verify it did't crash.
    for (;;) {
    }
#else
    PERFETTO_FATAL("Only supported on Android.");
#endif
  };
  child.Start();
  const auto pid = child.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  heapprofd_config.add_heaps("test");
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));

  size_t total_alloc = 0;
  size_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : helper->trace()) {
    for (const auto& dump : packet.profile_packet().process_dumps()) {
      for (const auto& sample : dump.samples()) {
        total_alloc += sample.self_allocated();
        total_freed += sample.self_freed();
      }
    }
  }
  EXPECT_EQ(total_alloc, 40u);
  EXPECT_EQ(total_freed, 25u);
  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, SmokeCustom) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousMalloc(0, kAllocSize);
  const auto pid = child.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  heapprofd_config.add_heaps("test");
  auto* cont_config = heapprofd_config.mutable_continuous_dump_config();
  cont_config->set_dump_phase_ms(0);
  cont_config->set_dump_interval_ms(100);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "test");
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid), kAllocSize);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, CustomDisabled) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousMalloc(0, kAllocSize);
  const auto pid = child.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  auto* cont_config = heapprofd_config.mutable_continuous_dump_config();
  cont_config->set_dump_phase_ms(0);
  cont_config->set_dump_interval_ms(100);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateNoSamples(helper.get(), static_cast<uint64_t>(pid));

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, TwoProcesses) {
  constexpr size_t kAllocSize = 1024;
  constexpr size_t kAllocSize2 = 7;

  base::Subprocess child = ForkContinuousMalloc(kAllocSize);
  base::Subprocess child2 = ForkContinuousMalloc(kAllocSize2);
  const auto pid = child.pid();
  const auto pid2 = child2.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.add_pid(static_cast<uint64_t>(pid2));
  heapprofd_config.set_all(false);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "malloc");
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid), kAllocSize);
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid2), "malloc");
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid2), kAllocSize2);

  KillAssertRunning(&child);
  KillAssertRunning(&child2);
}

TEST_P(HeapprofdEndToEnd, FinalFlush) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousMalloc(kAllocSize);
  const auto pid = child.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "malloc");
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid), kAllocSize);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, NativeStartup) {
  auto helper = GetHelper(&task_runner);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(5000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_process_cmdline("heapprofd_continuous_malloc");
  heapprofd_config.set_all(false);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  helper->StartTracing(trace_config);

  // Wait to guarantee that the process forked below is hooked by the profiler
  // by virtue of the startup check, and not by virtue of being seen as a
  // running process. This sleep is here to prevent that, accidentally, the
  // test gets to the fork()+exec() too soon, before the heap profiling daemon
  // has received the trace config.
  sleep(1);

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC=1");
  child.Start();

  helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);

  helper->ReadData();
  helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);

  KillAssertRunning(&child);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  uint64_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        packet.profile_packet().process_dumps().size() > 0) {
      const auto& dumps = packet.profile_packet().process_dumps();
      ASSERT_EQ(dumps.size(), 1u);
      const protos::gen::ProfilePacket_ProcessHeapSamples& dump = dumps[0];
      EXPECT_EQ(static_cast<pid_t>(dump.pid()), child.pid());
      profile_packets++;
      for (const auto& sample : dump.samples()) {
        samples++;
        total_allocated += sample.self_allocated();
        total_freed += sample.self_freed();
      }
    }
  }
  EXPECT_EQ(profile_packets, 1u);
  EXPECT_GT(samples, 0u);
  EXPECT_GT(total_allocated, 0u);
  EXPECT_GT(total_freed, 0u);
}

TEST_P(HeapprofdEndToEnd, NativeStartupDenormalizedCmdline) {
  auto helper = GetHelper(&task_runner);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(5000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_process_cmdline("heapprofd_continuous_malloc@1.2.3");
  heapprofd_config.set_all(false);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  helper->StartTracing(trace_config);

  // Wait to guarantee that the process forked below is hooked by the profiler
  // by virtue of the startup check, and not by virtue of being seen as a
  // running process. This sleep is here to prevent that, accidentally, the
  // test gets to the fork()+exec() too soon, before the heap profiling daemon
  // has received the trace config.
  sleep(1);

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC=1");
  child.Start();

  helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);

  helper->ReadData();
  helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);

  KillAssertRunning(&child);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  uint64_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        packet.profile_packet().process_dumps().size() > 0) {
      const auto& dumps = packet.profile_packet().process_dumps();
      ASSERT_EQ(dumps.size(), 1u);
      const protos::gen::ProfilePacket_ProcessHeapSamples& dump = dumps[0];
      EXPECT_EQ(static_cast<pid_t>(dump.pid()), child.pid());
      profile_packets++;
      for (const auto& sample : dump.samples()) {
        samples++;
        total_allocated += sample.self_allocated();
        total_freed += sample.self_freed();
      }
    }
  }
  EXPECT_EQ(profile_packets, 1u);
  EXPECT_GT(samples, 0u);
  EXPECT_GT(total_allocated, 0u);
  EXPECT_GT(total_freed, 0u);
}

TEST_P(HeapprofdEndToEnd, DiscoverByName) {
  auto helper = GetHelper(&task_runner);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(5000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_process_cmdline("heapprofd_continuous_malloc");
  heapprofd_config.set_all(false);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC=1");
  child.Start();

  // Wait to make sure process is fully initialized, so we do not accidentally
  // match it by the startup logic.
  sleep(1);

  helper->StartTracing(trace_config);
  helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);

  helper->ReadData();
  helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);

  KillAssertRunning(&child);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  uint64_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        packet.profile_packet().process_dumps().size() > 0) {
      const auto& dumps = packet.profile_packet().process_dumps();
      ASSERT_EQ(dumps.size(), 1u);
      const protos::gen::ProfilePacket_ProcessHeapSamples& dump = dumps[0];
      EXPECT_EQ(static_cast<pid_t>(dump.pid()), child.pid());
      profile_packets++;
      for (const auto& sample : dump.samples()) {
        samples++;
        total_allocated += sample.self_allocated();
        total_freed += sample.self_freed();
      }
    }
  }
  EXPECT_EQ(profile_packets, 1u);
  EXPECT_GT(samples, 0u);
  EXPECT_GT(total_allocated, 0u);
  EXPECT_GT(total_freed, 0u);
}

TEST_P(HeapprofdEndToEnd, DiscoverByNameDenormalizedCmdline) {
  auto helper = GetHelper(&task_runner);

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(5000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_process_cmdline("heapprofd_continuous_malloc@1.2.3");
  heapprofd_config.set_all(false);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  // Make sure the forked process does not get reparented to init.
  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC=1");
  child.Start();

  // Wait to make sure process is fully initialized, so we do not accidentally
  // match it by the startup logic.
  sleep(1);

  helper->StartTracing(trace_config);
  helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);

  helper->ReadData();
  helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);

  KillAssertRunning(&child);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  uint64_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        packet.profile_packet().process_dumps().size() > 0) {
      const auto& dumps = packet.profile_packet().process_dumps();
      ASSERT_EQ(dumps.size(), 1u);
      const protos::gen::ProfilePacket_ProcessHeapSamples& dump = dumps[0];
      EXPECT_EQ(static_cast<pid_t>(dump.pid()), child.pid());
      profile_packets++;
      for (const auto& sample : dump.samples()) {
        samples++;
        total_allocated += sample.self_allocated();
        total_freed += sample.self_freed();
      }
    }
  }
  EXPECT_EQ(profile_packets, 1u);
  EXPECT_GT(samples, 0u);
  EXPECT_GT(total_allocated, 0u);
  EXPECT_GT(total_freed, 0u);
}

TEST_P(HeapprofdEndToEnd, ReInit) {
  constexpr size_t kFirstIterationBytes = 5;
  constexpr size_t kSecondIterationBytes = 7;

  base::Pipe signal_pipe = base::Pipe::Create(base::Pipe::kBothNonBlock);
  base::Pipe ack_pipe = base::Pipe::Create(base::Pipe::kBothBlock);

  base::Subprocess child;
  int signal_pipe_rd = *signal_pipe.rd;
  int ack_pipe_wr = *ack_pipe.wr;
  child.args.preserve_fds.push_back(signal_pipe_rd);
  child.args.preserve_fds.push_back(ack_pipe_wr);
  child.args.entrypoint_for_testing = [signal_pipe_rd, ack_pipe_wr] {
    // The Subprocess harness takes care of closing all the unused pipe ends.
    size_t bytes = kFirstIterationBytes;
    bool signalled = false;
    for (;;) {
      AllocateAndFree(bytes);
      char buf[1];
      if (!signalled && read(signal_pipe_rd, buf, sizeof(buf)) == 1) {
        signalled = true;
        close(signal_pipe_rd);

        // make sure the client has noticed that the session has stopped
        AllocateAndFree(bytes);

        bytes = kSecondIterationBytes;
        PERFETTO_CHECK(PERFETTO_EINTR(write(ack_pipe_wr, "1", 1)) == 1);
        close(ack_pipe_wr);
      }
      usleep(10 * kMsToUs);
    }
    PERFETTO_FATAL("Should be unreachable");
  };
  child.Start();
  auto pid = child.pid();

  signal_pipe.rd.reset();
  ack_pipe.wr.reset();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "malloc");
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid),
                      kFirstIterationBytes);

  PERFETTO_CHECK(PERFETTO_EINTR(write(*signal_pipe.wr, "1", 1)) == 1);
  signal_pipe.wr.reset();
  char buf[1];
  ASSERT_EQ(PERFETTO_EINTR(read(*ack_pipe.rd, buf, sizeof(buf))), 1);
  ack_pipe.rd.reset();

  // A brief sleep to allow the client to notice that the profiling session is
  // to be torn down (as it rejects concurrent sessions).
  usleep(500 * kMsToUs);

  PERFETTO_LOG("HeapprofdEndToEnd::Reinit: Starting second");
  helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "malloc");
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid),
                      kSecondIterationBytes);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, ReInitAfterInvalid) {
  constexpr size_t kFirstIterationBytes = 5;
  constexpr size_t kSecondIterationBytes = 7;

  base::Pipe signal_pipe = base::Pipe::Create(base::Pipe::kBothNonBlock);
  base::Pipe ack_pipe = base::Pipe::Create(base::Pipe::kBothBlock);

  base::Subprocess child;
  int signal_pipe_rd = *signal_pipe.rd;
  int ack_pipe_wr = *ack_pipe.wr;
  child.args.preserve_fds.push_back(signal_pipe_rd);
  child.args.preserve_fds.push_back(ack_pipe_wr);
  child.args.entrypoint_for_testing = [signal_pipe_rd, ack_pipe_wr] {
    // The Subprocess harness takes care of closing all the unused pipe ends.
    size_t bytes = kFirstIterationBytes;
    bool signalled = false;
    for (;;) {
      AllocateAndFree(bytes);
      char buf[1];
      if (!signalled && read(signal_pipe_rd, buf, sizeof(buf)) == 1) {
        signalled = true;
        close(signal_pipe_rd);

        // make sure the client has noticed that the session has stopped
        AllocateAndFree(bytes);

        bytes = kSecondIterationBytes;
        PERFETTO_CHECK(PERFETTO_EINTR(write(ack_pipe_wr, "1", 1)) == 1);
        close(ack_pipe_wr);
      }
      usleep(10 * kMsToUs);
    }
    PERFETTO_FATAL("Should be unreachable");
  };
  child.Start();
  auto pid = child.pid();

  signal_pipe.rd.reset();
  ack_pipe.wr.reset();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  heapprofd_config.add_heaps("invalid");
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateNoSamples(helper.get(), static_cast<uint64_t>(pid));

  PERFETTO_CHECK(PERFETTO_EINTR(write(*signal_pipe.wr, "1", 1)) == 1);
  signal_pipe.wr.reset();
  char buf[1];
  ASSERT_EQ(PERFETTO_EINTR(read(*ack_pipe.rd, buf, sizeof(buf))), 1);
  ack_pipe.rd.reset();

  // A brief sleep to allow the client to notice that the profiling session is
  // to be torn down (as it rejects concurrent sessions).
  usleep(500 * kMsToUs);

  heapprofd_config.add_heaps("malloc");
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  PERFETTO_LOG("HeapprofdEndToEnd::ReinitAfterInvalid: Starting second");
  helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "malloc");
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid),
                      kSecondIterationBytes);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, ConcurrentSession) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousMalloc(kAllocSize);
  const auto pid = child.pid();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(5000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  heapprofd_config.set_all(false);
  auto* cont_config = heapprofd_config.mutable_continuous_dump_config();
  cont_config->set_dump_phase_ms(0);
  cont_config->set_dump_interval_ms(100);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  auto helper = GetHelper(&task_runner);
  helper->StartTracing(trace_config);
  sleep(1);
  auto helper_concurrent = GetHelper(&task_runner);
  helper_concurrent->StartTracing(trace_config);

  helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);
  helper->ReadData();
  helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid), "malloc");
  ValidateOnlyPID(helper.get(), static_cast<uint64_t>(pid));
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid), kAllocSize);
  ValidateRejectedConcurrent(helper_concurrent.get(),
                             static_cast<uint64_t>(pid), false);

  helper_concurrent->WaitForTracingDisabled(kTracingDisabledTimeoutMs);
  helper_concurrent->ReadData();
  helper_concurrent->WaitForReadData(0, kWaitForReadDataTimeoutMs);
  PrintStats(helper.get());
  ValidateOnlyPID(helper_concurrent.get(), static_cast<uint64_t>(pid));
  ValidateRejectedConcurrent(helper_concurrent.get(),
                             static_cast<uint64_t>(pid), true);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, NativeProfilingActiveAtProcessExit) {
  constexpr uint64_t kTestAllocSize = 128;
  base::Pipe start_pipe = base::Pipe::Create(base::Pipe::kBothBlock);
  base::Subprocess child;
  int start_pipe_wr = *start_pipe.wr;
  child.args.preserve_fds.push_back(start_pipe_wr);
  child.args.entrypoint_for_testing = [start_pipe_wr] {
    PERFETTO_CHECK(PERFETTO_EINTR(write(start_pipe_wr, "1", 1)) == 1);
    PERFETTO_CHECK(close(start_pipe_wr) == 0 || errno == EINTR);

    // The subprocess harness will take care of closing
    for (int i = 0; i < 200; i++) {
      // malloc and leak, otherwise the free batching will cause us to filter
      // out the allocations (as we don't see the interleaved frees).
      volatile char* x = static_cast<char*>(malloc(kTestAllocSize));
      if (x) {
        x[0] = 'x';
      }
      usleep(10 * kMsToUs);
    }
  };
  child.Start();
  auto pid = child.pid();
  start_pipe.wr.reset();

  // Construct tracing config (without starting profiling).
  auto helper = GetHelper(&task_runner);
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(5000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");

  protos::gen::HeapprofdConfig heapprofd_config;
  heapprofd_config.set_sampling_interval_bytes(1);
  heapprofd_config.add_pid(static_cast<uint64_t>(pid));
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());

  // Wait for child to have been scheduled at least once.
  char buf[1] = {};
  ASSERT_EQ(PERFETTO_EINTR(read(*start_pipe.rd, buf, sizeof(buf))), 1);
  start_pipe.rd.reset();

  // Trace until child exits.
  helper->StartTracing(trace_config);

  // Wait for the child and assert that it exited successfully.
  EXPECT_TRUE(child.Wait(30000));
  EXPECT_EQ(child.status(), base::Subprocess::kExited);
  EXPECT_EQ(child.returncode(), 0);

  // Assert that we did profile the process.
  helper->FlushAndWait(2000);
  helper->DisableTracing();
  helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);
  helper->ReadData();
  helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        packet.profile_packet().process_dumps().size() > 0) {
      const auto& dumps = packet.profile_packet().process_dumps();
      ASSERT_EQ(dumps.size(), 1u);
      const protos::gen::ProfilePacket_ProcessHeapSamples& dump = dumps[0];
      EXPECT_EQ(static_cast<pid_t>(dump.pid()), pid);
      profile_packets++;
      for (const auto& sample : dump.samples()) {
        samples++;
        total_allocated += sample.self_allocated();
      }
    }
  }
  EXPECT_EQ(profile_packets, 1u);
  EXPECT_GT(samples, 0u);
  EXPECT_GT(total_allocated, 0u);
}

// This test only works when run on Android using an Android Q version of
// Bionic.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
INSTANTIATE_TEST_CASE_P(DISABLED_Run, HeapprofdEndToEnd, Bool(), TestSuffix);
#else
INSTANTIATE_TEST_CASE_P(Run, HeapprofdEndToEnd, Bool(), TestSuffix);
#endif

}  // namespace
}  // namespace profiling
}  // namespace perfetto
