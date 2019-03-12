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

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <initializer_list>
#include <random>
#include <thread>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/pipe.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/base/test/test_task_runner.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/tracing/ipc/default_socket.h"
#include "test/task_runner_thread.h"
#include "test/task_runner_thread_delegates.h"
#include "test/test_helper.h"

#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {

using ::testing::ContainsRegex;
using ::testing::HasSubstr;

class PerfettoTest : public ::testing::Test {
 public:
  void SetUp() override {
    // TODO(primiano): refactor this, it's copy/pasted in three places now.
    size_t index = 0;
    constexpr auto kTracingPaths = FtraceController::kTracingPaths;
    while (!ftrace_procfs_ && kTracingPaths[index]) {
      ftrace_procfs_ = FtraceProcfs::Create(kTracingPaths[index++]);
    }
    if (!ftrace_procfs_)
      return;
    ftrace_procfs_->SetTracingOn(false);
  }

  void TearDown() override {
    if (ftrace_procfs_)
      ftrace_procfs_->SetTracingOn(false);
  }

  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
};

class PerfettoCmdlineTest : public ::testing::Test {
 public:
  void SetUp() override { test_helper_.StartServiceIfRequired(); }

  void TearDown() override {}

  // Fork() + executes the perfetto cmdline client with the given args and
  // returns the exit code.
  int Exec(std::initializer_list<std::string> args, std::string input = "") {
    std::vector<char> argv_buffer;
    std::vector<size_t> argv_offsets;
    std::vector<char*> argv;
    argv_offsets.push_back(0);

    std::string argv0("perfetto");
    argv_buffer.insert(argv_buffer.end(), argv0.begin(), argv0.end());
    argv_buffer.push_back('\0');

    for (const std::string& arg : args) {
      argv_offsets.push_back(argv_buffer.size());
      argv_buffer.insert(argv_buffer.end(), arg.begin(), arg.end());
      argv_buffer.push_back('\0');
    }

    for (size_t off : argv_offsets)
      argv.push_back(&argv_buffer[off]);
    argv.push_back(nullptr);

    // Create the pipe for the child process to return stderr.
    base::Pipe err_pipe = base::Pipe::Create();
    base::Pipe in_pipe = base::Pipe::Create();

    pid_t pid = fork();
    PERFETTO_CHECK(pid >= 0);
    if (pid == 0) {
      // Child process.
      err_pipe.rd.reset();
      in_pipe.wr.reset();

      int devnull = open("/dev/null", O_RDWR);
      PERFETTO_CHECK(devnull >= 0);
      PERFETTO_CHECK(dup2(*in_pipe.rd, STDIN_FILENO) != -1);
      PERFETTO_CHECK(dup2(devnull, STDOUT_FILENO) != -1);
      PERFETTO_CHECK(dup2(*err_pipe.wr, STDERR_FILENO) != -1);
#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
      setenv("PERFETTO_CONSUMER_SOCK_NAME", TestHelper::GetConsumerSocketName(),
             1);
      _exit(PerfettoCmdMain(static_cast<int>(argv.size() - 1), argv.data()));
#else
      execv("/system/bin/perfetto", &argv[0]);
      _exit(3);
#endif
    }

    // Parent.
    err_pipe.wr.reset();
    stderr_ = std::string(1024 * 1024, '\0');

    // This is generally an unsafe pattern because the child process might be
    // blocked on stdout and stall the stdin reads. It's pragmatically okay for
    // our test cases because stdin is not expected to exceed the pipe buffer.
    PERFETTO_CHECK(input.size() <= base::kPageSize);
    PERFETTO_CHECK(
        PERFETTO_EINTR(write(*in_pipe.wr, input.data(), input.size())) ==
        static_cast<ssize_t>(input.size()));
    in_pipe.wr.reset();

    // Close the input pipe only after the write so we don't get an EPIPE signal
    // in the cases when the child process earlies out without reading stdin.
    in_pipe.rd.reset();

    ssize_t rsize = 0;
    size_t stderr_pos = 0;
    while (stderr_pos < stderr_.size()) {
      rsize = PERFETTO_EINTR(read(*err_pipe.rd, &stderr_[stderr_pos],
                                  stderr_.size() - stderr_pos - 1));
      if (rsize <= 0)
        break;
      stderr_pos += static_cast<size_t>(rsize);
    }
    stderr_.resize(stderr_pos);
    int status = 1;
    PERFETTO_CHECK(PERFETTO_EINTR(waitpid(pid, &status, 0)) == pid);
    int exit_code;
    if (WIFEXITED(status)) {
      exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      exit_code = -(WTERMSIG(status));
      PERFETTO_CHECK(exit_code < 0);
    } else {
      PERFETTO_FATAL("Unexpected exit status: %d", status);
    }
    return exit_code;
  }

  std::string stderr_;
  base::TestTaskRunner task_runner_;
  TestHelper test_helper_{&task_runner_};
};

}  // namespace

// If we're building on Android and starting the daemons ourselves,
// create the sockets in a world-writable location.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#else
#define TEST_PRODUCER_SOCK_NAME ::perfetto::GetProducerSocket()
#endif

// TODO(b/73453011): reenable on more platforms (including standalone Android).
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#define TreeHuggerOnly(x) x
#else
#define TreeHuggerOnly(x) DISABLED_##x
#endif

TEST_F(PerfettoTest, TreeHuggerOnly(TestFtraceProducer)) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<ProbesProducerDelegate>(
      new ProbesProducerDelegate(TEST_PRODUCER_SOCK_NAME)));
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(3000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");
  ds_config->set_target_buffer(0);

  auto* ftrace_config = ds_config->mutable_ftrace_config();
  *ftrace_config->add_ftrace_events() = "sched_switch";
  *ftrace_config->add_ftrace_events() = "bar";

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);

  for (const auto& packet : packets) {
    for (int ev = 0; ev < packet.ftrace_events().event_size(); ev++) {
      ASSERT_TRUE(packet.ftrace_events().event(ev).has_sched_switch());
    }
  }
}

TEST_F(PerfettoTest, TreeHuggerOnly(TestFtraceFlush)) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<ProbesProducerDelegate>(
      new ProbesProducerDelegate(TEST_PRODUCER_SOCK_NAME)));
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  const uint32_t kTestTimeoutMs = 30000;
  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(16);
  trace_config.set_duration_ms(kTestTimeoutMs);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");

  auto* ftrace_config = ds_config->mutable_ftrace_config();
  *ftrace_config->add_ftrace_events() = "print";

  helper.StartTracing(trace_config);

  // Do a first flush just to synchronize with the producer. The problem here
  // is that, on a Linux workstation, the producer can take several seconds just
  // to get to the point where ftrace is ready. We use the flush ack as a
  // synchronization point.
  helper.FlushAndWait(kTestTimeoutMs);

  EXPECT_TRUE(ftrace_procfs_->IsTracingEnabled());
  const char kMarker[] = "just_one_event";
  EXPECT_TRUE(ftrace_procfs_->WriteTraceMarker(kMarker));

  // This is the real flush we are testing.
  helper.FlushAndWait(kTestTimeoutMs);

  helper.DisableTracing();
  helper.WaitForTracingDisabled(kTestTimeoutMs);

  helper.ReadData();
  helper.WaitForReadData();

  int marker_found = 0;
  for (const auto& packet : helper.trace()) {
    for (int i = 0; i < packet.ftrace_events().event_size(); i++) {
      const auto& ev = packet.ftrace_events().event(i);
      if (ev.has_print() && ev.print().buf().find(kMarker) != std::string::npos)
        marker_found++;
    }
  }
  ASSERT_EQ(marker_found, 1);
}

TEST_F(PerfettoTest, TreeHuggerOnly(TestBatteryTracing)) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<ProbesProducerDelegate>(
      new ProbesProducerDelegate(TEST_PRODUCER_SOCK_NAME)));
#else
  base::ignore_result(TEST_PRODUCER_SOCK_NAME);
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(128);
  trace_config.set_duration_ms(3000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.power");
  ds_config->set_target_buffer(0);
  auto* power_config = ds_config->mutable_android_power_config();
  power_config->set_battery_poll_ms(250);
  *power_config->add_battery_counters() =
      AndroidPowerConfig::BATTERY_COUNTER_CHARGE;
  *power_config->add_battery_counters() =
      AndroidPowerConfig::BATTERY_COUNTER_CAPACITY_PERCENT;

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);

  bool has_battery_packet = false;
  for (const auto& packet : packets) {
    if (!packet.has_battery())
      continue;
    has_battery_packet = true;
    // Unfortunately we cannot make any assertions on the charge counter.
    // On some devices it can reach negative values (b/64685329).
    EXPECT_GE(packet.battery().capacity_percent(), 0);
    EXPECT_LE(packet.battery().capacity_percent(), 100);
  }

  ASSERT_TRUE(has_battery_packet);
}

TEST_F(PerfettoTest, TestFakeProducer) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(200);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  static constexpr size_t kNumPackets = 11;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 1024;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), kNumPackets);

  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
  }
}

TEST_F(PerfettoTest, VeryLargePackets) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(4096 * 10);
  trace_config.set_duration_ms(500);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->set_target_buffer(0);

  static constexpr size_t kNumPackets = 7;
  static constexpr uint32_t kRandomSeed = 42;
  static constexpr uint32_t kMsgSize = 1024 * 1024 - 42;
  ds_config->mutable_for_testing()->set_seed(kRandomSeed);
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(kMsgSize);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), kNumPackets);

  std::minstd_rand0 rnd_engine(kRandomSeed);
  for (const auto& packet : packets) {
    ASSERT_TRUE(packet.has_for_testing());
    ASSERT_EQ(packet.for_testing().seq_value(), rnd_engine());
    size_t msg_size = packet.for_testing().str().size();
    ASSERT_EQ(kMsgSize, msg_size);
    for (size_t i = 0; i < msg_size; i++)
      ASSERT_EQ(i < msg_size - 1 ? '.' : 0, packet.for_testing().str()[i]);
  }
}

TEST_F(PerfettoTest, DetachAndReattach) {
  base::TestTaskRunner task_runner;

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(10000);  // Max timeout, session is ended before.
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  static constexpr size_t kNumPackets = 11;
  ds_config->mutable_for_testing()->set_message_count(kNumPackets);
  ds_config->mutable_for_testing()->set_message_size(32);

  // Enable tracing and detach as soon as it gets started.
  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  auto* fake_producer = helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();
  helper.StartTracing(trace_config);

  // Detach.
  helper.DetachConsumer("key");

  // Write data while detached.
  helper.WaitForProducerEnabled();
  auto on_data_written = task_runner.CreateCheckpoint("data_written");
  fake_producer->ProduceEventBatch(helper.WrapTask(on_data_written));
  task_runner.RunUntilCheckpoint("data_written");

  // Then reattach the consumer.
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();
  helper.AttachConsumer("key");

  helper.DisableTracing();
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();
  const auto& packets = helper.trace();
  ASSERT_EQ(packets.size(), kNumPackets);
}

// Tests that a detached trace session is automatically cleaned up if the
// consumer doesn't re-attach before its expiration time.
TEST_F(PerfettoTest, ReattachFailsAfterTimeout) {
  base::TestTaskRunner task_runner;

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(250);
  trace_config.set_write_into_file(true);
  trace_config.set_file_write_period_ms(100000);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(1);
  ds_config->mutable_for_testing()->set_message_size(32);
  ds_config->mutable_for_testing()->set_send_batch_on_register(true);

  // Enable tracing and detach as soon as it gets started.
  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();
  helper.ConnectFakeProducer();
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  auto pipe_pair = base::Pipe::Create();
  helper.StartTracing(trace_config, std::move(pipe_pair.wr));

  // Detach.
  helper.DetachConsumer("key");

  // Use the file EOF (write end closed) as a way to detect when the trace
  // session is ended.
  char buf[1024];
  while (PERFETTO_EINTR(read(*pipe_pair.rd, buf, sizeof(buf))) > 0) {
  }

  // Give some margin for the tracing service to destroy the session.
  usleep(250000);

  // Reconnect and find out that it's too late and the session is gone.
  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();
  EXPECT_FALSE(helper.AttachConsumer("key"));
}

// Disable cmdline tests on sanitizets because they use fork() and that messes
// up leak / races detections, which has been fixed only recently (see
// https://github.com/google/sanitizers/issues/836 ).
#if defined(ADDRESS_SANITIZER) || defined(THREAD_SANITIZER) || \
    defined(MEMORY_SANITIZER) || defined(LEAK_SANITIZER)
#define NoSanitizers(X) DISABLED_##X
#else
#define NoSanitizers(X) X
#endif

TEST_F(PerfettoCmdlineTest, NoSanitizers(InvalidCases)) {
  std::string cfg("duration_ms: 100");

  EXPECT_EQ(1, Exec({"--invalid-arg"}));

  EXPECT_EQ(1, Exec({"-c", "-", "-o", "-"}, ""));
  EXPECT_THAT(stderr_, HasSubstr("TraceConfig is empty"));

  // Cannot make assertions on --dropbox because on standalone builds it fails
  // prematurely due to lack of dropbox.
  EXPECT_EQ(1, Exec({"-c", "-", "--txt", "-o", "-", "--dropbox=foo"}, cfg));

  EXPECT_EQ(1, Exec({"-c", "-", "--txt"}, cfg));
  EXPECT_THAT(stderr_, HasSubstr("Either --out or --dropbox"));

  // Disallow mixing simple and file config.
  EXPECT_EQ(1, Exec({"-o", "-", "-c", "-", "-t", "2s"}, cfg));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  EXPECT_EQ(1, Exec({"-o", "-", "-c", "-", "-b", "2m"}, cfg));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  EXPECT_EQ(1, Exec({"-o", "-", "-c", "-", "-s", "2m"}, cfg));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  // Invalid --attach / --detach cases.
  EXPECT_EQ(1, Exec({"-c", "-", "--txt", "-o", "-", "--stop"}, cfg));
  EXPECT_THAT(stderr_, HasSubstr("--stop is supported only in combination"));

  EXPECT_EQ(1, Exec({"-c", "-", "--txt", "-o", "-", "--attach=foo"}, cfg));
  EXPECT_THAT(stderr_, HasSubstr("trace config with --attach"));

  EXPECT_EQ(1, Exec({"-t", "2s", "-o", "-", "--attach=foo"}, cfg));
  EXPECT_THAT(stderr_, HasSubstr("trace config with --attach"));

  EXPECT_EQ(1, Exec({"--attach"}, cfg));
  EXPECT_THAT(stderr_, ContainsRegex("option.*--attach.*requires an argument"));

  EXPECT_EQ(1, Exec({"-t", "2s", "-o", "-", "--detach"}, cfg));
  EXPECT_THAT(stderr_, ContainsRegex("option.*--detach.*requires an argument"));

  EXPECT_EQ(1, Exec({"-t", "2s", "--detach=foo"}, cfg));
  EXPECT_THAT(stderr_, HasSubstr("--out or --dropbox is required"));
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(TxtConfig)) {
  std::string cfg("duration_ms: 100");
  EXPECT_EQ(0, Exec({"-c", "-", "--txt", "-o", "-"}, cfg)) << stderr_;
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(SimpleConfig)) {
  EXPECT_EQ(0, Exec({"-o", "-", "-c", "-", "-t", "100ms"}));
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(DetachAndAttach)) {
  EXPECT_NE(0, Exec({"--attach=not_existent"}));
  EXPECT_THAT(stderr_, HasSubstr("Session re-attach failed"));

  std::string cfg("duration_ms: 10000; write_into_file: true");
  EXPECT_EQ(0,
            Exec({"-o", "-", "-c", "-", "--txt", "--detach=valid_stop"}, cfg))
      << stderr_;
  EXPECT_EQ(0, Exec({"--attach=valid_stop", "--stop"}));
}

}  // namespace perfetto
