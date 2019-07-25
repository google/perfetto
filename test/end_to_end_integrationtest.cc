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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <initializer_list>
#include <random>
#include <thread>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/config/power/android_power_config.pbzero.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/traced/traced.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/ipc/default_socket.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/tracing/core/test_config.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "test/task_runner_thread.h"
#include "test/task_runner_thread_delegates.h"
#include "test/test_helper.h"

namespace perfetto {

namespace {

using ::testing::ContainsRegex;
using ::testing::HasSubstr;

std::string RandomTraceFileName() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  constexpr char kSysTmpPath[] = "/data/misc/perfetto-traces";
#else
  constexpr char kSysTmpPath[] = "/tmp";
#endif
  static int suffix = 0;

  std::string path;
  path.assign(kSysTmpPath);
  path.append("/trace-");
  path.append(std::to_string(base::GetBootTimeNs().count()));
  path.append("-");
  path.append(std::to_string(suffix++));
  return path;
}

// This class is a reference to a child process that has in essence been execv
// to the requested binary. The process will start and then wait for Run()
// before proceeding. We use this to fork new processes before starting any
// additional threads in the parent proces (otherwise you would risk
// deadlocks), but pause the forked processes until remaining setup (including
// any necessary threads) in the parent process is complete.
class Exec {
 public:
  // Starts the forked process that was created. If not null then |stderr_out
  // will contain the std::cerr output of the process.
  int Run(std::string* stderr_out = nullptr) {
    // We can't be the child process.
    PERFETTO_CHECK(pid_ != 0);

    // Send some random bytes so the child process knows the service is up and
    // it can connect and execute.
    PERFETTO_CHECK(PERFETTO_EINTR(write(*start_pipe_.wr, "42", 2)) ==
                   static_cast<ssize_t>(2));
    start_pipe_.wr.reset();

    // Setup a large enough buffer and read all of stderr (until the process
    // closes the err_pipe on process exit).
    std::string stderr_str = std::string(1024 * 1024, '\0');
    ssize_t rsize = 0;
    size_t stderr_pos = 0;
    while (stderr_pos < stderr_str.size()) {
      rsize = PERFETTO_EINTR(read(*err_pipe_.rd, &stderr_str[stderr_pos],
                                  stderr_str.size() - stderr_pos - 1));
      if (rsize <= 0)
        break;
      stderr_pos += static_cast<size_t>(rsize);
    }
    stderr_str.resize(stderr_pos);

    // Either output the stderr_out to the provided variable or for the record
    // it into the info logs.
    if (stderr_out) {
      *stderr_out = stderr_str;
    } else {
      PERFETTO_LOG("Child proc %d exited with stderr: \"%s\"", pid_,
                   stderr_str.c_str());
    }

    int status = 1;
    PERFETTO_CHECK(PERFETTO_EINTR(waitpid(pid_, &status, 0)) == pid_);
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

 private:
  Exec(pid_t pid, base::Pipe err, base::Pipe start)
      : pid_(pid), err_pipe_(std::move(err)), start_pipe_(std::move(start)) {}

  static Exec Create(const std::string& argv0,
                     std::initializer_list<std::string> args,
                     std::string input = "") {
    if (argv0 != "perfetto" && argv0 != "trigger_perfetto") {
      PERFETTO_FATAL(
          "Received argv0: \"%s\" which isn't supported. Supported binaries "
          "are \"perfetto\" or \"trigger_perfetto\".",
          argv0.c_str());
    }

    // |in_pipe| == std::cin, |err_pipe| == std::cerr for the process we're
    // about to fork. |start_pipe| is used to block the process so we can hold
    // it until we're ready (the service has started up).
    base::Pipe in_pipe = base::Pipe::Create();
    base::Pipe err_pipe = base::Pipe::Create();
    base::Pipe start_pipe = base::Pipe::Create();

    pid_t pid = fork();
    PERFETTO_CHECK(pid >= 0);
    if (pid == 0) {
      // Child process, we need to block the child process until we've been
      // signaled on the |start_pipe|.
      std::string junk = std::string(4, '\0');
      start_pipe.wr.reset();
      ssize_t rsize = 0;
      rsize = PERFETTO_EINTR(read(*start_pipe.rd, &junk[0], junk.size() - 1));
      PERFETTO_CHECK(rsize >= 0);
      start_pipe.rd.reset();

      // We've been signalled to start so execute in a sub function.
      _exit(RunChild(argv0, std::move(args), std::move(in_pipe),
                     std::move(err_pipe)));
    } else {
      // Parent, we don't need to write to the childs std::cerr nor do we need
      // to read the start_pipe.
      err_pipe.wr.reset();
      start_pipe.rd.reset();

      // This is generally an unsafe pattern because the child process might
      // be blocked on stdout and stall the stdin reads. It's pragmatically
      // okay for our test cases because stdin is not expected to exceed the
      // pipe buffer.
      //
      // We need to write this now up front (rather than in Run(), because in
      // some tests we create multiple Exec classes, and if we don't close the
      // input pipe up front then future Exec's will have a reference and the
      // pipe won't close properly.
      PERFETTO_CHECK(input.size() <= base::kPageSize);
      PERFETTO_CHECK(
          PERFETTO_EINTR(write(*in_pipe.wr, input.data(), input.size())) ==
          static_cast<ssize_t>(input.size()));
      in_pipe.wr.reset();
      // Close the input pipe only after the write so we don't get an EPIPE
      // signal in the cases when the child process earlies out without
      // reading stdin.
      in_pipe.rd.reset();

      return Exec(pid, std::move(err_pipe), std::move(start_pipe));
    }
  }

  // Wrapper to contain all the work the child process needs to do.
  static int RunChild(const std::string& argv0,
                      std::initializer_list<std::string> args,
                      base::Pipe in_pipe,
                      base::Pipe err_pipe) {
    // This sets up the char** argv buffer we're going to provide to the main
    // function for |argv0| binary.
    std::vector<char> argv_buffer;
    std::vector<size_t> argv_offsets;
    std::vector<char*> argv;
    argv_offsets.push_back(0);

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

    // We aren't reading std::cerr nor writing to std::cin.
    err_pipe.rd.reset();
    in_pipe.wr.reset();

    // This makes it so the binaries below will correctly write their std::cin
    // and std::cerr to the right pipes.
    int devnull = open("/dev/null", O_RDWR);
    PERFETTO_CHECK(devnull >= 0);
    PERFETTO_CHECK(dup2(*in_pipe.rd, STDIN_FILENO) != -1);
    PERFETTO_CHECK(dup2(devnull, STDOUT_FILENO) != -1);
    PERFETTO_CHECK(dup2(*err_pipe.wr, STDERR_FILENO) != -1);
#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
    setenv("PERFETTO_CONSUMER_SOCK_NAME", TestHelper::GetConsumerSocketName(),
           1);
    setenv("PERFETTO_PRODUCER_SOCK_NAME", TestHelper::GetProducerSocketName(),
           1);
    if (argv0 == "perfetto") {
      return PerfettoCmdMain(static_cast<int>(argv.size() - 1), argv.data());
    } else if (argv0 == "trigger_perfetto") {
      return TriggerPerfettoMain(static_cast<int>(argv.size() - 1),
                                 argv.data());
    } else {
      PERFETTO_FATAL("Unknown binary: %s", argv0.c_str());
      return 4;
    }
#else
    execv((std::string("/system/bin/") + argv0).c_str(), &argv[0]);
    return 3;
#endif
  }

  friend class PerfettoCmdlineTest;

  pid_t pid_;
  base::Pipe err_pipe_;
  base::Pipe start_pipe_;
};

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
  void SetUp() override { exec_allowed_ = true; }

  void TearDown() override {}

  void StartServiceIfRequiredNoNewExecsAfterThis() {
    exec_allowed_ = false;
    test_helper_.StartServiceIfRequired();
  }

  FakeProducer* ConnectFakeProducer() {
    return test_helper_.ConnectFakeProducer();
  }

  std::function<void()> WrapTask(const std::function<void()>& function) {
    return test_helper_.WrapTask(function);
  }

  void WaitForProducerSetup() { test_helper_.WaitForProducerSetup(); }

  void WaitForProducerEnabled() { test_helper_.WaitForProducerEnabled(); }

  // Creates a process that represents the perfetto binary that will
  // start when Run() is called. |args| will be passed as part of
  // the command line and |std_in| will be piped into std::cin.
  Exec ExecPerfetto(std::initializer_list<std::string> args,
                    std::string std_in = "") {
    // You can not fork after you've started the service due to risk of
    // deadlocks.
    PERFETTO_CHECK(exec_allowed_);
    return Exec::Create("perfetto", std::move(args), std::move(std_in));
  }

  // Creates a process that represents the trigger_perfetto binary that will
  // start when Run() is called. |args| will be passed as part of
  // the command line and |std_in| will be piped into std::cin.
  Exec ExecTrigger(std::initializer_list<std::string> args,
                   std::string std_in = "") {
    // You can not fork after you've started the service due to risk of
    // deadlocks.
    PERFETTO_CHECK(exec_allowed_);
    return Exec::Create("trigger_perfetto", std::move(args), std::move(std_in));
  }

  // Tests are allowed to freely use these variables.
  std::string stderr_;
  base::TestTaskRunner task_runner_;

 private:
  bool exec_allowed_;
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

  protos::FtraceConfig ftrace_config;
  ftrace_config.add_ftrace_events("sched_switch");
  ftrace_config.add_ftrace_events("bar");
  ds_config->set_ftrace_config_raw(ftrace_config.SerializeAsString());

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

  protos::FtraceConfig ftrace_config;
  ftrace_config.add_ftrace_events("print");
  ds_config->set_ftrace_config_raw(ftrace_config.SerializeAsString());

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

  using protos::pbzero::AndroidPowerConfig;
  protozero::HeapBuffered<AndroidPowerConfig> power_config;
  power_config->set_battery_poll_ms(250);
  power_config->add_battery_counters(
      AndroidPowerConfig::BATTERY_COUNTER_CHARGE);
  power_config->add_battery_counters(
      AndroidPowerConfig::BATTERY_COUNTER_CAPACITY_PERCENT);
  ds_config->set_android_power_config_raw(power_config.SerializeAsString());

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

  auto invalid_arg = ExecPerfetto({"--invalid-arg"});
  auto empty_config = ExecPerfetto({"-c", "-", "-o", "-"}, "");

  // Cannot make assertions on --dropbox because on standalone builds it fails
  // prematurely due to lack of dropbox.
  auto missing_dropbox =
      ExecPerfetto({"-c", "-", "--txt", "-o", "-", "--dropbox=foo"}, cfg);
  auto either_out_or_dropbox = ExecPerfetto({"-c", "-", "--txt"}, cfg);

  // Disallow mixing simple and file config.
  auto simple_and_file_1 =
      ExecPerfetto({"-o", "-", "-c", "-", "-t", "2s"}, cfg);
  auto simple_and_file_2 =
      ExecPerfetto({"-o", "-", "-c", "-", "-b", "2m"}, cfg);
  auto simple_and_file_3 =
      ExecPerfetto({"-o", "-", "-c", "-", "-s", "2m"}, cfg);

  // Invalid --attach / --detach cases.
  auto invalid_stop =
      ExecPerfetto({"-c", "-", "--txt", "-o", "-", "--stop"}, cfg);
  auto attach_and_config_1 =
      ExecPerfetto({"-c", "-", "--txt", "-o", "-", "--attach=foo"}, cfg);
  auto attach_and_config_2 =
      ExecPerfetto({"-t", "2s", "-o", "-", "--attach=foo"}, cfg);
  auto attach_needs_argument = ExecPerfetto({"--attach"}, cfg);
  auto detach_needs_argument =
      ExecPerfetto({"-t", "2s", "-o", "-", "--detach"}, cfg);
  auto detach_without_out_or_dropbox =
      ExecPerfetto({"-t", "2s", "--detach=foo"}, cfg);

  // Cannot trace and use --query.
  auto trace_and_query_1 = ExecPerfetto({"-t", "2s", "--query"}, cfg);
  auto trace_and_query_2 = ExecPerfetto({"-c", "-", "--query"}, cfg);

  // Ensure all Exec:: calls have been saved to prevent deadlocks.
  StartServiceIfRequiredNoNewExecsAfterThis();

  EXPECT_EQ(1, invalid_arg.Run(&stderr_));

  EXPECT_EQ(1, empty_config.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("TraceConfig is empty"));

  // Cannot make assertions on --dropbox because on standalone builds it fails
  // prematurely due to lack of dropbox.
  EXPECT_EQ(1, missing_dropbox.Run(&stderr_));

  EXPECT_EQ(1, either_out_or_dropbox.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Either --out or --dropbox"));

  // Disallow mixing simple and file config.
  EXPECT_EQ(1, simple_and_file_1.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  EXPECT_EQ(1, simple_and_file_2.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  EXPECT_EQ(1, simple_and_file_3.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify both -c"));

  // Invalid --attach / --detach cases.
  EXPECT_EQ(1, invalid_stop.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("--stop is supported only in combination"));

  EXPECT_EQ(1, attach_and_config_1.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify a trace config"));

  EXPECT_EQ(1, attach_and_config_2.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify a trace config"));

  EXPECT_EQ(1, attach_needs_argument.Run(&stderr_));
  EXPECT_THAT(stderr_, ContainsRegex("option.*--attach.*requires an argument"));

  EXPECT_EQ(1, detach_needs_argument.Run(&stderr_));
  EXPECT_THAT(stderr_, ContainsRegex("option.*--detach.*requires an argument"));

  EXPECT_EQ(1, detach_without_out_or_dropbox.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("--out or --dropbox is required"));

  // Cannot trace and use --query.
  EXPECT_EQ(1, trace_and_query_1.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify a trace config"));

  EXPECT_EQ(1, trace_and_query_2.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Cannot specify a trace config"));
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(TxtConfig)) {
  std::string cfg("duration_ms: 100");
  auto perfetto = ExecPerfetto({"-c", "-", "--txt", "-o", "-"}, cfg);
  StartServiceIfRequiredNoNewExecsAfterThis();
  EXPECT_EQ(0, perfetto.Run(&stderr_)) << stderr_;
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(SimpleConfig)) {
  auto perfetto = ExecPerfetto({"-o", "-", "-c", "-", "-t", "100ms"});
  StartServiceIfRequiredNoNewExecsAfterThis();
  EXPECT_EQ(0, perfetto.Run(&stderr_)) << stderr_;
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(DetachAndAttach)) {
  auto attach_to_not_existing = ExecPerfetto({"--attach=not_existent"});

  std::string cfg("duration_ms: 10000; write_into_file: true");
  auto detach_valid_stop =
      ExecPerfetto({"-o", "-", "-c", "-", "--txt", "--detach=valid_stop"}, cfg);
  auto stop_valid_stop = ExecPerfetto({"--attach=valid_stop", "--stop"});

  StartServiceIfRequiredNoNewExecsAfterThis();

  EXPECT_NE(0, attach_to_not_existing.Run(&stderr_));
  EXPECT_THAT(stderr_, HasSubstr("Session re-attach failed"));

  EXPECT_EQ(0, detach_valid_stop.Run(&stderr_)) << stderr_;
  EXPECT_EQ(0, stop_valid_stop.Run(&stderr_));
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(StartTracingTrigger)) {
  // See |message_count| and |message_size| in the TraceConfig above.
  constexpr size_t kMessageCount = 11;
  constexpr size_t kMessageSize = 32;
  protos::TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(kMessageCount);
  ds_config->mutable_for_testing()->set_message_size(kMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::TraceConfig::TriggerConfig::START_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);

  // We have 6 normal preamble packets (start clock, trace config, clock,
  // system info, sync marker, stats) and then since this is a trace with a
  // trigger config we have an additional ReceivedTriggers packet.
  constexpr size_t kPreamblePackets = 7;

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  auto trigger_proc = ExecTrigger({"trigger_name"});

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();

  auto* fake_producer = ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  // Start a background thread that will deliver the config now that we've
  // started the service. See |perfetto_proc| above for the args passed.
  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  WaitForProducerSetup();
  EXPECT_EQ(0, trigger_proc.Run(&stderr_));

  // Wait for the producer to start, and then write out 11 packets.
  WaitForProducerEnabled();
  auto on_data_written = task_runner_.CreateCheckpoint("data_written");
  fake_producer->ProduceEventBatch(WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written");
  background_trace.join();

  std::string trace_str;
  base::ReadFile(path, &trace_str);
  protos::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_str));
  EXPECT_EQ(kPreamblePackets + kMessageCount, trace.packet_size());
  for (const auto& packet : trace.packet()) {
    if (packet.data_case() == protos::TracePacket::kTraceConfig) {
      // Ensure the trace config properly includes the trigger mode we set.
      EXPECT_EQ(protos::TraceConfig::TriggerConfig::START_TRACING,
                packet.trace_config().trigger_config().trigger_mode());
    } else if (packet.data_case() == protos::TracePacket::kTrigger) {
      // validate that the triggers are properly added to the trace.
      EXPECT_EQ("trigger_name", packet.trigger().trigger_name());
    } else if (packet.data_case() == protos::TracePacket::kForTesting) {
      // Make sure that the data size is correctly set based on what we
      // requested.
      EXPECT_EQ(kMessageSize, packet.for_testing().str().size());
    }
  }
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(StopTracingTrigger)) {
  // See |message_count| and |message_size| in the TraceConfig above.
  constexpr size_t kMessageCount = 11;
  constexpr size_t kMessageSize = 32;
  protos::TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(kMessageCount);
  ds_config->mutable_for_testing()->set_message_size(kMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);
  trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name_3");
  trigger->set_stop_delay_ms(60000);

  // We have 6 normal preamble packets (start clock, trace config, clock,
  // system info, sync marker, stats) and then since this is a trace with a
  // trigger config we have an additional ReceivedTriggers packet.
  constexpr size_t kPreamblePackets = 8;

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  auto trigger_proc =
      ExecTrigger({"trigger_name_2", "trigger_name", "trigger_name_3"});

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  // Start a background thread that will deliver the config now that we've
  // started the service. See |perfetto_proc| above for the args passed.
  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  WaitForProducerEnabled();
  // Wait for the producer to start, and then write out 11 packets, before the
  // trace actually starts (the trigger is seen).
  auto on_data_written = task_runner_.CreateCheckpoint("data_written_1");
  fake_producer->ProduceEventBatch(WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written_1");

  EXPECT_EQ(0, trigger_proc.Run(&stderr_)) << "stderr: " << stderr_;

  background_trace.join();

  std::string trace_str;
  base::ReadFile(path, &trace_str);
  protos::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_str));
  EXPECT_EQ(kPreamblePackets + kMessageCount, trace.packet_size());
  bool seen_first_trigger = false;
  for (const auto& packet : trace.packet()) {
    if (packet.data_case() == protos::TracePacket::kTraceConfig) {
      // Ensure the trace config properly includes the trigger mode we set.
      EXPECT_EQ(protos::TraceConfig::TriggerConfig::STOP_TRACING,
                packet.trace_config().trigger_config().trigger_mode());
    } else if (packet.data_case() == protos::TracePacket::kTrigger) {
      // validate that the triggers are properly added to the trace.
      if (!seen_first_trigger) {
        EXPECT_EQ("trigger_name", packet.trigger().trigger_name());
        seen_first_trigger = true;
      } else {
        EXPECT_EQ("trigger_name_3", packet.trigger().trigger_name());
      }
    } else if (packet.data_case() == protos::TracePacket::kForTesting) {
      // Make sure that the data size is correctly set based on what we
      // requested.
      EXPECT_EQ(kMessageSize, packet.for_testing().str().size());
    }
  }
}

// Dropbox on the commandline client only works on android builds. So disable
// this test on all other builds.
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
TEST_F(PerfettoCmdlineTest, NoSanitizers(NoDataNoFileWithoutTrigger)) {
#else
TEST_F(PerfettoCmdlineTest, DISABLED_NoDataNoFileWithoutTrigger) {
#endif
  // See |message_count| and |message_size| in the TraceConfig above.
  constexpr size_t kMessageCount = 11;
  constexpr size_t kMessageSize = 32;
  protos::TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_allow_user_build_tracing(true);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(kMessageCount);
  ds_config->mutable_for_testing()->set_message_size(kMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(1000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);
  trigger = trigger_cfg->add_triggers();

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  auto perfetto_proc = ExecPerfetto(
      {
          "--dropbox",
          "TAG",
          "--no-guardrails",
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::string stderr_str;
  std::thread background_trace([&perfetto_proc, &stderr_str]() {
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str));
  });
  background_trace.join();

  EXPECT_THAT(stderr_str,
              ::testing::HasSubstr("Skipping write to dropbox. Empty trace."));
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(StopTracingTriggerFromConfig)) {
  // See |message_count| and |message_size| in the TraceConfig above.
  constexpr size_t kMessageCount = 11;
  constexpr size_t kMessageSize = 32;
  protos::TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(kMessageCount);
  ds_config->mutable_for_testing()->set_message_size(kMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);
  trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name_3");
  trigger->set_stop_delay_ms(60000);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
      },
      trace_config.SerializeAsString());

  std::string triggers = R"(
    activate_triggers: "trigger_name_2"
    activate_triggers: "trigger_name"
    activate_triggers: "trigger_name_3"
  )";
  auto perfetto_proc_2 = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
          "--txt",
      },
      triggers);

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::thread background_trace([&perfetto_proc]() {
    std::string stderr_str;
    EXPECT_EQ(0, perfetto_proc.Run(&stderr_str)) << stderr_str;
  });

  WaitForProducerEnabled();
  // Wait for the producer to start, and then write out 11 packets, before the
  // trace actually starts (the trigger is seen).
  auto on_data_written = task_runner_.CreateCheckpoint("data_written_1");
  fake_producer->ProduceEventBatch(WrapTask(on_data_written));
  task_runner_.RunUntilCheckpoint("data_written_1");

  EXPECT_EQ(0, perfetto_proc_2.Run(&stderr_)) << "stderr: " << stderr_;

  background_trace.join();

  std::string trace_str;
  base::ReadFile(path, &trace_str);
  protos::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(trace_str));
  EXPECT_LT(kMessageCount, trace.packet_size());
  bool seen_first_trigger = false;
  for (const auto& packet : trace.packet()) {
    if (packet.data_case() == protos::TracePacket::kTraceConfig) {
      // Ensure the trace config properly includes the trigger mode we set.
      EXPECT_EQ(protos::TraceConfig::TriggerConfig::STOP_TRACING,
                packet.trace_config().trigger_config().trigger_mode());
    } else if (packet.data_case() == protos::TracePacket::kTrigger) {
      // validate that the triggers are properly added to the trace.
      if (!seen_first_trigger) {
        EXPECT_EQ("trigger_name", packet.trigger().trigger_name());
        seen_first_trigger = true;
      } else {
        EXPECT_EQ("trigger_name_3", packet.trigger().trigger_name());
      }
    } else if (packet.data_case() == protos::TracePacket::kForTesting) {
      // Make sure that the data size is correctly set based on what we
      // requested.
      EXPECT_EQ(kMessageSize, packet.for_testing().str().size());
    }
  }
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(TriggerFromConfigStopsFileOpening)) {
  // See |message_count| and |message_size| in the TraceConfig above.
  constexpr size_t kMessageCount = 11;
  constexpr size_t kMessageSize = 32;
  protos::TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.perfetto.FakeProducer");
  ds_config->mutable_for_testing()->set_message_count(kMessageCount);
  ds_config->mutable_for_testing()->set_message_size(kMessageSize);
  auto* trigger_cfg = trace_config.mutable_trigger_config();
  trigger_cfg->set_trigger_mode(
      protos::TraceConfig::TriggerConfig::STOP_TRACING);
  trigger_cfg->set_trigger_timeout_ms(15000);
  auto* trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name");
  // |stop_delay_ms| must be long enough that we can write the packets in
  // before the trace finishes. This has to be long enough for the slowest
  // emulator. But as short as possible to prevent the test running a long
  // time.
  trigger->set_stop_delay_ms(500);
  trigger = trigger_cfg->add_triggers();
  trigger->set_name("trigger_name_3");
  trigger->set_stop_delay_ms(60000);

  // We have to construct all the processes we want to fork before we start the
  // service with |StartServiceIfRequired()|. this is because it is unsafe
  // (could deadlock) to fork after we've spawned some threads which might
  // printf (and thus hold locks).
  const std::string path = RandomTraceFileName();
  std::string triggers = R"(
    activate_triggers: "trigger_name_2"
    activate_triggers: "trigger_name"
    activate_triggers: "trigger_name_3"
  )";
  auto perfetto_proc = ExecPerfetto(
      {
          "-o",
          path,
          "-c",
          "-",
          "--txt",
      },
      triggers);

  // Start the service and connect a simple fake producer.
  StartServiceIfRequiredNoNewExecsAfterThis();
  auto* fake_producer = ConnectFakeProducer();
  EXPECT_TRUE(fake_producer);

  std::string trace_str;
  EXPECT_FALSE(base::ReadFile(path, &trace_str));

  EXPECT_EQ(0, perfetto_proc.Run(&stderr_)) << "stderr: " << stderr_;

  EXPECT_FALSE(base::ReadFile(path, &trace_str));
}

TEST_F(PerfettoCmdlineTest, NoSanitizers(Query)) {
  auto query = ExecPerfetto({"--query"});
  auto query_raw = ExecPerfetto({"--query-raw"});
  StartServiceIfRequiredNoNewExecsAfterThis();
  EXPECT_EQ(0, query.Run(&stderr_)) << stderr_;
  EXPECT_EQ(0, query_raw.Run(&stderr_)) << stderr_;
}

}  // namespace perfetto
