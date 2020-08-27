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
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"
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
constexpr size_t kStartupAllocSize = 10;
constexpr size_t kFirstIterationBytes = 5;
constexpr size_t kSecondIterationBytes = 7;

constexpr const char* kHeapprofdModeProperty = "heapprofd.userdebug.mode";

enum class TestMode { kCentral, kFork, kStatic };
enum class AllocatorMode { kMalloc, kCustom };

using ::testing::AnyOf;
using ::testing::Bool;
using ::testing::Eq;
using ::testing::Values;

std::string AllocatorName(AllocatorMode mode) {
  switch (mode) {
    case AllocatorMode::kMalloc:
      return "com.android.malloc";
    case AllocatorMode::kCustom:
      return "test";
  }
}

AllocatorMode AllocatorModeFromNameOrDie(std::string s) {
  if (s == "com.android.malloc")
    return AllocatorMode::kMalloc;
  if (s == "test")
    return AllocatorMode::kCustom;
  PERFETTO_FATAL("Invalid allocator mode [malloc | test]: %s", s.c_str());
}

void ContinuousDump(HeapprofdConfig* cfg) {
  auto* cont_config = cfg->mutable_continuous_dump_config();
  cont_config->set_dump_phase_ms(0);
  cont_config->set_dump_interval_ms(100);
}

template <typename F>
TraceConfig MakeTraceConfig(F fn) {
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(2000);
  trace_config.set_data_source_stop_timeout_ms(10000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  protos::gen::HeapprofdConfig heapprofd_config;
  fn(&heapprofd_config);
  ds_config->set_heapprofd_config_raw(heapprofd_config.SerializeAsString());
  return trace_config;
}

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

#endif

void CustomAllocateAndFree(size_t bytes) {
  static uint32_t heap_id = AHeapProfile_registerHeap(AHeapInfo_create("test"));
  AHeapProfile_reportAllocation(heap_id, 0x1234abc, bytes);
  AHeapProfile_reportFree(heap_id, 0x1234abc);
}

void SecondaryAllocAndFree(size_t bytes) {
  static uint32_t heap_id =
      AHeapProfile_registerHeap(AHeapInfo_create("secondary"));
  AHeapProfile_reportAllocation(heap_id, 0x1234abc, bytes);
  AHeapProfile_reportFree(heap_id, 0x1234abc);
}

void AllocateAndFree(size_t bytes) {
  // This volatile is needed to prevent the compiler from trying to be
  // helpful and compiling a "useless" malloc + free into a noop.
  volatile char* x = static_cast<char*>(malloc(bytes));
  if (x) {
    if (bytes > 0)
      x[0] = 'x';
    free(const_cast<char*>(x));
  }
}

void DoAllocation(AllocatorMode mode, size_t bytes) {
  switch (mode) {
    case AllocatorMode::kMalloc:
      AllocateAndFree(bytes);
      break;
    case AllocatorMode::kCustom:
      // We need to run malloc(0) even if we want to test the custom allocator,
      // as the init mechanism assumes the application uses malloc.
      AllocateAndFree(1);
      CustomAllocateAndFree(bytes);
      break;
  }
}

void ContinuousMalloc(AllocatorMode mode,
                      size_t primary_bytes,
                      size_t secondary_bytes,
                      ssize_t max_iter = -1) {
  for (ssize_t i = 0; max_iter == -1 || i < max_iter; ++i) {
    DoAllocation(mode, primary_bytes);
    if (secondary_bytes)
      SecondaryAllocAndFree(secondary_bytes);
    usleep(10 * kMsToUs);
  }
}

void StartAndWaitForHandshake(base::Subprocess* child) {
  // We cannot use base::Pipe because that assumes we want CLOEXEC.
  // We do NOT want CLOEXEC as this gets used by the RunReInit in the child.
  int ready_pipe[2];
  PERFETTO_CHECK(pipe(ready_pipe) == 0);  // NOLINT(android-cloexec-pipe)

  int ready_pipe_rd = ready_pipe[0];
  int ready_pipe_wr = ready_pipe[1];
  child->args.preserve_fds.push_back(ready_pipe_wr);
  child->args.env.push_back("HEAPPROFD_TESTING_READY_PIPE=" +
                            std::to_string(ready_pipe_wr));
  child->Start();
  close(ready_pipe_wr);
  // Wait for libc to initialize the signal handler. If we signal before the
  // handler is installed, we can kill the process.
  char buf[1];
  PERFETTO_CHECK(PERFETTO_EINTR(read(ready_pipe_rd, buf, sizeof(buf))) == 0);
  close(ready_pipe_rd);
}

void ChildFinishHandshake() {
  const char* ready_pipe = getenv("HEAPPROFD_TESTING_READY_PIPE");
  if (ready_pipe != nullptr) {
    close(static_cast<int>(base::StringToInt64(ready_pipe).value()));
  }
}

base::Subprocess ForkContinuousAlloc(AllocatorMode mode,
                                     size_t primary_bytes,
                                     size_t secondary_bytes = 0,
                                     ssize_t max_iter = -1) {
  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           AllocatorName(mode));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(primary_bytes));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::to_string(secondary_bytes));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG3=" +
                           std::to_string(max_iter));

  StartAndWaitForHandshake(&child);
  return child;
}

void __attribute__((constructor(1024))) RunContinuousMalloc() {
  const char* a0 = getenv("HEAPPROFD_TESTING_RUN_MALLOC_ARG0");
  const char* a1 = getenv("HEAPPROFD_TESTING_RUN_MALLOC_ARG1");
  const char* a2 = getenv("HEAPPROFD_TESTING_RUN_MALLOC_ARG2");
  const char* a3 = getenv("HEAPPROFD_TESTING_RUN_MALLOC_ARG3");
  if (a0 == nullptr)
    return;

  AllocatorMode arg0 = AllocatorModeFromNameOrDie(a0);
  uint32_t arg1 = a1 ? base::StringToUInt32(a1).value() : 0;
  uint32_t arg2 = a2 ? base::StringToUInt32(a2).value() : 0;
  int32_t arg3 = a3 ? base::StringToInt32(a3).value() : -1;

  ChildFinishHandshake();

  ContinuousMalloc(arg0, arg1, arg2, arg3);
  exit(0);
}

void __attribute__((constructor(1024))) RunAccurateMalloc() {
  const char* a0 = getenv("HEAPPROFD_TESTING_RUN_ACCURATE_MALLOC");
  if (a0 == nullptr)
    return;

  static std::atomic<bool> initialized{false};
  static uint32_t heap_id = AHeapProfile_registerHeap(AHeapInfo_setCallback(
      AHeapInfo_create("test"), [](bool) { initialized = true; }));

  ChildFinishHandshake();

  // heapprofd_client needs malloc to see the signal.
  while (!initialized)
    AllocateAndFree(1);
  // We call the callback before setting enabled=true on the heap, so we
  // wait a bit for the assignment to happen.
  usleep(100000);
  AHeapProfile_reportAllocation(heap_id, 0x1, 10u);
  AHeapProfile_reportFree(heap_id, 0x1);
  AHeapProfile_reportAllocation(heap_id, 0x2, 15u);
  AHeapProfile_reportAllocation(heap_id, 0x3, 15u);
  AHeapProfile_reportFree(heap_id, 0x2);

  // Wait around so we can verify it did't crash.
  for (;;) {
  }
}

void __attribute__((constructor(1024))) RunReInit() {
  const char* a0 = getenv("HEAPPROFD_TESTING_RUN_REINIT_ARG0");
  if (a0 == nullptr)
    return;

  AllocatorMode mode = AllocatorModeFromNameOrDie(a0);
  const char* a1 = getenv("HEAPPROFD_TESTING_RUN_REINIT_ARG1");
  const char* a2 = getenv("HEAPPROFD_TESTING_RUN_REINIT_ARG2");
  PERFETTO_CHECK(a1 != nullptr && a2 != nullptr);
  int signal_pipe_rd = static_cast<int>(base::StringToInt64(a1).value());
  int ack_pipe_wr = static_cast<int>(base::StringToInt64(a2).value());

  ChildFinishHandshake();

  size_t bytes = kFirstIterationBytes;
  bool signalled = false;
  for (;;) {
    DoAllocation(mode, bytes);
    char buf[1];
    if (!signalled && read(signal_pipe_rd, buf, sizeof(buf)) == 1) {
      signalled = true;
      close(signal_pipe_rd);

      // make sure the client has noticed that the session has stopped
      DoAllocation(mode, bytes);

      bytes = kSecondIterationBytes;
      PERFETTO_CHECK(PERFETTO_EINTR(write(ack_pipe_wr, "1", 1)) == 1);
      close(ack_pipe_wr);
    }
    usleep(10 * kMsToUs);
  }
  PERFETTO_FATAL("Should be unreachable");
}

std::unique_ptr<TestHelper> GetHelper(base::TestTaskRunner* task_runner) {
  std::unique_ptr<TestHelper> helper(new TestHelper(task_runner));
  helper->StartServiceIfRequired();

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

__attribute__((unused)) std::string TestSuffix(
    const ::testing::TestParamInfo<std::tuple<TestMode, AllocatorMode>>& info) {
  TestMode tm = std::get<0>(info.param);
  AllocatorMode am = std::get<1>(info.param);

  std::string result;
  switch (tm) {
    case TestMode::kCentral:
      result += "CentralMode";
      break;
    case TestMode::kFork:
      result += "ForkMode";
      break;
    case TestMode::kStatic:
      result += "StaticMode";
      break;
  }
  switch (am) {
    case AllocatorMode::kMalloc:
      result += "Malloc";
      break;
    case AllocatorMode::kCustom:
      result += "Custom";
      break;
  }
  return result;
}

class HeapprofdEndToEnd
    : public ::testing::TestWithParam<std::tuple<TestMode, AllocatorMode>> {
 public:
  HeapprofdEndToEnd() {
    // This is not needed for correctness, but works around a init behavior that
    // makes this test take much longer. If persist.heapprofd.enable is set to 0
    // and then set to 1 again too quickly, init decides that the service is
    // "restarting" and waits before restarting it.
    usleep(50000);
    switch (test_mode()) {
      case TestMode::kCentral:
        fork_prop_ = DisableFork();
        PERFETTO_CHECK(ReadProperty(kHeapprofdModeProperty, "") == "");
        break;
      case TestMode::kFork:
        fork_prop_ = EnableFork();
        PERFETTO_CHECK(ReadProperty(kHeapprofdModeProperty, "") == "fork");
        break;
      case TestMode::kStatic:
        break;
    }
  }

 protected:
  base::TestTaskRunner task_runner;
  base::ScopedResource<std::string*, SetModeProperty, nullptr> fork_prop_{
      nullptr};

  TestMode test_mode() { return std::get<0>(GetParam()); }

  AllocatorMode allocator_mode() { return std::get<1>(GetParam()); }

  std::string allocator_name() { return AllocatorName(allocator_mode()); }

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

TEST_P(HeapprofdEndToEnd, Disabled) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps("invalid");
    ContinuousDump(cfg);
  });

  auto helper = Trace(trace_config);
  PrintStats(helper.get());

  ValidateNoSamples(helper.get(), pid);
  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, Smoke) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
    ContinuousDump(cfg);
  });

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, allocator_name());
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kAllocSize);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, TwoAllocators) {
  constexpr size_t kCustomAllocSize = 1024;
  constexpr size_t kAllocSize = 7;

  base::Subprocess child =
      ForkContinuousAlloc(allocator_mode(), kAllocSize, kCustomAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
    cfg->add_heaps("secondary");
    ContinuousDump(cfg);
  });

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, "secondary");
  ValidateHasSamples(helper.get(), pid, allocator_name());
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kCustomAllocSize, "secondary");
  ValidateSampleSizes(helper.get(), pid, kAllocSize, allocator_name());

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, TwoAllocatorsAll) {
  constexpr size_t kCustomAllocSize = 1024;
  constexpr size_t kAllocSize = 7;

  base::Subprocess child =
      ForkContinuousAlloc(allocator_mode(), kAllocSize, kCustomAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->set_all_heaps(true);
    ContinuousDump(cfg);
  });

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, "secondary");
  ValidateHasSamples(helper.get(), pid, allocator_name());
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kCustomAllocSize, "secondary");
  ValidateSampleSizes(helper.get(), pid, kAllocSize, allocator_name());

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, AccurateCustom) {
  if (allocator_mode() != AllocatorMode::kCustom)
    GTEST_SKIP();

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_ACCURATE_MALLOC=1");
  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps("test");
  });

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateOnlyPID(helper.get(), pid);

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

TEST_P(HeapprofdEndToEnd, AccurateDumpAtMaxCustom) {
  if (allocator_mode() != AllocatorMode::kCustom)
    GTEST_SKIP();

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_ACCURATE_MALLOC=1");
  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps("test");
    cfg->set_dump_at_max(true);
  });

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateOnlyPID(helper.get(), pid);

  size_t total_alloc = 0;
  size_t total_count = 0;
  for (const protos::gen::TracePacket& packet : helper->trace()) {
    for (const auto& dump : packet.profile_packet().process_dumps()) {
      for (const auto& sample : dump.samples()) {
        total_alloc += sample.self_max();
        total_count += sample.self_max_count();
      }
    }
  }
  EXPECT_EQ(total_alloc, 30u);
  EXPECT_EQ(total_count, 2u);
  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, TwoProcesses) {
  constexpr size_t kAllocSize = 1024;
  constexpr size_t kAllocSize2 = 7;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  base::Subprocess child2 = ForkContinuousAlloc(allocator_mode(), kAllocSize2);
  const uint64_t pid = static_cast<uint64_t>(child.pid());
  const auto pid2 = child2.pid();

  TraceConfig trace_config =
      MakeTraceConfig([this, pid, pid2](HeapprofdConfig* cfg) {
        cfg->set_sampling_interval_bytes(1);
        cfg->add_pid(pid);
        cfg->add_pid(static_cast<uint64_t>(pid2));
        cfg->add_heaps(allocator_name());
      });

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, allocator_name());
  ValidateSampleSizes(helper.get(), pid, kAllocSize);
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid2),
                     allocator_name());
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid2), kAllocSize2);

  KillAssertRunning(&child);
  KillAssertRunning(&child2);
}

TEST_P(HeapprofdEndToEnd, FinalFlush) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());
  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
  });

  auto helper = Trace(trace_config);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, allocator_name());
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kAllocSize);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, NativeStartup) {
  // We only enable heaps on initialization of the session. The custom heap is
  // only registered later, so we do not see the allocations.
  if (test_mode() == TestMode::kStatic ||
      allocator_mode() == AllocatorMode::kCustom)
    GTEST_SKIP();

  auto helper = GetHelper(&task_runner);

  TraceConfig trace_config = MakeTraceConfig([this](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_process_cmdline("heapprofd_continuous_malloc");
    cfg->add_heaps(allocator_name());
  });
  trace_config.set_duration_ms(5000);

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
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(kStartupAllocSize));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::string("0"));
  StartAndWaitForHandshake(&child);

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
  // We only enable heaps on initialization of the session. The custom heap is
  // only registered later, so we do not see the allocations.
  if (test_mode() == TestMode::kStatic ||
      allocator_mode() == AllocatorMode::kCustom)
    GTEST_SKIP();

  auto helper = GetHelper(&task_runner);

  TraceConfig trace_config = MakeTraceConfig([this](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_process_cmdline("heapprofd_continuous_malloc@1.2.3");
    cfg->add_heaps(allocator_name());
  });
  trace_config.set_duration_ms(5000);

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
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(kStartupAllocSize));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::string("0"));

  StartAndWaitForHandshake(&child);

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

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(kStartupAllocSize));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::string("0"));

  StartAndWaitForHandshake(&child);

  // Wait to make sure process is fully initialized, so we do not accidentally
  // match it by the startup logic.
  sleep(1);

  TraceConfig trace_config = MakeTraceConfig([this](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_process_cmdline("heapprofd_continuous_malloc");
    cfg->add_heaps(allocator_name());
  });
  trace_config.set_duration_ms(5000);

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

  // Make sure the forked process does not get reparented to init.
  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(kStartupAllocSize));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::string("0"));

  StartAndWaitForHandshake(&child);

  // Wait to make sure process is fully initialized, so we do not accidentally
  // match it by the startup logic.
  sleep(1);

  TraceConfig trace_config = MakeTraceConfig([this](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_process_cmdline("heapprofd_continuous_malloc@1.2.3");
    cfg->add_heaps(allocator_name());
  });
  trace_config.set_duration_ms(5000);

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
  // We cannot use base::Pipe because that assumes we want CLOEXEC.
  // We do NOT want CLOEXEC as this gets used by the RunReInit in the child.
  int signal_pipe[2];
  int ack_pipe[2];

  PERFETTO_CHECK(pipe(signal_pipe) == 0);  // NOLINT(android-cloexec-pipe)
  PERFETTO_CHECK(pipe(ack_pipe) == 0);     // NOLINT(android-cloexec-pipe)

  int cur_flags = fcntl(signal_pipe[0], F_GETFL, 0);
  PERFETTO_CHECK(cur_flags >= 0);
  PERFETTO_CHECK(fcntl(signal_pipe[0], F_SETFL, cur_flags | O_NONBLOCK) == 0);
  cur_flags = fcntl(signal_pipe[1], F_GETFL, 0);
  PERFETTO_CHECK(cur_flags >= 0);
  PERFETTO_CHECK(fcntl(signal_pipe[1], F_SETFL, cur_flags | O_NONBLOCK) == 0);

  int signal_pipe_rd = signal_pipe[0];
  int signal_pipe_wr = signal_pipe[1];
  int ack_pipe_rd = ack_pipe[0];
  int ack_pipe_wr = ack_pipe[1];

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.preserve_fds.push_back(signal_pipe_rd);
  child.args.preserve_fds.push_back(ack_pipe_wr);
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_REINIT_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_REINIT_ARG1=" +
                           std::to_string(signal_pipe_rd));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_REINIT_ARG2=" +
                           std::to_string(ack_pipe_wr));
  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  close(signal_pipe_rd);
  close(ack_pipe_wr);

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
  });

  auto helper = Trace(trace_config);

  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, allocator_name());
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kFirstIterationBytes);

  PERFETTO_CHECK(PERFETTO_EINTR(write(signal_pipe_wr, "1", 1)) == 1);
  close(signal_pipe_wr);
  char buf[1];
  ASSERT_EQ(PERFETTO_EINTR(read(ack_pipe_rd, buf, sizeof(buf))), 1);
  close(ack_pipe_rd);

  // A brief sleep to allow the client to notice that the profiling session is
  // to be torn down (as it rejects concurrent sessions).
  usleep(500 * kMsToUs);

  PERFETTO_LOG("HeapprofdEndToEnd::Reinit: Starting second");

  // We must keep alive the original helper because it owns the service thread.
  std::unique_ptr<TestHelper> helper2 =
      std::unique_ptr<TestHelper>(new TestHelper(&task_runner));

  helper2->ConnectConsumer();
  helper2->WaitForConsumerConnect();
  helper2->StartTracing(trace_config);
  helper2->WaitForTracingDisabled(kTracingDisabledTimeoutMs);

  helper2->ReadData();
  helper2->WaitForReadData(0, kWaitForReadDataTimeoutMs);

  PrintStats(helper2.get());
  ValidateHasSamples(helper2.get(), pid, allocator_name());
  ValidateOnlyPID(helper2.get(), pid);
  ValidateSampleSizes(helper2.get(), pid, kSecondIterationBytes);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, ReInitAfterInvalid) {
  // We cannot use base::Pipe because that assumes we want CLOEXEC.
  // We do NOT want CLOEXEC as this gets used by the RunReInit in the child.
  int signal_pipe[2];
  int ack_pipe[2];

  PERFETTO_CHECK(pipe(signal_pipe) == 0);  // NOLINT(android-cloexec-pipe)
  PERFETTO_CHECK(pipe(ack_pipe) == 0);     // NOLINT(android-cloexec-pipe)

  int cur_flags = fcntl(signal_pipe[0], F_GETFL, 0);
  PERFETTO_CHECK(cur_flags >= 0);
  PERFETTO_CHECK(fcntl(signal_pipe[0], F_SETFL, cur_flags | O_NONBLOCK) == 0);
  cur_flags = fcntl(signal_pipe[1], F_GETFL, 0);
  PERFETTO_CHECK(cur_flags >= 0);
  PERFETTO_CHECK(fcntl(signal_pipe[1], F_SETFL, cur_flags | O_NONBLOCK) == 0);

  int signal_pipe_rd = signal_pipe[0];
  int signal_pipe_wr = signal_pipe[1];
  int ack_pipe_rd = ack_pipe[0];
  int ack_pipe_wr = ack_pipe[1];

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.preserve_fds.push_back(signal_pipe_rd);
  child.args.preserve_fds.push_back(ack_pipe_wr);
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_REINIT_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_REINIT_ARG1=" +
                           std::to_string(signal_pipe_rd));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_REINIT_ARG2=" +
                           std::to_string(ack_pipe_wr));
  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  close(signal_pipe_rd);
  close(ack_pipe_wr);

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
  });

  auto helper = Trace(trace_config);

  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, allocator_name());
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kFirstIterationBytes);

  PERFETTO_CHECK(PERFETTO_EINTR(write(signal_pipe_wr, "1", 1)) == 1);
  close(signal_pipe_wr);
  char buf[1];
  ASSERT_EQ(PERFETTO_EINTR(read(ack_pipe_rd, buf, sizeof(buf))), 1);
  close(ack_pipe_rd);

  // A brief sleep to allow the client to notice that the profiling session is
  // to be torn down (as it rejects concurrent sessions).
  usleep(500 * kMsToUs);

  PERFETTO_LOG("HeapprofdEndToEnd::Reinit: Starting second");

  // We must keep alive the original helper because it owns the service thread.
  std::unique_ptr<TestHelper> helper2 =
      std::unique_ptr<TestHelper>(new TestHelper(&task_runner));

  helper2->ConnectConsumer();
  helper2->WaitForConsumerConnect();
  helper2->StartTracing(trace_config);
  helper2->WaitForTracingDisabled(kTracingDisabledTimeoutMs);

  helper2->ReadData();
  helper2->WaitForReadData(0, kWaitForReadDataTimeoutMs);

  PrintStats(helper2.get());
  ValidateHasSamples(helper2.get(), pid, allocator_name());
  ValidateOnlyPID(helper2.get(), pid);
  ValidateSampleSizes(helper2.get(), pid, kSecondIterationBytes);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, ConcurrentSession) {
  constexpr size_t kAllocSize = 1024;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
    ContinuousDump(cfg);
  });
  trace_config.set_duration_ms(5000);

  auto helper = GetHelper(&task_runner);
  helper->StartTracing(trace_config);
  sleep(1);

  PERFETTO_LOG("Starting concurrent.");
  std::unique_ptr<TestHelper> helper_concurrent(new TestHelper(&task_runner));
  helper_concurrent->ConnectConsumer();
  helper_concurrent->WaitForConsumerConnect();
  helper_concurrent->StartTracing(trace_config);

  helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);
  helper->ReadData();
  helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);
  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, allocator_name());
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kAllocSize);
  ValidateRejectedConcurrent(helper_concurrent.get(), pid, false);

  helper_concurrent->WaitForTracingDisabled(kTracingDisabledTimeoutMs);
  helper_concurrent->ReadData();
  helper_concurrent->WaitForReadData(0, kWaitForReadDataTimeoutMs);
  PrintStats(helper.get());
  ValidateOnlyPID(helper_concurrent.get(), pid);
  ValidateRejectedConcurrent(helper_concurrent.get(), pid, true);

  KillAssertRunning(&child);
}

TEST_P(HeapprofdEndToEnd, NativeProfilingActiveAtProcessExit) {
  constexpr uint64_t kTestAllocSize = 128;
  base::Pipe start_pipe = base::Pipe::Create(base::Pipe::kBothBlock);
  int start_pipe_wr = *start_pipe.wr;

  base::Subprocess child({"/proc/self/exe"});
  child.args.argv0_override = "heapprofd_continuous_malloc";
  child.args.stdout_mode = base::Subprocess::kDevNull;
  child.args.stderr_mode = base::Subprocess::kDevNull;
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(kTestAllocSize));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::to_string(0));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG3=" +
                           std::to_string(200));
  child.args.preserve_fds.push_back(start_pipe_wr);
  child.args.entrypoint_for_testing = [start_pipe_wr] {
    PERFETTO_CHECK(PERFETTO_EINTR(write(start_pipe_wr, "1", 1)) == 1);
    PERFETTO_CHECK(close(start_pipe_wr) == 0 || errno == EINTR);
  };

  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());
  start_pipe.wr.reset();

  // Construct tracing config (without starting profiling).
  auto helper = GetHelper(&task_runner);

  // Wait for child to have been scheduled at least once.
  char buf[1] = {};
  ASSERT_EQ(PERFETTO_EINTR(read(*start_pipe.rd, buf, sizeof(buf))), 1);
  start_pipe.rd.reset();

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
  });
  trace_config.set_duration_ms(5000);

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
      EXPECT_EQ(dump.pid(), pid);
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

// On in-tree Android, we use the system heapprofd in fork or central mode.
// For Linux and out-of-tree Android, we statically include a copy of
// heapprofd and use that. This one does not support intercepting malloc.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#if !PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#error "Need to start daemons for Linux test."
#endif

INSTANTIATE_TEST_CASE_P(Run,
                        HeapprofdEndToEnd,
                        Values(std::make_tuple(TestMode::kStatic,
                                               AllocatorMode::kCustom)),
                        TestSuffix);
#elif !PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
INSTANTIATE_TEST_CASE_P(
    Run,
    HeapprofdEndToEnd,
    Values(std::make_tuple(TestMode::kCentral, AllocatorMode::kMalloc),
           std::make_tuple(TestMode::kFork, AllocatorMode::kMalloc),
           std::make_tuple(TestMode::kCentral, AllocatorMode::kCustom),
           std::make_tuple(TestMode::kFork, AllocatorMode::kCustom)),
    TestSuffix);
#endif

}  // namespace
}  // namespace profiling
}  // namespace perfetto
