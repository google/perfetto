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

#include "src/traced/probes/ftrace/ftrace_controller.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/ftrace_config.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"
#include "src/tracing/core/trace_writer_for_testing.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::AnyNumber;
using testing::ByMove;
using testing::Invoke;
using testing::NiceMock;
using testing::MatchesRegex;
using testing::Return;
using testing::IsEmpty;
using testing::ElementsAre;
using testing::Pair;

using Table = perfetto::ProtoTranslationTable;
using FtraceEventBundle = perfetto::protos::pbzero::FtraceEventBundle;

namespace perfetto {

namespace {

constexpr char kFooEnablePath[] = "/root/events/group/foo/enable";
constexpr char kBarEnablePath[] = "/root/events/group/bar/enable";

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

  void OnPostDelayedTask(std::function<void()> task, int /*delay*/) {
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
  MOCK_METHOD2(PostDelayedTask, void(std::function<void()>, uint32_t delay_ms));
  MOCK_METHOD2(AddFileDescriptorWatch, void(int fd, std::function<void()>));
  MOCK_METHOD1(RemoveFileDescriptorWatch, void(int fd));

 private:
  std::mutex lock_;
  std::function<void()> task_;
};

class MockDelegate : public perfetto::FtraceSink::Delegate {
 public:
  MOCK_METHOD1(GetBundleForCpu,
               protozero::MessageHandle<FtraceEventBundle>(size_t));
  MOCK_METHOD3(OnBundleComplete_,
               void(size_t,
                    protozero::MessageHandle<FtraceEventBundle>&,
                    const FtraceMetadata& metadata));

  void OnBundleComplete(size_t cpu,
                        protozero::MessageHandle<FtraceEventBundle> bundle,
                        const FtraceMetadata& metadata) override {
    OnBundleComplete_(cpu, bundle, metadata);
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

  return std::unique_ptr<Table>(
      new Table(events, std::move(common_fields),
                ProtoTranslationTable::DefaultPageHeaderSpecForTesting()));
}

std::unique_ptr<FtraceConfigMuxer> FakeModel(
    FtraceProcfs* ftrace,
    const ProtoTranslationTable* table) {
  return std::unique_ptr<FtraceConfigMuxer>(
      new FtraceConfigMuxer(ftrace, table));
}

class MockFtraceProcfs : public FtraceProcfs {
 public:
  explicit MockFtraceProcfs(size_t cpu_count = 1) : FtraceProcfs("/root/") {
    ON_CALL(*this, NumberOfCpus()).WillByDefault(Return(cpu_count));
    EXPECT_CALL(*this, NumberOfCpus()).Times(AnyNumber());

    ON_CALL(*this, ReadFileIntoString("/root/trace_clock"))
        .WillByDefault(Return("local global [boot]"));
    EXPECT_CALL(*this, ReadFileIntoString("/root/trace_clock"))
        .Times(AnyNumber());

    ON_CALL(*this, WriteToFile(_, _)).WillByDefault(Return(true));
    ON_CALL(*this, ClearFile(_)).WillByDefault(Return(true));

    ON_CALL(*this, WriteToFile("/root/tracing_on", _))
        .WillByDefault(Invoke(this, &MockFtraceProcfs::WriteTracingOn));
    ON_CALL(*this, ReadOneCharFromFile("/root/tracing_on"))
        .WillByDefault(Invoke(this, &MockFtraceProcfs::ReadTracingOn));
    EXPECT_CALL(*this, ReadOneCharFromFile("/root/tracing_on"))
        .Times(AnyNumber());
  }

  bool WriteTracingOn(const std::string& /*path*/, const std::string& value) {
    PERFETTO_CHECK(value == "1" || value == "0");
    tracing_on_ = value == "1";
    return true;
  }

  char ReadTracingOn(const std::string& /*path*/) {
    return tracing_on_ ? '1' : '0';
  }

  base::ScopedFile OpenPipeForCpu(size_t /*cpu*/) override {
    return base::ScopedFile(open("/dev/null", O_RDONLY));
  }

  MOCK_METHOD2(WriteToFile,
               bool(const std::string& path, const std::string& str));
  MOCK_CONST_METHOD0(NumberOfCpus, size_t());
  MOCK_METHOD1(ReadOneCharFromFile, char(const std::string& path));
  MOCK_METHOD1(ClearFile, bool(const std::string& path));
  MOCK_CONST_METHOD1(ReadFileIntoString, std::string(const std::string& path));

  bool is_tracing_on() { return tracing_on_; }

 private:
  bool tracing_on_ = false;
};

}  // namespace

class TestFtraceController : public FtraceController {
 public:
  TestFtraceController(std::unique_ptr<MockFtraceProcfs> ftrace_procfs,
                       std::unique_ptr<Table> table,
                       std::unique_ptr<FtraceConfigMuxer> model,
                       std::unique_ptr<MockTaskRunner> runner,
                       MockFtraceProcfs* raw_procfs)
      : FtraceController(std::move(ftrace_procfs),
                         std::move(table),
                         std::move(model),
                         runner.get()),
        runner_(std::move(runner)),
        procfs_(raw_procfs) {}

  MOCK_METHOD1(OnRawFtraceDataAvailable, void(size_t cpu));

  MockTaskRunner* runner() { return runner_.get(); }
  MockFtraceProcfs* procfs() { return procfs_; }

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

  std::unique_ptr<MockTaskRunner> runner_;
  MockFtraceProcfs* procfs_;
};

namespace {

std::unique_ptr<TestFtraceController> CreateTestController(
    bool runner_is_nice_mock,
    bool procfs_is_nice_mock,
    size_t cpu_count = 1) {
  std::unique_ptr<MockTaskRunner> runner;
  if (runner_is_nice_mock) {
    runner = std::unique_ptr<MockTaskRunner>(new NiceMock<MockTaskRunner>());
  } else {
    runner = std::unique_ptr<MockTaskRunner>(new MockTaskRunner());
  }

  auto table = FakeTable();

  std::unique_ptr<MockFtraceProcfs> ftrace_procfs;
  if (procfs_is_nice_mock) {
    ftrace_procfs = std::unique_ptr<MockFtraceProcfs>(
        new NiceMock<MockFtraceProcfs>(cpu_count));
  } else {
    ftrace_procfs =
        std::unique_ptr<MockFtraceProcfs>(new MockFtraceProcfs(cpu_count));
  }

  auto model = FakeModel(ftrace_procfs.get(), table.get());

  MockFtraceProcfs* raw_procfs = ftrace_procfs.get();
  return std::unique_ptr<TestFtraceController>(new TestFtraceController(
      std::move(ftrace_procfs), std::move(table), std::move(model),
      std::move(runner), raw_procfs));
}

}  // namespace

TEST(FtraceControllerTest, NonExistentEventsDontCrash) {
  auto controller =
      CreateTestController(true /* nice runner */, true /* nice procfs */);

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"not_an_event"});

  std::unique_ptr<FtraceSink> sink = controller->CreateSink(config, &delegate);
}

TEST(FtraceControllerTest, RejectsBadEventNames) {
  auto controller =
      CreateTestController(true /* nice runner */, true /* nice procfs */);

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"../try/to/escape"});
  EXPECT_FALSE(controller->CreateSink(config, &delegate));
  EXPECT_FALSE(controller->procfs()->is_tracing_on());
}

TEST(FtraceControllerTest, OneSink) {
  auto controller =
      CreateTestController(true /* nice runner */, false /* nice procfs */);

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "1"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", _));
  std::unique_ptr<FtraceSink> sink = controller->CreateSink(config, &delegate);

  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  EXPECT_TRUE(controller->procfs()->is_tracing_on());

  sink.reset();
  EXPECT_FALSE(controller->procfs()->is_tracing_on());
}

TEST(FtraceControllerTest, MultipleSinks) {
  auto controller =
      CreateTestController(false /* nice runner */, false /* nice procfs */);

  MockDelegate delegate;

  FtraceConfig configA = CreateFtraceConfig({"foo"});
  FtraceConfig configB = CreateFtraceConfig({"foo", "bar"});

  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "1"));
  std::unique_ptr<FtraceSink> sinkA =
      controller->CreateSink(configA, &delegate);

  EXPECT_CALL(*controller->procfs(), WriteToFile(kBarEnablePath, "1"));
  std::unique_ptr<FtraceSink> sinkB =
      controller->CreateSink(configB, &delegate);

  sinkA.reset();

  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kBarEnablePath, "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  sinkB.reset();
}

TEST(FtraceControllerTest, ControllerMayDieFirst) {
  auto controller =
      CreateTestController(false /* nice runner */, false /* nice procfs */);

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "1"));
  std::unique_ptr<FtraceSink> sink = controller->CreateSink(config, &delegate);

  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  controller.reset();

  sink.reset();
}

TEST(FtraceControllerTest, TaskScheduling) {
  auto controller = CreateTestController(
      false /* nice runner */, false /* nice procfs */, 2 /* num cpus */);

  // For this test we don't care about calls to WriteToFile/ClearFile.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), ClearFile(_)).Times(AnyNumber());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  std::unique_ptr<FtraceSink> sink = controller->CreateSink(config, &delegate);

  // Only one call to drain should be scheduled for the next drain period.
  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, 100));

  // However both CPUs should be drained.
  EXPECT_CALL(*controller, OnRawFtraceDataAvailable(_)).Times(2);

  // Finally, another task should be posted to unblock the workers.
  EXPECT_CALL(*controller->runner(), PostTask(_));

  // Simulate two worker threads reporting available data.
  auto on_data_available0 = controller->GetDataAvailableCallback(0u);
  std::thread worker0([on_data_available0] { on_data_available0(); });

  auto on_data_available1 = controller->GetDataAvailableCallback(1u);
  std::thread worker1([on_data_available1] { on_data_available1(); });

  // Poll until both worker threads have reported available data.
  controller->WaitForData(0u);
  controller->WaitForData(1u);

  // Run the task to drain all CPUs.
  controller->runner()->RunLastTask();

  // Run the task to unblock all workers.
  controller->runner()->RunLastTask();

  worker0.join();
  worker1.join();

  sink.reset();
}

// TODO(b/73452932): Fix and reenable this test.
TEST(FtraceControllerTest, DISABLED_DrainPeriodRespected) {
  auto controller =
      CreateTestController(false /* nice runner */, false /* nice procfs */);

  // For this test we don't care about calls to WriteToFile/ClearFile.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), ClearFile(_)).Times(AnyNumber());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  // Test several cycles of a worker producing data and make sure the drain
  // delay is consistent with the drain period.
  std::unique_ptr<FtraceSink> sink = controller->CreateSink(config, &delegate);

  const int kCycles = 50;
  EXPECT_CALL(*controller->runner(),
              PostDelayedTask(_, controller->drain_period_ms()))
      .Times(kCycles);
  EXPECT_CALL(*controller, OnRawFtraceDataAvailable(_)).Times(kCycles);
  EXPECT_CALL(*controller->runner(), PostTask(_)).Times(kCycles);

  // Simulate a worker thread continually reporting pages of available data.
  auto on_data_available = controller->GetDataAvailableCallback(0u);
  std::thread worker([on_data_available] {
    for (int i = 0; i < kCycles; i++)
      on_data_available();
  });

  for (int i = 0; i < kCycles; i++) {
    controller->WaitForData(0u);
    // Run two tasks: one to drain each CPU and another to unblock the worker.
    controller->runner()->RunLastTask();
    controller->runner()->RunLastTask();
    controller->now_ms += controller->drain_period_ms();
  }

  worker.join();
  sink.reset();
}

TEST(FtraceControllerTest, BackToBackEnableDisable) {
  auto controller =
      CreateTestController(false /* nice runner */, false /* nice procfs */);

  // For this test we don't care about calls to WriteToFile/ClearFile.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), ClearFile(_)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), ReadOneCharFromFile("/root/tracing_on"))
      .Times(AnyNumber());

  MockDelegate delegate;
  FtraceConfig config = CreateFtraceConfig({"foo"});

  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, 100)).Times(2);
  std::unique_ptr<FtraceSink> sink_a =
      controller->CreateSink(config, &delegate);

  auto on_data_available = controller->GetDataAvailableCallback(0u);
  std::thread worker([on_data_available] { on_data_available(); });
  controller->WaitForData(0u);

  // Disable the first sink and run the delayed task that it generated. It
  // should be a no-op.
  sink_a.reset();
  controller->runner()->RunLastTask();
  worker.join();

  // Register another sink and wait for it to generate data.
  std::unique_ptr<FtraceSink> sink_b =
      controller->CreateSink(config, &delegate);
  std::thread worker2([on_data_available] { on_data_available(); });
  controller->WaitForData(0u);

  // This drain should also be a no-op after the sink is unregistered.
  sink_b.reset();
  controller->runner()->RunLastTask();
  worker2.join();
}

TEST(FtraceControllerTest, BufferSize) {
  auto controller =
      CreateTestController(true /* nice runner */, false /* nice procfs */);

  // For this test we don't care about most calls to WriteToFile/ClearFile.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), ClearFile(_)).Times(AnyNumber());
  MockDelegate delegate;

  {
    // No buffer size -> good default.
    // 8192kb = 8mb
    EXPECT_CALL(*controller->procfs(),
                WriteToFile("/root/buffer_size_kb", "512"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    auto sink = controller->CreateSink(config, &delegate);
  }

  {
    // Way too big buffer size -> max size.
    EXPECT_CALL(*controller->procfs(),
                WriteToFile("/root/buffer_size_kb", "65536"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_buffer_size_kb(10 * 1024 * 1024);
    auto sink = controller->CreateSink(config, &delegate);
  }

  {
    // The limit is 64mb, 65mb is too much.
    EXPECT_CALL(*controller->procfs(),
                WriteToFile("/root/buffer_size_kb", "65536"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    ON_CALL(*controller->procfs(), NumberOfCpus()).WillByDefault(Return(2));
    config.set_buffer_size_kb(65 * 1024);
    auto sink = controller->CreateSink(config, &delegate);
  }

  {
    // Your size ends up with less than 1 page per cpu -> 1 page.
    EXPECT_CALL(*controller->procfs(),
                WriteToFile("/root/buffer_size_kb", "4"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_buffer_size_kb(1);
    auto sink = controller->CreateSink(config, &delegate);
  }

  {
    // You picked a good size -> your size rounded to nearest page.
    EXPECT_CALL(*controller->procfs(),
                WriteToFile("/root/buffer_size_kb", "40"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_buffer_size_kb(42);
    auto sink = controller->CreateSink(config, &delegate);
  }

  {
    // You picked a good size -> your size rounded to nearest page.
    EXPECT_CALL(*controller->procfs(),
                WriteToFile("/root/buffer_size_kb", "40"));
    FtraceConfig config = CreateFtraceConfig({"foo"});
    ON_CALL(*controller->procfs(), NumberOfCpus()).WillByDefault(Return(2));
    config.set_buffer_size_kb(42);
    auto sink = controller->CreateSink(config, &delegate);
  }
}

TEST(FtraceControllerTest, PeriodicDrainConfig) {
  auto controller =
      CreateTestController(true /* nice runner */, false /* nice procfs */);

  // For this test we don't care about calls to WriteToFile/ClearFile.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), ClearFile(_)).Times(AnyNumber());
  MockDelegate delegate;

  {
    // No period -> good default.
    FtraceConfig config = CreateFtraceConfig({"foo"});
    auto sink = controller->CreateSink(config, &delegate);
    EXPECT_EQ(100u, controller->drain_period_ms());
  }

  {
    // Pick a tiny value -> good default.
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_drain_period_ms(0);
    auto sink = controller->CreateSink(config, &delegate);
    EXPECT_EQ(100u, controller->drain_period_ms());
  }

  {
    // Pick a huge value -> good default.
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_drain_period_ms(1000 * 60 * 60);
    auto sink = controller->CreateSink(config, &delegate);
    EXPECT_EQ(100u, controller->drain_period_ms());
  }

  {
    // Pick a resonable value -> get that value.
    FtraceConfig config = CreateFtraceConfig({"foo"});
    config.set_drain_period_ms(200);
    auto sink = controller->CreateSink(config, &delegate);
    EXPECT_EQ(200u, controller->drain_period_ms());
  }
}

TEST(FtraceMetadataTest, Clear) {
  FtraceMetadata metadata;
  metadata.inode_and_device.push_back(std::make_pair(1, 1));
  metadata.pids.push_back(2);
  metadata.overwrite_count = 3;
  metadata.last_seen_device_id = 100;
  metadata.Clear();
  EXPECT_THAT(metadata.inode_and_device, IsEmpty());
  EXPECT_THAT(metadata.pids, IsEmpty());
  EXPECT_EQ(0u, metadata.overwrite_count);
  EXPECT_EQ(BlockDeviceID(0), metadata.last_seen_device_id);
}

TEST(FtraceMetadataTest, AddDevice) {
  FtraceMetadata metadata;
  metadata.AddDevice(1);
  EXPECT_EQ(BlockDeviceID(1), metadata.last_seen_device_id);
  metadata.AddDevice(3);
  EXPECT_EQ(BlockDeviceID(3), metadata.last_seen_device_id);
}

TEST(FtraceMetadataTest, AddInode) {
  FtraceMetadata metadata;
  metadata.AddCommonPid(getpid() + 1);
  metadata.AddDevice(3);
  metadata.AddInode(2);
  metadata.AddInode(1);
  metadata.AddCommonPid(getpid() + 1);
  metadata.AddDevice(4);
  metadata.AddInode(3);

  // Check activity from ourselves is excluded.
  metadata.AddCommonPid(getpid());
  metadata.AddDevice(5);
  metadata.AddInode(5);

  EXPECT_THAT(metadata.inode_and_device,
              ElementsAre(Pair(2, 3), Pair(1, 3), Pair(3, 4)));
}

TEST(FtraceMetadataTest, AddPid) {
  FtraceMetadata metadata;
  metadata.AddPid(1);
  metadata.AddPid(2);
  metadata.AddPid(2);
  metadata.AddPid(3);
  EXPECT_THAT(metadata.pids, ElementsAre(1, 2, 3));
}

TEST(FtraceStatsTest, Write) {
  FtraceStats stats{};
  FtraceCpuStats cpu_stats{};
  cpu_stats.cpu = 0;
  cpu_stats.entries = 1;
  cpu_stats.overrun = 2;
  stats.cpu_stats.push_back(cpu_stats);

  std::unique_ptr<TraceWriterForTesting> writer =
      std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
  {
    auto packet = writer->NewTracePacket();
    auto* out = packet->set_ftrace_stats();
    stats.Write(out);
  }

  std::unique_ptr<protos::TracePacket> result_packet = writer->ParseProto();
  auto result = result_packet->ftrace_stats().cpu_stats(0);
  EXPECT_EQ(result.cpu(), 0);
  EXPECT_EQ(result.entries(), 1);
  EXPECT_EQ(result.overrun(), 2);
}

}  // namespace perfetto
