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
#include "proto_translation_table.h"

#include "protos/ftrace/ftrace_event_bundle.pbzero.h"

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
    ON_CALL(*this, PostDelayedTask(_, _))
        .WillByDefault(Invoke(this, &MockTaskRunner::OnPostDelayedTask));
  }

  void OnPostDelayedTask(std::function<void()> task, int _delay) {
    task_ = std::move(task);
  }

  void RunLastTask() { task_(); }

  std::function<void()> TakeTask() { return std::move(task_); }

  MOCK_METHOD1(PostTask, void(std::function<void()>));
  MOCK_METHOD2(PostDelayedTask, void(std::function<void()>, int delay_ms));
  MOCK_METHOD2(AddFileDescriptorWatch, void(int fd, std::function<void()>));
  MOCK_METHOD1(RemoveFileDescriptorWatch, void(int fd));

 private:
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
  MockFtraceProcfs() : FtraceProcfs("/root/") {
    ON_CALL(*this, NumberOfCpus()).WillByDefault(Return(1));
    EXPECT_CALL(*this, NumberOfCpus()).Times(AnyNumber());
  }

  MOCK_METHOD2(WriteToFile,
               bool(const std::string& path, const std::string& str));
  MOCK_CONST_METHOD0(NumberOfCpus, size_t());
};

class TestFtraceController : public FtraceController {
 public:
  TestFtraceController(std::unique_ptr<MockFtraceProcfs> ftrace_procfs,
                       base::TaskRunner* runner,
                       std::unique_ptr<Table> table)
      : FtraceController(std::move(ftrace_procfs), runner, std::move(table)) {}

  MOCK_METHOD1(OnRawFtraceDataAvailable, bool(size_t cpu));

 private:
  TestFtraceController(const TestFtraceController&) = delete;
  TestFtraceController& operator=(const TestFtraceController&) = delete;
};

}  // namespace

TEST(FtraceControllerTest, NoExistentEventsDontCrash) {
  NiceMock<MockTaskRunner> task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new NiceMock<MockFtraceProcfs>());
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;
  FtraceConfig config;
  config.AddEvent("not_an_event");

  std::unique_ptr<FtraceSink> sink = controller.CreateSink(config, &delegate);
}

TEST(FtraceControllerTest, OneSink) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  MockDelegate delegate;
  FtraceConfig config({"foo"});

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(task_runner, PostDelayedTask(_, _));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "1"));
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

  FtraceConfig configA({"foo"});
  FtraceConfig configB({"foo", "bar"});

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "1"));
  EXPECT_CALL(task_runner, PostDelayedTask(_, _));
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
  FtraceConfig config({"foo"});

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "1"));
  EXPECT_CALL(task_runner, PostDelayedTask(_, _));
  std::unique_ptr<FtraceSink> sink = controller->CreateSink(config, &delegate);

  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile("/root/tracing_on", "0"));
  controller.reset();

  sink.reset();
}

TEST(FtraceControllerTest, TaskScheduling) {
  MockTaskRunner task_runner;
  auto ftrace_procfs =
      std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs());
  auto raw_ftrace_procfs = ftrace_procfs.get();
  TestFtraceController controller(std::move(ftrace_procfs), &task_runner,
                                  FakeTable());

  // For this test we don't care about calls to WriteToFile.
  EXPECT_CALL(*raw_ftrace_procfs, WriteToFile(_, _)).Times(AnyNumber());

  MockDelegate delegate;
  FtraceConfig config({"foo"});

  EXPECT_CALL(task_runner, PostDelayedTask(_, 100));
  std::unique_ptr<FtraceSink> sink = controller.CreateSink(config, &delegate);

  // Running task will call OnRawFtraceDataAvailable:
  EXPECT_CALL(controller, OnRawFtraceDataAvailable(_)).WillOnce(Return(true));
  // And since we return true (= there is more data) we re-schedule immediately:
  EXPECT_CALL(task_runner, PostDelayedTask(_, 0));
  task_runner.RunLastTask();

  // Running task will call OnRawFtraceDataAvailable:
  EXPECT_CALL(controller, OnRawFtraceDataAvailable(_)).WillOnce(Return(false));
  // And since we return false (= no more data) we re-schedule in 100ms:
  EXPECT_CALL(task_runner, PostDelayedTask(_, 100));
  task_runner.RunLastTask();

  sink.reset();

  // The task may be run after the sink is gone, in this case we shouldn't call
  // OnRawFtraceDataAvailable and shouldn't reschedule.
  task_runner.RunLastTask();
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
  FtraceConfig config({"foo"});

  EXPECT_CALL(task_runner, PostDelayedTask(_, 100));
  std::unique_ptr<FtraceSink> sink_a = controller.CreateSink(config, &delegate);
  sink_a.reset();
  std::function<void()> task_a = task_runner.TakeTask();

  EXPECT_CALL(task_runner, PostDelayedTask(_, 100));
  std::unique_ptr<FtraceSink> sink_b = controller.CreateSink(config, &delegate);
  std::function<void()> task_b = task_runner.TakeTask();

  // Task A shouldn't reschedule:
  task_a();
  // But task B should:
  EXPECT_CALL(controller, OnRawFtraceDataAvailable(_)).WillOnce(Return(false));
  EXPECT_CALL(task_runner, PostDelayedTask(_, 100));
  task_b();

  sink_b.reset();
}

}  // namespace perfetto
