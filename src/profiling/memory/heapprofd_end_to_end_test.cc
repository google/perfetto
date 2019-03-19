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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/pipe.h"
#include "src/base/test/test_task_runner.h"
#include "test/test_helper.h"

#include "src/profiling/memory/heapprofd_producer.h"
#include "src/tracing/ipc/default_socket.h"

#include <sys/system_properties.h>

#include <fcntl.h>

// This test only works when run on Android using an Android Q version of
// Bionic.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#error "This test can only be used on Android."
#endif

// If we're building on Android and starting the daemons ourselves,
// create the sockets in a world-writable location.
#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#else
#define TEST_PRODUCER_SOCK_NAME ::perfetto::GetProducerSocket()
#endif

namespace perfetto {
namespace profiling {
namespace {

constexpr useconds_t kMsToUs = 1000;

using ::testing::Eq;
using ::testing::AnyOf;

void WaitForHeapprofd(uint64_t timeout_ms) {
  constexpr uint64_t kSleepMs = 10;
  std::vector<std::string> cmdlines{"heapprofd"};
  std::set<pid_t> pids;
  for (size_t i = 0; i < timeout_ms / kSleepMs && pids.empty(); ++i) {
    FindPidsForCmdlines(cmdlines, &pids);
    usleep(kSleepMs * 1000);
  }
}

class HeapprofdDelegate : public ThreadDelegate {
 public:
  HeapprofdDelegate(const std::string& producer_socket)
      : producer_socket_(producer_socket) {}
  ~HeapprofdDelegate() override = default;

  void Initialize(base::TaskRunner* task_runner) override {
    producer_.reset(
        new HeapprofdProducer(HeapprofdMode::kCentral, task_runner));
    producer_->ConnectWithRetries(producer_socket_.c_str());
  }

 private:
  std::string producer_socket_;
  std::unique_ptr<HeapprofdProducer> producer_;
};

constexpr const char* kEnableHeapprofdProperty = "persist.heapprofd.enable";
constexpr const char* kHeapprofdModeProperty = "heapprofd.userdebug.mode";

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

int __attribute__((unused)) SetModeProperty(std::string* value) {
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

int __attribute__((unused)) SetEnableProperty(std::string* value) {
  if (value) {
    __system_property_set(kEnableHeapprofdProperty, value->c_str());
    delete value;
  }
  return 0;
}

base::ScopedResource<std::string*, SetEnableProperty, nullptr>
StartSystemHeapprofdIfRequired() {
  base::ignore_result(TEST_PRODUCER_SOCK_NAME);
  std::string prev_property_value = ReadProperty(kEnableHeapprofdProperty, "0");
  __system_property_set(kEnableHeapprofdProperty, "1");
  WaitForHeapprofd(5000);
  return base::ScopedResource<std::string*, SetEnableProperty, nullptr>(
      new std::string(prev_property_value));
}

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

void __attribute__((noreturn)) ContinuousMalloc(size_t bytes) {
  for (;;) {
    AllocateAndFree(bytes);
    usleep(10 * kMsToUs);
  }
}

pid_t ForkContinuousMalloc(size_t bytes) {
  // Make sure forked process does not get reparented to init.
  setsid();
  pid_t pid = fork();
  switch (pid) {
    case -1:
      PERFETTO_FATAL("Failed to fork.");
    case 0:
      ContinuousMalloc(bytes);
    default:
      break;
  }
  return pid;
}

void __attribute__((constructor)) RunContinuousMalloc() {
  if (getenv("HEAPPROFD_TESTING_RUN_MALLOC") != nullptr)
    ContinuousMalloc(kStartupAllocSize);
}

std::unique_ptr<TestHelper> GetHelper(base::TestTaskRunner* task_runner) {
  std::unique_ptr<TestHelper> helper(new TestHelper(task_runner));
  helper->ConnectConsumer();
  helper->WaitForConsumerConnect();
  return helper;
}

class HeapprofdEndToEnd : public ::testing::Test {
 public:
  HeapprofdEndToEnd() {
    // This is not needed for correctness, but works around a init behavior that
    // makes this test take much longer. If persist.heapprofd.enable is set to 0
    // and then set to 1 again too quickly, init decides that the service is
    // "restarting" and waits before restarting it.
    usleep(50000);
    unset_property = StartSystemHeapprofdIfRequired();
  }

 protected:
  base::TestTaskRunner task_runner;

  void TraceAndValidate(const TraceConfig& trace_config,
                        pid_t pid,
                        uint64_t alloc_size) {
    auto helper = GetHelper(&task_runner);

    helper->StartTracing(trace_config);
    helper->WaitForTracingDisabled(20000);

    helper->ReadData();
    helper->WaitForReadData();

    const auto& packets = helper->trace();
    ASSERT_GT(packets.size(), 0u);
    size_t profile_packets = 0;
    size_t samples = 0;
    uint64_t last_allocated = 0;
    uint64_t last_freed = 0;
    for (const protos::TracePacket& packet : packets) {
      if (packet.has_profile_packet() &&
          packet.profile_packet().process_dumps().size() > 0) {
        const auto& dumps = packet.profile_packet().process_dumps();
        ASSERT_EQ(dumps.size(), 1);
        const protos::ProfilePacket_ProcessHeapSamples& dump = dumps.Get(0);
        EXPECT_EQ(dump.pid(), pid);
        for (const auto& sample : dump.samples()) {
          samples++;
          EXPECT_EQ(sample.self_allocated() % alloc_size, 0);
          EXPECT_EQ(sample.self_freed() % alloc_size, 0);
          last_allocated = sample.self_allocated();
          last_freed = sample.self_freed();
          EXPECT_THAT(sample.self_allocated() - sample.self_freed(),
                      AnyOf(Eq(0), Eq(alloc_size)));
        }
        profile_packets++;
      }
    }
    EXPECT_GT(profile_packets, 0);
    EXPECT_GT(samples, 0);
    EXPECT_GT(last_allocated, 0);
    EXPECT_GT(last_freed, 0);
  }

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<HeapprofdDelegate>(
      new HeapprofdDelegate(TEST_PRODUCER_SOCK_NAME)));
#else
  base::ScopedResource<std::string*, SetEnableProperty, nullptr> unset_property;
#endif

  void Smoke() {
    constexpr size_t kAllocSize = 1024;

    pid_t pid = ForkContinuousMalloc(kAllocSize);

    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(10 * 1024);
    trace_config.set_duration_ms(2000);
    trace_config.set_flush_timeout_ms(10000);

    auto* ds_config = trace_config.add_data_sources()->mutable_config();
    ds_config->set_name("android.heapprofd");
    ds_config->set_target_buffer(0);

    auto* heapprofd_config = ds_config->mutable_heapprofd_config();
    heapprofd_config->set_sampling_interval_bytes(1);
    *heapprofd_config->add_pid() = static_cast<uint64_t>(pid);
    heapprofd_config->set_all(false);
    heapprofd_config->mutable_continuous_dump_config()->set_dump_phase_ms(0);
    heapprofd_config->mutable_continuous_dump_config()->set_dump_interval_ms(
        100);

    TraceAndValidate(trace_config, pid, kAllocSize);

    PERFETTO_CHECK(kill(pid, SIGKILL) == 0);
    PERFETTO_CHECK(waitpid(pid, nullptr, 0) == pid);
  }

  void FinalFlush() {
    constexpr size_t kAllocSize = 1024;

    pid_t pid = ForkContinuousMalloc(kAllocSize);

    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(10 * 1024);
    trace_config.set_duration_ms(2000);
    trace_config.set_flush_timeout_ms(10000);

    auto* ds_config = trace_config.add_data_sources()->mutable_config();
    ds_config->set_name("android.heapprofd");
    ds_config->set_target_buffer(0);

    auto* heapprofd_config = ds_config->mutable_heapprofd_config();
    heapprofd_config->set_sampling_interval_bytes(1);
    *heapprofd_config->add_pid() = static_cast<uint64_t>(pid);
    heapprofd_config->set_all(false);

    TraceAndValidate(trace_config, pid, kAllocSize);

    PERFETTO_CHECK(kill(pid, SIGKILL) == 0);
    PERFETTO_CHECK(waitpid(pid, nullptr, 0) == pid);
  }

  void NativeStartup() {
    auto helper = GetHelper(&task_runner);

    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(10 * 1024);
    trace_config.set_duration_ms(5000);
    trace_config.set_flush_timeout_ms(10000);

    auto* ds_config = trace_config.add_data_sources()->mutable_config();
    ds_config->set_name("android.heapprofd");

    auto* heapprofd_config = ds_config->mutable_heapprofd_config();
    heapprofd_config->set_sampling_interval_bytes(1);
    *heapprofd_config->add_process_cmdline() = "heapprofd_continuous_malloc";
    heapprofd_config->set_all(false);

    helper->StartTracing(trace_config);

    // Wait to guarantee that the process forked below is hooked by the profiler
    // by virtue of the startup check, and not by virtue of being seen as a
    // running process. This sleep is here to prevent that, accidentally, the
    // test gets to the fork()+exec() too soon, before the heap profiling daemon
    // has received the trace config.
    sleep(1);

    // Make sure the forked process does not get reparented to init.
    setsid();
    pid_t pid = fork();
    switch (pid) {
      case -1:
        PERFETTO_FATAL("Failed to fork.");
      case 0: {
        const char* envp[] = {"HEAPPROFD_TESTING_RUN_MALLOC=1", nullptr};
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        PERFETTO_CHECK(execle("/proc/self/exe", "heapprofd_continuous_malloc",
                              nullptr, envp) == 0);
        break;
      }
      default:
        break;
    }

    helper->WaitForTracingDisabled(20000);

    helper->ReadData();
    helper->WaitForReadData();

    PERFETTO_CHECK(kill(pid, SIGKILL) == 0);
    PERFETTO_CHECK(waitpid(pid, nullptr, 0) == pid);

    const auto& packets = helper->trace();
    ASSERT_GT(packets.size(), 0u);
    size_t profile_packets = 0;
    size_t samples = 0;
    uint64_t total_allocated = 0;
    uint64_t total_freed = 0;
    for (const protos::TracePacket& packet : packets) {
      if (packet.has_profile_packet() &&
          packet.profile_packet().process_dumps().size() > 0) {
        const auto& dumps = packet.profile_packet().process_dumps();
        ASSERT_EQ(dumps.size(), 1);
        const protos::ProfilePacket_ProcessHeapSamples& dump = dumps.Get(0);
        EXPECT_EQ(dump.pid(), pid);
        profile_packets++;
        for (const auto& sample : dump.samples()) {
          samples++;
          total_allocated += sample.self_allocated();
          total_freed += sample.self_freed();
        }
      }
    }
    EXPECT_EQ(profile_packets, 1);
    EXPECT_GT(samples, 0);
    EXPECT_GT(total_allocated, 0);
    EXPECT_GT(total_freed, 0);
  }

  void ReInit() {
    constexpr uint64_t kFirstIterationBytes = 5;
    constexpr uint64_t kSecondIterationBytes = 7;

    base::Pipe signal_pipe = base::Pipe::Create(base::Pipe::kBothNonBlock);
    base::Pipe ack_pipe = base::Pipe::Create(base::Pipe::kBothBlock);

    pid_t pid = fork();
    switch (pid) {
      case -1:
        PERFETTO_FATAL("Failed to fork.");
      case 0: {
        uint64_t bytes = kFirstIterationBytes;
        signal_pipe.wr.reset();
        ack_pipe.rd.reset();
        for (;;) {
          AllocateAndFree(bytes);
          char buf[1];
          if (bool(signal_pipe.rd) &&
              read(*signal_pipe.rd, buf, sizeof(buf)) == 0) {
            // make sure the client has noticed that the session has stopped
            AllocateAndFree(bytes);

            bytes = kSecondIterationBytes;
            signal_pipe.rd.reset();
            ack_pipe.wr.reset();
          }
          usleep(10 * kMsToUs);
        }
        PERFETTO_FATAL("Should be unreachable");
      }
      default:
        break;
    }

    signal_pipe.rd.reset();
    ack_pipe.wr.reset();

    TraceConfig trace_config;
    trace_config.add_buffers()->set_size_kb(10 * 1024);
    trace_config.set_duration_ms(2000);
    trace_config.set_flush_timeout_ms(10000);

    auto* ds_config = trace_config.add_data_sources()->mutable_config();
    ds_config->set_name("android.heapprofd");
    ds_config->set_target_buffer(0);

    auto* heapprofd_config = ds_config->mutable_heapprofd_config();
    heapprofd_config->set_sampling_interval_bytes(1);
    *heapprofd_config->add_pid() = static_cast<uint64_t>(pid);
    heapprofd_config->set_all(false);

    TraceAndValidate(trace_config, pid, kFirstIterationBytes);

    signal_pipe.wr.reset();
    char buf[1];
    ASSERT_EQ(read(*ack_pipe.rd, buf, sizeof(buf)), 0);
    ack_pipe.rd.reset();

    // TODO(rsavitski): this sleep is to compensate for the heapprofd delaying
    // in closing the sockets (and therefore the client noticing that the
    // session is over). Clarify where the delays are coming from.
    usleep(100 * kMsToUs);

    PERFETTO_LOG("HeapprofdEndToEnd::Reinit: Starting second");
    TraceAndValidate(trace_config, pid, kSecondIterationBytes);

    PERFETTO_CHECK(kill(pid, SIGKILL) == 0);
    PERFETTO_CHECK(waitpid(pid, nullptr, 0) == pid);
  }
};

TEST_F(HeapprofdEndToEnd, Smoke_Central) {
  ASSERT_EQ(ReadProperty(kHeapprofdModeProperty, ""), "");
  Smoke();
}

TEST_F(HeapprofdEndToEnd, Smoke_Fork) {
  // RAII handle that resets to central mode when out of scope.
  auto prop = EnableFork();
  ASSERT_EQ(ReadProperty(kHeapprofdModeProperty, ""), "fork");
  Smoke();
}

TEST_F(HeapprofdEndToEnd, FinalFlush_Central) {
  ASSERT_EQ(ReadProperty(kHeapprofdModeProperty, ""), "");
  FinalFlush();
}

TEST_F(HeapprofdEndToEnd, FinalFlush_Fork) {
  // RAII handle that resets to central mode when out of scope.
  auto prop = EnableFork();
  ASSERT_EQ(ReadProperty(kHeapprofdModeProperty, ""), "fork");
  FinalFlush();
}

TEST_F(HeapprofdEndToEnd, NativeStartup_Central) {
  ASSERT_EQ(ReadProperty(kHeapprofdModeProperty, ""), "");
  NativeStartup();
}

TEST_F(HeapprofdEndToEnd, NativeStartup_Fork) {
  // RAII handle that resets to central mode when out of scope.
  auto prop = EnableFork();
  ASSERT_EQ(ReadProperty(kHeapprofdModeProperty, ""), "fork");
  NativeStartup();
}

TEST_F(HeapprofdEndToEnd, ReInit_Central) {
  ASSERT_EQ(ReadProperty(kHeapprofdModeProperty, ""), "");
  ReInit();
}

TEST_F(HeapprofdEndToEnd, ReInit_Fork) {
  // RAII handle that resets to central mode when out of scope.
  auto prop = EnableFork();
  ASSERT_EQ(ReadProperty(kHeapprofdModeProperty, ""), "fork");
  ReInit();
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
