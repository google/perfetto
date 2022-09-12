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

#include <atomic>
#include <string>
#include <vector>

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/tracing/ipc/default_socket.h"
#include "perfetto/heap_profile.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "src/base/test/test_task_runner.h"
#include "src/profiling/memory/heapprofd_producer.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/system_properties.h>
#endif

#include "protos/perfetto/config/profiling/heapprofd_config.gen.h"
#include "protos/perfetto/trace/interned_data/interned_data.gen.h"
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

enum class TestMode { kCentral, kStatic };
enum class AllocatorMode { kMalloc, kCustom };

using ::testing::AllOf;
using ::testing::AnyOf;
using ::testing::Bool;
using ::testing::Contains;
using ::testing::Eq;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::Values;

constexpr const char* kOnlyFlamegraph =
    "SELECT id, name, map_name, count, cumulative_count, size, "
    "cumulative_size, "
    "alloc_count, cumulative_alloc_count, alloc_size, cumulative_alloc_size, "
    "parent_id "
    "FROM experimental_flamegraph WHERE "
    "(ts, upid) IN (SELECT distinct ts, upid from heap_profile_allocation) AND "
    "profile_type = 'native' order by abs(cumulative_size) desc;";

struct FlamegraphNode {
  int64_t id;
  std::string name;
  std::string map_name;
  int64_t count;
  int64_t cumulative_count;
  int64_t size;
  int64_t cumulative_size;
  int64_t alloc_count;
  int64_t cumulative_alloc_count;
  int64_t alloc_size;
  int64_t cumulative_alloc_size;
  base::Optional<int64_t> parent_id;
};

std::vector<FlamegraphNode> GetFlamegraph(trace_processor::TraceProcessor* tp) {
  std::vector<FlamegraphNode> result;
  auto it = tp->ExecuteQuery(kOnlyFlamegraph);
  while (it.Next()) {
    result.push_back({
        it.Get(0).AsLong(),
        it.Get(1).AsString(),
        it.Get(2).AsString(),
        it.Get(3).AsLong(),
        it.Get(4).AsLong(),
        it.Get(5).AsLong(),
        it.Get(6).AsLong(),
        it.Get(7).AsLong(),
        it.Get(8).AsLong(),
        it.Get(9).AsLong(),
        it.Get(10).AsLong(),
        it.Get(11).is_null() ? base::nullopt
                             : base::Optional<int64_t>(it.Get(11).AsLong()),
    });
  }
  PERFETTO_CHECK(it.Status().ok());
  return result;
}

std::string AllocatorName(AllocatorMode mode) {
  switch (mode) {
    case AllocatorMode::kMalloc:
      return "libc.malloc";
    case AllocatorMode::kCustom:
      return "test";
  }
}

AllocatorMode AllocatorModeFromNameOrDie(std::string s) {
  if (s == "libc.malloc")
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
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
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
  static uint32_t heap_id =
      AHeapProfile_registerHeap(AHeapInfo_setEnabledCallback(
          AHeapInfo_create("test"),
          [](void*, const AHeapProfileEnableCallbackInfo*) {
            initialized = true;
          },
          nullptr));

  ChildFinishHandshake();

  // heapprofd_client needs malloc to see the signal.
  while (!initialized)
    AllocateAndFree(1);
  // We call the callback before setting enabled=true on the heap, so we
  // wait a bit for the assignment to happen.
  usleep(100000);
  if (!AHeapProfile_reportAllocation(heap_id, 0x1, 10u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  AHeapProfile_reportFree(heap_id, 0x1);
  if (!AHeapProfile_reportAllocation(heap_id, 0x2, 15u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  if (!AHeapProfile_reportAllocation(heap_id, 0x3, 15u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  AHeapProfile_reportFree(heap_id, 0x2);

  // Wait around so we can verify it did't crash.
  for (;;) {
    // Call sleep, otherwise an empty busy loop is undefined behavior:
    // http://en.cppreference.com/w/cpp/language/memory_model#Progress_guarantee
    sleep(1);
  }
}

void __attribute__((noreturn)) RunAccurateMallocWithVforkCommon() {
  static std::atomic<bool> initialized{false};
  static uint32_t heap_id =
      AHeapProfile_registerHeap(AHeapInfo_setEnabledCallback(
          AHeapInfo_create("test"),
          [](void*, const AHeapProfileEnableCallbackInfo*) {
            initialized = true;
          },
          nullptr));

  ChildFinishHandshake();

  // heapprofd_client needs malloc to see the signal.
  while (!initialized)
    AllocateAndFree(1);
  // We call the callback before setting enabled=true on the heap, so we
  // wait a bit for the assignment to happen.
  usleep(100000);
  if (!AHeapProfile_reportAllocation(heap_id, 0x1, 10u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  AHeapProfile_reportFree(heap_id, 0x1);
  pid_t pid = vfork();
  PERFETTO_CHECK(pid != -1);
  if (pid == 0) {
    AHeapProfile_reportAllocation(heap_id, 0x2, 15u);
    AHeapProfile_reportAllocation(heap_id, 0x3, 15u);
    exit(0);
  }
  if (!AHeapProfile_reportAllocation(heap_id, 0x2, 15u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  if (!AHeapProfile_reportAllocation(heap_id, 0x3, 15u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  AHeapProfile_reportFree(heap_id, 0x2);

  // Wait around so we can verify it did't crash.
  for (;;) {
    // Call sleep, otherwise an empty busy loop is undefined behavior:
    // http://en.cppreference.com/w/cpp/language/memory_model#Progress_guarantee
    sleep(1);
  }
}

void __attribute__((constructor(1024))) RunAccurateSample() {
  const char* a0 = getenv("HEAPPROFD_TESTING_RUN_ACCURATE_SAMPLE");
  if (a0 == nullptr)
    return;

  static std::atomic<bool> initialized{false};
  static uint32_t heap_id =
      AHeapProfile_registerHeap(AHeapInfo_setEnabledCallback(
          AHeapInfo_create("test"),
          [](void*, const AHeapProfileEnableCallbackInfo*) {
            initialized = true;
          },
          nullptr));

  ChildFinishHandshake();

  // heapprofd_client needs malloc to see the signal.
  while (!initialized)
    AllocateAndFree(1);
  // We call the callback before setting enabled=true on the heap, so we
  // wait a bit for the assignment to happen.
  usleep(100000);
  if (!AHeapProfile_reportSample(heap_id, 0x1, 10u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  AHeapProfile_reportFree(heap_id, 0x1);
  if (!AHeapProfile_reportSample(heap_id, 0x2, 15u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  if (!AHeapProfile_reportSample(heap_id, 0x3, 15u))
    PERFETTO_FATAL("Expected allocation to be sampled.");
  AHeapProfile_reportFree(heap_id, 0x2);

  // Wait around so we can verify it did't crash.
  for (;;) {
    // Call sleep, otherwise an empty busy loop is undefined behavior:
    // http://en.cppreference.com/w/cpp/language/memory_model#Progress_guarantee
    sleep(1);
  }
}

void __attribute__((constructor(1024))) RunAccurateMallocWithVfork() {
  const char* a0 = getenv("HEAPPROFD_TESTING_RUN_ACCURATE_MALLOC_WITH_VFORK");
  if (a0 == nullptr)
    return;
  RunAccurateMallocWithVforkCommon();
}

void __attribute__((constructor(1024))) RunAccurateMallocWithVforkThread() {
  const char* a0 =
      getenv("HEAPPROFD_TESTING_RUN_ACCURATE_MALLOC_WITH_VFORK_THREAD");
  if (a0 == nullptr)
    return;
  std::thread th(RunAccurateMallocWithVforkCommon);
  th.join();
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

void __attribute__((constructor(1024))) RunCustomLifetime() {
  const char* a0 = getenv("HEAPPROFD_TESTING_RUN_LIFETIME_ARG0");
  const char* a1 = getenv("HEAPPROFD_TESTING_RUN_LIFETIME_ARG1");
  if (a0 == nullptr)
    return;
  uint64_t arg0 = a0 ? base::StringToUInt64(a0).value() : 0;
  uint64_t arg1 = a0 ? base::StringToUInt64(a1).value() : 0;

  PERFETTO_CHECK(arg1);

  static std::atomic<bool> initialized{false};
  static std::atomic<bool> disabled{false};
  static std::atomic<uint64_t> sampling_interval;

  static uint32_t other_heap_id = 0;
  auto enabled_callback = [](void*,
                             const AHeapProfileEnableCallbackInfo* info) {
    sampling_interval =
        AHeapProfileEnableCallbackInfo_getSamplingInterval(info);
    initialized = true;
  };
  auto disabled_callback = [](void*, const AHeapProfileDisableCallbackInfo*) {
    PERFETTO_CHECK(other_heap_id);
    AHeapProfile_reportFree(other_heap_id, 0);
    disabled = true;
  };
  static uint32_t heap_id =
      AHeapProfile_registerHeap(AHeapInfo_setDisabledCallback(
          AHeapInfo_setEnabledCallback(AHeapInfo_create("test"),
                                       enabled_callback, nullptr),
          disabled_callback, nullptr));

  other_heap_id = AHeapProfile_registerHeap(AHeapInfo_create("othertest"));
  ChildFinishHandshake();

  // heapprofd_client needs malloc to see the signal.
  while (!initialized)
    AllocateAndFree(1);

  if (sampling_interval.load() != arg0) {
    PERFETTO_FATAL("%" PRIu64 " != %" PRIu64, sampling_interval.load(), arg0);
  }

  while (!disabled)
    AHeapProfile_reportFree(heap_id, 0x2);

  char x = 'x';
  PERFETTO_CHECK(base::WriteAll(static_cast<int>(arg1), &x, sizeof(x)) == 1);
  close(static_cast<int>(arg1));

  // Wait around so we can verify it didn't crash.
  for (;;) {
    // Call sleep, otherwise an empty busy loop is undefined behavior:
    // http://en.cppreference.com/w/cpp/language/memory_model#Progress_guarantee
    sleep(1);
  }
}

class TraceProcessorTestHelper : public TestHelper {
 public:
  explicit TraceProcessorTestHelper(base::TestTaskRunner* task_runner)
      : TestHelper(task_runner),
        tp_(trace_processor::TraceProcessor::CreateInstance({})) {}

  void ReadTraceData(std::vector<TracePacket> packets) override {
    for (auto& packet : packets) {
      auto preamble = packet.GetProtoPreamble();
      std::string payload = packet.GetRawBytesForTesting();
      char* preamble_payload = std::get<0>(preamble);
      size_t preamble_size = std::get<1>(preamble);
      size_t buf_size = preamble_size + payload.size();
      std::unique_ptr<uint8_t[]> buf =
          std::unique_ptr<uint8_t[]>(new uint8_t[buf_size]);
      memcpy(&buf[0], preamble_payload, preamble_size);
      memcpy(&buf[preamble_size], payload.data(), payload.size());
      PERFETTO_CHECK(tp_->Parse(std::move(buf), buf_size).ok());
    }
    TestHelper::ReadTraceData(std::move(packets));
  }

  trace_processor::TraceProcessor& tp() { return *tp_; }

 private:
  std::unique_ptr<trace_processor::TraceProcessor> tp_;
};

std::unique_ptr<TraceProcessorTestHelper> GetHelper(
    base::TestTaskRunner* task_runner) {
  std::unique_ptr<TraceProcessorTestHelper> helper(
      new TraceProcessorTestHelper(task_runner));
  helper->StartServiceIfRequired();

  helper->ConnectConsumer();
  helper->WaitForConsumerConnect();
  return helper;
}

void ReadAndWait(TraceProcessorTestHelper* helper) {
  helper->WaitForTracingDisabled(kTracingDisabledTimeoutMs);
  helper->ReadData();
  helper->WaitForReadData(0, kWaitForReadDataTimeoutMs);
  helper->tp().NotifyEndOfFile();
}

std::string ToTraceString(
    const std::vector<protos::gen::TracePacket>& packets) {
  protos::gen::Trace trace;
  for (const protos::gen::TracePacket& packet : packets) {
    *trace.add_packet() = packet;
  }
  return trace.SerializeAsString();
}

#define WRITE_TRACE(trace)                 \
  do {                                     \
    WriteTrace(trace, __FILE__, __LINE__); \
  } while (0)

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

std::string Suffix(const std::tuple<TestMode, AllocatorMode>& param) {
  TestMode tm = std::get<0>(param);
  AllocatorMode am = std::get<1>(param);

  std::string result;
  switch (tm) {
    case TestMode::kCentral:
      result += "CentralMode";
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

__attribute__((unused)) std::string TestSuffix(
    const ::testing::TestParamInfo<std::tuple<TestMode, AllocatorMode>>& info) {
  return Suffix(info.param);
}

class HeapprofdEndToEnd
    : public ::testing::TestWithParam<std::tuple<TestMode, AllocatorMode>> {
 protected:
  base::TestTaskRunner task_runner;

  TestMode test_mode() { return std::get<0>(GetParam()); }
  AllocatorMode allocator_mode() { return std::get<1>(GetParam()); }
  std::string allocator_name() { return AllocatorName(allocator_mode()); }

  void WriteTrace(const std::vector<protos::gen::TracePacket>& packets,
                  const char* filename,
                  uint64_t lineno) {
    const char* outdir = getenv("HEAPPROFD_TEST_PROFILE_OUT");
    if (!outdir)
      return;
    const std::string fq_filename =
        std::string(outdir) + "/" + basename(filename) + ":" +
        std::to_string(lineno) + "_" + Suffix(GetParam());
    base::ScopedFile fd(base::OpenFile(fq_filename, O_WRONLY | O_CREAT, 0666));
    PERFETTO_CHECK(*fd);
    std::string trace_string = ToTraceString(packets);
    PERFETTO_CHECK(
        base::WriteAll(*fd, trace_string.data(), trace_string.size()) >= 0);
  }

  std::unique_ptr<TraceProcessorTestHelper> Trace(
      const TraceConfig& trace_config) {
    auto helper = GetHelper(&task_runner);

    helper->StartTracing(trace_config);

    ReadAndWait(helper.get());
    return helper;
  }

  std::vector<std::string> GetUnwindingErrors(
      TraceProcessorTestHelper* helper) {
    std::vector<std::string> out;
    const auto& packets = helper->trace();
    for (const protos::gen::TracePacket& packet : packets) {
      for (const protos::gen::InternedString& fn :
           packet.interned_data().function_names()) {
        if (fn.str().find("ERROR ") == 0) {
          out.push_back(fn.str());
        }
      }
    }
    return out;
  }

  void PrintStats(TraceProcessorTestHelper* helper) {
    const auto& packets = helper->trace();
    for (const protos::gen::TracePacket& packet : packets) {
      for (const auto& dump : packet.profile_packet().process_dumps()) {
        // protobuf uint64 does not like the PRIu64 formatter.
        PERFETTO_LOG("Stats for %s: %s", std::to_string(dump.pid()).c_str(),
                     FormatStats(dump.stats()).c_str());
      }
    }
    std::vector<std::string> errors = GetUnwindingErrors(helper);
    for (const std::string& err : errors) {
      PERFETTO_LOG("Unwinding error: %s", err.c_str());
    }
  }

  void ValidateSampleSizes(TraceProcessorTestHelper* helper,
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

  void ValidateFromStartup(TraceProcessorTestHelper* helper,
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

  void ValidateRejectedConcurrent(TraceProcessorTestHelper* helper,
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

  void ValidateNoSamples(TraceProcessorTestHelper* helper, uint64_t pid) {
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

  void ValidateHasSamples(TraceProcessorTestHelper* helper,
                          uint64_t pid,
                          const std::string& heap_name,
                          uint64_t sampling_interval) {
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
        EXPECT_EQ(dump.sampling_interval_bytes(), sampling_interval);
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

  void ValidateOnlyPID(TraceProcessorTestHelper* helper, uint64_t pid) {
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
  ASSERT_EQ(child->Poll(), base::Subprocess::kRunning)
      << "Target process not running. CHECK CRASH LOGS.";
  PERFETTO_LOG("Shutting down profile target.");
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
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

  ValidateNoSamples(helper.get(), pid);
}

TEST_P(HeapprofdEndToEnd, Smoke) {
  constexpr size_t kAllocSize = 1024;
  constexpr size_t kSamplingInterval = 1;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(kSamplingInterval);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
    ContinuousDump(cfg);
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

  ValidateHasSamples(helper.get(), pid, allocator_name(), kSamplingInterval);
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kAllocSize);
}

TEST_P(HeapprofdEndToEnd, TwoAllocators) {
  constexpr size_t kCustomAllocSize = 1024;
  constexpr size_t kAllocSize = 7;
  constexpr size_t kSamplingInterval = 1;

  base::Subprocess child =
      ForkContinuousAlloc(allocator_mode(), kAllocSize, kCustomAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(kSamplingInterval);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
    cfg->add_heaps("secondary");
    ContinuousDump(cfg);
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

  ValidateHasSamples(helper.get(), pid, "secondary", kSamplingInterval);
  ValidateHasSamples(helper.get(), pid, allocator_name(), kSamplingInterval);
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kCustomAllocSize, "secondary");
  ValidateSampleSizes(helper.get(), pid, kAllocSize, allocator_name());
}

TEST_P(HeapprofdEndToEnd, TwoAllocatorsAll) {
  constexpr size_t kCustomAllocSize = 1024;
  constexpr size_t kAllocSize = 7;
  constexpr size_t kSamplingInterval = 1;

  base::Subprocess child =
      ForkContinuousAlloc(allocator_mode(), kAllocSize, kCustomAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(kSamplingInterval);
    cfg->add_pid(pid);
    cfg->set_all_heaps(true);
    ContinuousDump(cfg);
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

  ValidateHasSamples(helper.get(), pid, "secondary", kSamplingInterval);
  ValidateHasSamples(helper.get(), pid, allocator_name(), kSamplingInterval);
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kCustomAllocSize, "secondary");
  ValidateSampleSizes(helper.get(), pid, kAllocSize, allocator_name());
}

TEST_P(HeapprofdEndToEnd, AccurateCustomReportAllocation) {
  if (allocator_mode() != AllocatorMode::kCustom)
    GTEST_SKIP();

  base::Subprocess child({"/proc/self/exe"});
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_ACCURATE_MALLOC=1");
  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps("test");
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

  auto flamegraph = GetFlamegraph(&helper->tp());
  EXPECT_THAT(flamegraph,
              Contains(AllOf(
                  Field(&FlamegraphNode::name, HasSubstr("RunAccurateMalloc")),
                  Field(&FlamegraphNode::cumulative_size, Eq(15)),
                  Field(&FlamegraphNode::cumulative_alloc_size, Eq(40)))));

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
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_AccurateCustomReportAllocationWithVfork \
  AccurateCustomReportAllocationWithVfork
#define MAYBE_AccurateCustomReportAllocationWithVforkThread \
  AccurateCustomReportAllocationWithVforkThread
#else
#define MAYBE_AccurateCustomReportAllocationWithVfork \
  DISABLED_AccurateCustomReportAllocationWithVfork
#define MAYBE_AccurateCustomReportAllocationWithVforkThread \
  DISABLED_AccurateCustomReportAllocationWithVforkThread
#endif

TEST_P(HeapprofdEndToEnd, MAYBE_AccurateCustomReportAllocationWithVfork) {
  if (allocator_mode() != AllocatorMode::kCustom)
    GTEST_SKIP();

  base::Subprocess child({"/proc/self/exe"});
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
  child.args.env.push_back(
      "HEAPPROFD_TESTING_RUN_ACCURATE_MALLOC_WITH_VFORK=1");
  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps("test");
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

  auto flamegraph = GetFlamegraph(&helper->tp());
  EXPECT_THAT(flamegraph,
              Contains(AllOf(
                  Field(&FlamegraphNode::name, HasSubstr("RunAccurateMalloc")),
                  Field(&FlamegraphNode::cumulative_size, Eq(15)),
                  Field(&FlamegraphNode::cumulative_alloc_size, Eq(40)))));

  ValidateOnlyPID(helper.get(), pid);

  size_t total_alloc = 0;
  size_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : helper->trace()) {
    for (const auto& dump : packet.profile_packet().process_dumps()) {
      EXPECT_FALSE(dump.disconnected());
      for (const auto& sample : dump.samples()) {
        total_alloc += sample.self_allocated();
        total_freed += sample.self_freed();
      }
    }
  }
  EXPECT_EQ(total_alloc, 40u);
  EXPECT_EQ(total_freed, 25u);
}

TEST_P(HeapprofdEndToEnd, MAYBE_AccurateCustomReportAllocationWithVforkThread) {
  if (allocator_mode() != AllocatorMode::kCustom)
    GTEST_SKIP();

  base::Subprocess child({"/proc/self/exe"});
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
  child.args.env.push_back(
      "HEAPPROFD_TESTING_RUN_ACCURATE_MALLOC_WITH_VFORK_THREAD=1");
  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1);
    cfg->add_pid(pid);
    cfg->add_heaps("test");
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

  auto flamegraph = GetFlamegraph(&helper->tp());
  EXPECT_THAT(flamegraph,
              Contains(AllOf(
                  Field(&FlamegraphNode::name, HasSubstr("RunAccurateMalloc")),
                  Field(&FlamegraphNode::cumulative_size, Eq(15)),
                  Field(&FlamegraphNode::cumulative_alloc_size, Eq(40)))));

  ValidateOnlyPID(helper.get(), pid);

  size_t total_alloc = 0;
  size_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : helper->trace()) {
    for (const auto& dump : packet.profile_packet().process_dumps()) {
      EXPECT_FALSE(dump.disconnected());
      for (const auto& sample : dump.samples()) {
        total_alloc += sample.self_allocated();
        total_freed += sample.self_freed();
      }
    }
  }
  EXPECT_EQ(total_alloc, 40u);
  EXPECT_EQ(total_freed, 25u);
}

TEST_P(HeapprofdEndToEnd, AccurateCustomReportSample) {
  if (allocator_mode() != AllocatorMode::kCustom)
    GTEST_SKIP();

  base::Subprocess child({"/proc/self/exe"});
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_ACCURATE_SAMPLE=1");
  StartAndWaitForHandshake(&child);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1000000);
    cfg->add_pid(pid);
    cfg->add_heaps("test");
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

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
}

TEST_P(HeapprofdEndToEnd, AccurateDumpAtMaxCustom) {
  if (allocator_mode() != AllocatorMode::kCustom)
    GTEST_SKIP();

  base::Subprocess child({"/proc/self/exe"});
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
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
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

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
}

TEST_P(HeapprofdEndToEnd, CustomLifetime) {
  if (allocator_mode() != AllocatorMode::kCustom)
    GTEST_SKIP();

  int disabled_pipe[2];
  PERFETTO_CHECK(pipe(disabled_pipe) == 0);  // NOLINT(android-cloexec-pipe)

  int disabled_pipe_rd = disabled_pipe[0];
  int disabled_pipe_wr = disabled_pipe[1];

  base::Subprocess child({"/proc/self/exe"});
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_LIFETIME_ARG0=1000000");
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_LIFETIME_ARG1=" +
                           std::to_string(disabled_pipe_wr));
  child.args.preserve_fds.push_back(disabled_pipe_wr);
  StartAndWaitForHandshake(&child);
  close(disabled_pipe_wr);

  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(1000000);
    cfg->add_pid(pid);
    cfg->add_heaps("test");
    cfg->add_heaps("othertest");
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  // Give client some time to notice the disconnect.
  sleep(2);
  KillAssertRunning(&child);

  char x;
  EXPECT_EQ(base::Read(disabled_pipe_rd, &x, sizeof(x)), 1);
  close(disabled_pipe_rd);
}

TEST_P(HeapprofdEndToEnd, TwoProcesses) {
  constexpr size_t kAllocSize = 1024;
  constexpr size_t kAllocSize2 = 7;
  constexpr size_t kSamplingInterval = 1;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  base::Subprocess child2 = ForkContinuousAlloc(allocator_mode(), kAllocSize2);
  const uint64_t pid = static_cast<uint64_t>(child.pid());
  const auto pid2 = child2.pid();

  TraceConfig trace_config =
      MakeTraceConfig([this, pid, pid2](HeapprofdConfig* cfg) {
        cfg->set_sampling_interval_bytes(kSamplingInterval);
        cfg->add_pid(pid);
        cfg->add_pid(static_cast<uint64_t>(pid2));
        cfg->add_heaps(allocator_name());
      });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());

  KillAssertRunning(&child);
  KillAssertRunning(&child2);

  ValidateHasSamples(helper.get(), pid, allocator_name(), kSamplingInterval);
  ValidateSampleSizes(helper.get(), pid, kAllocSize);
  ValidateHasSamples(helper.get(), static_cast<uint64_t>(pid2),
                     allocator_name(), kSamplingInterval);
  ValidateSampleSizes(helper.get(), static_cast<uint64_t>(pid2), kAllocSize2);
}

TEST_P(HeapprofdEndToEnd, FinalFlush) {
  constexpr size_t kAllocSize = 1024;
  constexpr size_t kSamplingInterval = 1;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());
  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(kSamplingInterval);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());
  KillAssertRunning(&child);

  ValidateHasSamples(helper.get(), pid, allocator_name(), kSamplingInterval);
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kAllocSize);
}

TEST_P(HeapprofdEndToEnd, NativeStartup) {
  if (test_mode() == TestMode::kStatic)
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
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(kStartupAllocSize));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::string("0"));
  StartAndWaitForHandshake(&child);

  ReadAndWait(helper.get());
  WRITE_TRACE(helper->full_trace());

  KillAssertRunning(&child);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  uint64_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        !packet.profile_packet().process_dumps().empty()) {
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
  if (test_mode() == TestMode::kStatic)
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
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(kStartupAllocSize));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::string("0"));

  StartAndWaitForHandshake(&child);

  ReadAndWait(helper.get());
  WRITE_TRACE(helper->full_trace());

  KillAssertRunning(&child);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  uint64_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        !packet.profile_packet().process_dumps().empty()) {
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
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
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
  ReadAndWait(helper.get());
  WRITE_TRACE(helper->full_trace());

  KillAssertRunning(&child);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  uint64_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        !packet.profile_packet().process_dumps().empty()) {
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
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
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
  ReadAndWait(helper.get());
  WRITE_TRACE(helper->full_trace());

  KillAssertRunning(&child);

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  uint64_t total_freed = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        !packet.profile_packet().process_dumps().empty()) {
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
  constexpr size_t kSamplingInterval = 1;

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
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
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
    cfg->set_sampling_interval_bytes(kSamplingInterval);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());

  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, allocator_name(), kSamplingInterval);
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
  std::unique_ptr<TraceProcessorTestHelper> helper2 =
      std::unique_ptr<TraceProcessorTestHelper>(
          new TraceProcessorTestHelper(&task_runner));

  helper2->ConnectConsumer();
  helper2->WaitForConsumerConnect();
  helper2->StartTracing(trace_config);
  ReadAndWait(helper2.get());
  WRITE_TRACE(helper2->trace());

  PrintStats(helper2.get());
  KillAssertRunning(&child);

  ValidateHasSamples(helper2.get(), pid, allocator_name(), kSamplingInterval);
  ValidateOnlyPID(helper2.get(), pid);
  ValidateSampleSizes(helper2.get(), pid, kSecondIterationBytes);
}

TEST_P(HeapprofdEndToEnd, ReInitAfterInvalid) {
  constexpr size_t kSamplingInterval = 1;

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
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
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
    cfg->set_sampling_interval_bytes(kSamplingInterval);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
  });

  auto helper = Trace(trace_config);
  WRITE_TRACE(helper->full_trace());

  PrintStats(helper.get());
  ValidateHasSamples(helper.get(), pid, allocator_name(), kSamplingInterval);
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
  std::unique_ptr<TraceProcessorTestHelper> helper2 =
      std::unique_ptr<TraceProcessorTestHelper>(
          new TraceProcessorTestHelper(&task_runner));

  helper2->ConnectConsumer();
  helper2->WaitForConsumerConnect();
  helper2->StartTracing(trace_config);
  ReadAndWait(helper2.get());

  WRITE_TRACE(helper2->trace());

  PrintStats(helper2.get());
  KillAssertRunning(&child);

  ValidateHasSamples(helper2.get(), pid, allocator_name(), kSamplingInterval);
  ValidateOnlyPID(helper2.get(), pid);
  ValidateSampleSizes(helper2.get(), pid, kSecondIterationBytes);
}

TEST_P(HeapprofdEndToEnd, ConcurrentSession) {
  constexpr size_t kAllocSize = 1024;
  constexpr size_t kSamplingInterval = 1;

  base::Subprocess child = ForkContinuousAlloc(allocator_mode(), kAllocSize);
  const uint64_t pid = static_cast<uint64_t>(child.pid());

  TraceConfig trace_config = MakeTraceConfig([this, pid](HeapprofdConfig* cfg) {
    cfg->set_sampling_interval_bytes(kSamplingInterval);
    cfg->add_pid(pid);
    cfg->add_heaps(allocator_name());
    ContinuousDump(cfg);
  });
  trace_config.set_duration_ms(5000);

  auto helper = GetHelper(&task_runner);
  helper->StartTracing(trace_config);
  sleep(1);

  PERFETTO_LOG("Starting concurrent.");
  std::unique_ptr<TraceProcessorTestHelper> helper_concurrent(
      new TraceProcessorTestHelper(&task_runner));
  helper_concurrent->ConnectConsumer();
  helper_concurrent->WaitForConsumerConnect();
  helper_concurrent->StartTracing(trace_config);

  ReadAndWait(helper.get());
  WRITE_TRACE(helper->full_trace());
  PrintStats(helper.get());

  ReadAndWait(helper_concurrent.get());
  WRITE_TRACE(helper_concurrent->trace());
  PrintStats(helper_concurrent.get());
  KillAssertRunning(&child);

  ValidateHasSamples(helper.get(), pid, allocator_name(), kSamplingInterval);
  ValidateOnlyPID(helper.get(), pid);
  ValidateSampleSizes(helper.get(), pid, kAllocSize);
  ValidateRejectedConcurrent(helper.get(), pid, false);

  ValidateOnlyPID(helper_concurrent.get(), pid);
  ValidateRejectedConcurrent(helper_concurrent.get(), pid, true);
}

TEST_P(HeapprofdEndToEnd, NativeProfilingActiveAtProcessExit) {
  constexpr uint64_t kTestAllocSize = 128;
  base::Pipe start_pipe = base::Pipe::Create(base::Pipe::kBothBlock);
  int start_pipe_wr = *start_pipe.wr;

  base::Subprocess child({"/proc/self/exe"});
  child.args.posix_argv0_override_for_testing = "heapprofd_continuous_malloc";
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG0=" +
                           allocator_name());
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG1=" +
                           std::to_string(kTestAllocSize));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG2=" +
                           std::to_string(0));
  child.args.env.push_back("HEAPPROFD_TESTING_RUN_MALLOC_ARG3=" +
                           std::to_string(200));
  child.args.preserve_fds.push_back(start_pipe_wr);
  child.args.posix_entrypoint_for_testing = [start_pipe_wr] {
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
  EXPECT_EQ(child.status(), base::Subprocess::kTerminated);
  EXPECT_EQ(child.returncode(), 0);

  // Assert that we did profile the process.
  helper->FlushAndWait(2000);
  helper->DisableTracing();
  ReadAndWait(helper.get());
  WRITE_TRACE(helper->full_trace());

  const auto& packets = helper->trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  size_t samples = 0;
  uint64_t total_allocated = 0;
  for (const protos::gen::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        !packet.profile_packet().process_dumps().empty()) {
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

INSTANTIATE_TEST_SUITE_P(Run,
                         HeapprofdEndToEnd,
                         Values(std::make_tuple(TestMode::kStatic,
                                                AllocatorMode::kCustom)),
                         TestSuffix);
#elif !PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
INSTANTIATE_TEST_SUITE_P(
    Run,
    HeapprofdEndToEnd,
    Values(std::make_tuple(TestMode::kCentral, AllocatorMode::kMalloc),
           std::make_tuple(TestMode::kCentral, AllocatorMode::kCustom)),
    TestSuffix);
#endif

}  // namespace
}  // namespace profiling
}  // namespace perfetto
