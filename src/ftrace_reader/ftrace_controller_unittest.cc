/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/ftrace_reader/ftrace_controller.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cpu_reader.h"
#include "ftrace_procfs.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/ftrace_reader/ftrace_config.h"
#include "proto_translation_table.h"

#include "src/base/test/test_task_runner.h"

#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

using testing::_;
using testing::AnyNumber;
using testing::ByMove;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;

using Table = perfetto::ProtoTranslationTable;
using FtraceEventBundle = perfetto::protos::pbzero::FtraceEventBundle;

namespace perfetto {

namespace {

const char kFooEnablePath[] = "/root/events/group/foo/enable";
const char kBarEnablePath[] = "/root/events/group/bar/enable";

class MockTaskRunner : public base::TaskRunner {
 public:
  MockTaskRunner() {
    ON_CALL(*this, PostTask(_))
        .WillByDefault(Invoke(this, &MockTaskRunner::OnPostTask));
    ON_CALL(*this, PostDelayedTask(_, _))
        .WillByDefault(Invoke(this, &MockTaskRunner::OnPostDelayedTask));
  }

  void OnPostTask(std::function<void()> task) {
    std::unique_lock<std::mutex> lock(lock_);
    EXPECT_FALSE(task_);
    task_ = std::move(task);
  }

  void OnPostDelayedTask(std::function<void()> task, int _delay) {
    std::unique_lock<std::mutex> lock(lock_);
    EXPECT_FALSE(task_);
    task_ = std::move(task);
  }

  void RunLastTask() { TakeTask()(); }

  std::function<void()> TakeTask() {
    std::unique_lock<std::mutex> lock(lock_);
    return std::move(task_);
  }

  MOCK_METHOD1(PostTask, void(std::function<void()>));
  MOCK_METHOD2(PostDelayedTask, void(std::function<void()>, int delay_ms));
  MOCK_METHOD2(AddFileDescriptorWatch, void(int fd, std::function<void()>));
  MOCK_METHOD1(RemoveFileDescriptorWatch, void(int fd));

 private:
  std::mutex lock_;
  std::function<void()> task_;
};

class MockDelegate : public perfetto::FtraceSink::Delegate {
 public:
  MOCK_METHOD1(GetBundleForCpu,
               protozero::ProtoZeroMessageHandle<FtraceEventBundle>(size_t));
  MOCK_METHOD2(OnBundleComplete_,
               void(size_t,
                    protozero::ProtoZeroMessageHandle<FtraceEventBundle>&));

  void OnBundleComplete(
      size_t cpu,
      protozero::ProtoZeroMessageHandle<FtraceEventBundle> bundle) {
    OnBundleComplete_(cpu, bundle);
  }
};

std::unique_ptr<Table> FakeTable() {
  std::vector<Field> common_fields;
  std::vector<Event> events;

  {
    Event event;
    event.name = "foo";
    event.group = "group";
    event.ftrace_event_id = 1;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "bar";
    event.group = "group";
    event.ftrace_event_id = 10;
    events.push_back(event);
  }

  return std::unique_ptr<Table>(new Table(events, std::move(common_fields)));
}

class MockFtraceProcfs : public FtraceProcfs {
 public:
  MockFtraceProcfs(size_t cpu_count = 1) : FtraceProcfs("/root/") {
    ON_CALL(*this, NumberOfCpus()).WillByDefault(Return(cpu_count));
    EXPECT_CALL(*this, NumberOfCpus()).Times(AnyNumber());
  }

  base::ScopedFile OpenPipeForCpu(size_t cpu) override {
    return base::ScopedFile(open("/dev/null", O_RDONLY));
  }

  MOCK_METHOD2(WriteToFile,
               bool(const std::string& path, const std::string& str));
  MOCK_CONST_METHOD0(NumberOfCpus, size_t());
};

}  // namespace

class TestFtraceController : public FtraceController {
 public:
  TestFtraceController(std::unique_ptr<MockFtraceProcfs> ftrace_procfs,
                       base::TaskRunner* runner,
                       std::unique_ptr<Table> table)
      : FtraceController(std::move(ftrace_procfs), runner, std::move(table)) {}

  MOCK_METHOD1(OnRawFtraceDataAvailable, void(size_t cpu));

  uint64_t NowMs() const override { return now_ms; }

  uint32_t drain_period_ms() { return GetDrainPeriodMs(); }

  std::function<void()> GetDataAvailableCallback(size_t cpu) {
    base::WeakPtr<FtraceController> weak_this = weak_factory_.GetWeakPtr();
    size_t generation = generation_;
    return [this, weak_this, generation, cpu] {
      OnDataAvailable(weak_this, generation, cpu, GetDrainPeriodMs());
    };
  }

  void WaitForData(size_t cpu) {
    while (true) {
      {
        std::unique_lock<std::mutex> lock(lock_);
        if (cpus_to_drain_[cpu])
          return;
      }
      usleep(5000);
    }
  }

  uint64_t now_ms = 0;

 private:
  TestFtraceController(const TestFtraceController&) = delete;
  TestFtraceController& operator=(const TestFtraceController&) = delete;
};

TEST(FtraceControllerTest, NonExistentEventsDontCrash) {
  NiceMock<MockTaskRunner> task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new NiceMock<MockFtraceProcfs>());
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"not_an_event"});

  std::unique_ptr<FtraceSink> sink = controller.CreateSink(config, &delegate);
}

TEST(FtraceControllerTest, RejectsBadEventNames) {
  NiceMock<MockTaskRunner> task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new NiceMock<MockFtraceProcfs>());
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"../try/to/escape"});
  EXPECT_FALSE(controller.CreateSink(config, &delegate));
}

TEST(FtraceControllerTest, OneSink) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "1"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/buffer_size_kb", _));
  std::unique_ptr<FtraceSink> sink = controller.CreateSink(config, &delegate);

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "0"));
  sink.reset();
}

TEST(FtraceControllerTest, MultipleSinks) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;

  FtraceConfig configA = CreateFtraceConfig({"foo"});
  FtraceConfig configB = CreateFtraceConfig({"foo", "bar"});

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "1"));
  std::unique_ptr<FtraceSink> sinkA = controller.CreateSink(configA, &delegate);

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kBarEnablePath, "1"));
  std::unique_ptr<FtraceSink> sinkB = controller.CreateSink(configB, &delegate);

  sinkA.reset();

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kBarEnablePath, "0"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "0"));
  sinkB.reset();
}

TEST(FtraceControllerTest, ControllerMayDieFirst) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  std::unique_ptr<TestFtraceController> controller(new TestFtraceController(
      std::move(ftrace_procfs), &task_runner, FakeTable()));

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "1"));
  std::unique_ptr<FtraceSink> sink = controller->CreateSink(config, &delegate);

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "0"));
  controller.reset();

  sink.reset();
}

TEST(FtraceControllerTest, TaskScheduling) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs(2u));
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  // For this test we don't care about calls to WriteToFile.
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(_, _)).Times(AnyNumber());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  std::unique_ptr<FtraceSink> sink = controller.CreateSink(config, &delegate);

  // Only one call to drain should be scheduled for the next drain period.
  EXPECT_CALL(task_runner, PostDelayedTask(_, 100));

  // However both CPUs should be drained.
  EXPECT_CALL(controller, OnRawFtraceDataAvailable(_)).Times(2);

  // Finally, another task should be posted to unblock the workers.
  EXPECT_CALL(task_runner, PostTask(_));

  // Simulate two worker threads reporting available data.
  auto on_data_available0 = controller.GetDataAvailableCallback(0u);
  std::thread worker0([on_data_available0] { on_data_available0(); });

  auto on_data_available1 = controller.GetDataAvailableCallback(1u);
  std::thread worker1([on_data_available1] { on_data_available1(); });

  // Poll until both worker threads have reported available data.
  controller.WaitForData(0u);
  controller.WaitForData(1u);

  // Run the task to drain all CPUs.
  task_runner.RunLastTask();

  // Run the task to unblock all workers.
  task_runner.RunLastTask();

  worker0.join();
  worker1.join();

  sink.reset();
}

// TODO(b/73452932): Fix and reenable this test.
TEST(FtraceControllerTest, DISABLED_DrainPeriodRespected) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  // For this test we don't care about calls to WriteToFile.
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(_, _)).Times(AnyNumber());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  // Test several cycles of a worker producing data and make sure the drain
  // delay is consistent with the drain period.
  std::unique_ptr<FtraceSink> sink = controller.CreateSink(config, &delegate);

  const int kCycles = 50;
  EXPECT_CALL(task_runner, PostDelayedTask(_, controller.drain_period_ms()))
      .Times(kCycles);
  EXPECT_CALL(controller, OnRawFtraceDataAvailable(_)).Times(kCycles);
  EXPECT_CALL(task_runner, PostTask(_)).Times(kCycles);

  // Simulate a worker thread continually reporting pages of available data.
  auto on_data_available = controller.GetDataAvailableCallback(0u);
  std::thread worker([on_data_available] {
    for (int i = 0; i < kCycles; i++)
      on_data_available();
  });

  for (int i = 0; i < kCycles; i++) {
    controller.WaitForData(0u);
    // Run two tasks: one to drain each CPU and another to unblock the worker.
    task_runner.RunLastTask();
    task_runner.RunLastTask();
    controller.now_ms += controller.drain_period_ms();
  }

  worker.join();
  sink.reset();
}

TEST(FtraceControllerTest, BackToBackEnableDisable) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  // For this test we don't care about calls to WriteToFile.
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(_, _)).Times(AnyNumber());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  EXPECT_CALL(task_runner, PostDelayedTask(_, 100)).Times(2);
  std::unique_ptr<FtraceSink> sink_a = controller.CreateSink(config, &delegate);

  auto on_data_available = controller.GetDataAvailableCallback(0u);
  std::thread worker([on_data_available] { on_data_available(); });
  controller.WaitForData(0u);

  // Disable the first sink and run the delayed task that it generated. It
  // should be a no-op.
  sink_a.reset();
  task_runner.RunLastTask();
  worker.join();

  // Register another sink and wait for it to generate data.
  std::unique_ptr<FtraceSink> sink_b = controller.CreateSink(config, &delegate);
  std::thread worker2([on_data_available] { on_data_available(); });
  controller.WaitForData(0u);

  // This drain should also be a no-op after the sink is unregistered.
  sink_b.reset();
  task_runner.RunLastTask();
  worker2.join();
}

TEST(FtraceControllerTest, BufferSize) {
  NiceMock<MockTaskRunner> task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  // For this test we don't care about most calls to WriteToFile.
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(_, _)).Times(AnyNumber());
  MockDelegate delegate;

  {
    // No buffer size -> good default.
    // 8192kb = 8mb
    EXPECT_CALL(*raw_ftrace_procfs,
                WriteToFile("/root/buffer_size_kb", "4096"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    auto sink = controller.CreateSink(config, &delegate);
  }

  {
    // Way too big buffer size -> good default.
    EXPECT_CALL(*raw_ftrace_procfs,
                WriteToFile("/root/buffer_size_kb", "4096"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_buffer_size_kb(10 * 1024 * 1024);
    auto sink = controller.CreateSink(config, &delegate);
  }

  {
    // The limit is 8mb, 9mb is too much.
    EXPECT_CALL(*raw_ftrace_procfs,
                WriteToFile("/root/buffer_size_kb", "4096"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    ON_CALL(*raw_ftrace_procfs, NumberOfCpus()).WillByDefault(Return(2));
    config.set_buffer_size_kb(9 * 1024);
    auto sink = controller.CreateSink(config, &delegate);
  }

  {
    // Your size ends up with less than 1 page per cpu -> 1 page.
    EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/buffer_size_kb", "4"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_buffer_size_kb(1);
    auto sink = controller.CreateSink(config, &delegate);
  }

  {
    // You picked a good size -> your size rounded to nearest page.
    EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/buffer_size_kb", "40"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_buffer_size_kb(42);
    auto sink = controller.CreateSink(config, &delegate);
  }

  {
    // You picked a good size -> your size rounded to nearest page.
    EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/buffer_size_kb", "40"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    ON_CALL(*raw_ftrace_procfs, NumberOfCpus()).WillByDefault(Return(2));
    config.set_buffer_size_kb(42);
    auto sink = controller.CreateSink(config, &delegate);
  }
}

TEST(FtraceControllerTest, PeriodicDrainConfig) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  // For this test we don't care about calls to WriteToFile.
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(_, _)).Times(AnyNumber());
  MockDelegate delegate;

  {
    // No period -> good default.
    FtraceConfig config = CreateFtraceConfig({"foo"});
    auto sink = controller.CreateSink(config, &delegate);
    EXPECT_EQ(100u, controller.drain_period_ms());
  }

  {
    // Pick a tiny value -> good default.
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_drain_period_ms(0);
    auto sink = controller.CreateSink(config, &delegate);
    EXPECT_EQ(100u, controller.drain_period_ms());
  }

  {
    // Pick a huge value -> good default.
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_drain_period_ms(1000 * 60 * 60);
    auto sink = controller.CreateSink(config, &delegate);
    EXPECT_EQ(100u, controller.drain_period_ms());
  }

  {
    // Pick a resonable value -> get that value.
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_drain_period_ms(200);
    auto sink = controller.CreateSink(config, &delegate);
    EXPECT_EQ(200u, controller.drain_period_ms());
  }
}

}  // namespace perfetto
