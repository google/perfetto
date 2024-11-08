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

#include "perfetto/ext/base/file_utils.h"
#include "src/traced/probes/ftrace/compact_sched.h"
#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/ftrace_config_utils.h"
#include "src/traced/probes/ftrace/ftrace_data_source.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_stats.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

using testing::_;
using testing::AnyNumber;
using testing::ByMove;
using testing::ElementsAre;
using testing::Invoke;
using testing::IsEmpty;
using testing::MatchesRegex;
using testing::Mock;
using testing::NiceMock;
using testing::Pair;
using testing::Return;
using testing::UnorderedElementsAre;

using Table = perfetto::ProtoTranslationTable;

namespace perfetto {

namespace {

constexpr char kFooEnablePath[] = "/root/events/group/foo/enable";
constexpr char kBarEnablePath[] = "/root/events/group/bar/enable";

std::string PageSizeKb() {
  return std::to_string(base::GetSysPageSize() / 1024);
}

class MockTaskRunner : public base::TaskRunner {
 public:
  MOCK_METHOD(void, PostTask, (std::function<void()>), (override));
  MOCK_METHOD(void,
              PostDelayedTask,
              (std::function<void()>, uint32_t delay_ms),
              (override));
  MOCK_METHOD(void,
              AddFileDescriptorWatch,
              (int fd, std::function<void()>),
              (override));
  MOCK_METHOD(void, RemoveFileDescriptorWatch, (int fd), (override));
  MOCK_METHOD(bool, RunsTasksOnCurrentThread, (), (const, override));
};

std::unique_ptr<Table> FakeTable(FtraceProcfs* ftrace) {
  std::vector<Field> common_fields;
  std::vector<Event> events;
  {
    events.push_back(Event{});
    auto& event = events.back();
    event.name = "foo";
    event.group = "group";
    event.ftrace_event_id = 1;
  }
  {
    events.push_back(Event{});
    auto& event = events.back();
    event.name = "bar";
    event.group = "group";
    event.ftrace_event_id = 10;
  }

  return std::unique_ptr<Table>(
      new Table(ftrace, events, std::move(common_fields),
                ProtoTranslationTable::DefaultPageHeaderSpecForTesting(),
                InvalidCompactSchedEventFormatForTesting(), PrintkMap()));
}

std::unique_ptr<FtraceConfigMuxer> FakeMuxer(FtraceProcfs* ftrace,
                                             AtraceWrapper* atrace_wrapper,
                                             ProtoTranslationTable* table) {
  return std::unique_ptr<FtraceConfigMuxer>(new FtraceConfigMuxer(
      ftrace, atrace_wrapper, table, SyscallTable(Architecture::kUnknown), {}));
}

class MockFtraceProcfs : public FtraceProcfs {
 public:
  explicit MockFtraceProcfs(const std::string& root, size_t cpu_count = 1)
      : FtraceProcfs(root) {
    ON_CALL(*this, NumberOfCpus()).WillByDefault(Return(cpu_count));
    EXPECT_CALL(*this, NumberOfCpus()).Times(AnyNumber());

    ON_CALL(*this, ReadFileIntoString(root + "trace_clock"))
        .WillByDefault(Return("local global [boot]"));
    EXPECT_CALL(*this, ReadFileIntoString(root + "trace_clock"))
        .Times(AnyNumber());

    ON_CALL(*this, ReadFileIntoString(root + "per_cpu/cpu0/stats"))
        .WillByDefault(Return(""));
    EXPECT_CALL(*this, ReadFileIntoString(root + "per_cpu/cpu0/stats"))
        .Times(AnyNumber());

    ON_CALL(*this, ReadFileIntoString(root + "events//not_an_event/format"))
        .WillByDefault(Return(""));
    EXPECT_CALL(*this, ReadFileIntoString(root + "events//not_an_event/format"))
        .Times(AnyNumber());

    ON_CALL(*this, ReadFileIntoString(root + "events/group/bar/format"))
        .WillByDefault(Return(""));
    EXPECT_CALL(*this, ReadFileIntoString(root + "events/group/bar/format"))
        .Times(AnyNumber());

    ON_CALL(*this, WriteToFile(_, _)).WillByDefault(Return(true));
    ON_CALL(*this, ClearFile(_)).WillByDefault(Return(true));

    ON_CALL(*this, WriteToFile(root + "tracing_on", _))
        .WillByDefault(Invoke(this, &MockFtraceProcfs::WriteTracingOn));
    ON_CALL(*this, ReadOneCharFromFile(root + "tracing_on"))
        .WillByDefault(Invoke(this, &MockFtraceProcfs::ReadTracingOn));
    EXPECT_CALL(*this, ReadOneCharFromFile(root + "tracing_on"))
        .Times(AnyNumber());

    ON_CALL(*this, WriteToFile(root + "current_tracer", _))
        .WillByDefault(Invoke(this, &MockFtraceProcfs::WriteCurrentTracer));
    ON_CALL(*this, ReadFileIntoString(root + "current_tracer"))
        .WillByDefault(Invoke(this, &MockFtraceProcfs::ReadCurrentTracer));
    EXPECT_CALL(*this, ReadFileIntoString(root + "current_tracer"))
        .Times(AnyNumber());

    ON_CALL(*this, ReadFileIntoString(root + "buffer_percent"))
        .WillByDefault(Return("50\n"));
    EXPECT_CALL(*this, ReadFileIntoString(root + "buffer_percent"))
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

  bool WriteCurrentTracer(const std::string& /*path*/,
                          const std::string& value) {
    current_tracer_ = value;
    return true;
  }

  std::string ReadCurrentTracer(const std::string& /*path*/) {
    return current_tracer_;
  }

  base::ScopedFile OpenPipeForCpu(size_t /*cpu*/) override {
    return base::ScopedFile(base::OpenFile("/dev/null", O_RDONLY));
  }

  MOCK_METHOD(bool,
              WriteToFile,
              (const std::string& path, const std::string& str),
              (override));
  MOCK_METHOD(size_t, NumberOfCpus, (), (const, override));
  MOCK_METHOD(char, ReadOneCharFromFile, (const std::string& path), (override));
  MOCK_METHOD(bool, ClearFile, (const std::string& path), (override));
  MOCK_METHOD(bool, IsFileWriteable, (const std::string& path), (override));
  MOCK_METHOD(std::string,
              ReadFileIntoString,
              (const std::string& path),
              (const, override));

  bool is_tracing_on() { return tracing_on_; }

 private:
  bool tracing_on_ = true;
  std::string current_tracer_ = "nop";
};

class MockAtraceWrapper : public AtraceWrapper {
 public:
  MOCK_METHOD(bool, RunAtrace, (const std::vector<std::string>&, std::string*));
  MOCK_METHOD(bool, SupportsUserspaceOnly, ());
  MOCK_METHOD(bool, SupportsPreferSdk, ());
};

}  // namespace

class TestFtraceController : public FtraceController,
                             public FtraceController::Observer {
 public:
  TestFtraceController(std::unique_ptr<MockFtraceProcfs> ftrace_procfs,
                       std::unique_ptr<Table> table,
                       std::unique_ptr<AtraceWrapper> atrace_wrapper,
                       std::unique_ptr<FtraceConfigMuxer> muxer,
                       std::unique_ptr<MockTaskRunner> runner,
                       MockFtraceProcfs* raw_procfs)
      : FtraceController(std::move(ftrace_procfs),
                         std::move(table),
                         std::move(atrace_wrapper),
                         std::move(muxer),
                         runner.get(),
                         /*observer=*/this),
        runner_(std::move(runner)),
        primary_procfs_(raw_procfs) {}

  MockTaskRunner* runner() { return runner_.get(); }
  MockFtraceProcfs* procfs() { return primary_procfs_; }
  uint32_t tick_period_ms() { return GetTickPeriodMs(); }

  std::unique_ptr<FtraceDataSource> AddFakeDataSource(const FtraceConfig& cfg) {
    std::unique_ptr<FtraceDataSource> data_source(new FtraceDataSource(
        GetWeakPtr(), 0 /* session id */, cfg, nullptr /* trace_writer */));
    if (!AddDataSource(data_source.get()))
      return nullptr;
    return data_source;
  }

  uint64_t NowMs() const override { return 0; }
  void OnFtraceDataWrittenIntoDataSourceBuffers() override {}

  bool InstanceExists(const std::string& instance_name) {
    auto* instance = GetInstance(instance_name);
    return instance != nullptr;
  }

  void PrepareMockProcfsForInstance(const std::string& name,
                                    std::unique_ptr<MockFtraceProcfs> fs) {
    pending_instance_procfs_[name] = std::move(fs);
  }

  MockFtraceProcfs* GetInstanceMockProcfs(const std::string& instance_name) {
    auto* instance = GetInstance(instance_name);
    PERFETTO_CHECK(instance);
    return reinterpret_cast<MockFtraceProcfs*>(instance->ftrace_procfs.get());
  }

  std::unique_ptr<FtraceInstanceState> CreateSecondaryInstance(
      const std::string& instance_name) override {
    auto ftrace_procfs = std::move(pending_instance_procfs_[instance_name]);
    PERFETTO_CHECK(ftrace_procfs);

    auto table = FakeTable(ftrace_procfs.get());
    auto muxer = FakeMuxer(ftrace_procfs.get(), atrace_wrapper(), table.get());
    return std::unique_ptr<FtraceController::FtraceInstanceState>(
        new FtraceController::FtraceInstanceState(
            std::move(ftrace_procfs), std::move(table), std::move(muxer)));
  }

 private:
  TestFtraceController(const TestFtraceController&) = delete;
  TestFtraceController& operator=(const TestFtraceController&) = delete;

  std::unique_ptr<MockTaskRunner> runner_;
  MockFtraceProcfs* primary_procfs_;
  std::map<std::string, std::unique_ptr<MockFtraceProcfs>>
      pending_instance_procfs_;
};

namespace {

std::unique_ptr<TestFtraceController> CreateTestController(
    bool procfs_is_nice_mock,
    size_t cpu_count = 1) {
  std::unique_ptr<MockTaskRunner> runner =
      std::unique_ptr<MockTaskRunner>(new NiceMock<MockTaskRunner>());

  std::unique_ptr<MockFtraceProcfs> ftrace_procfs;
  if (procfs_is_nice_mock) {
    ftrace_procfs = std::unique_ptr<MockFtraceProcfs>(
        new NiceMock<MockFtraceProcfs>("/root/", cpu_count));
  } else {
    ftrace_procfs = std::unique_ptr<MockFtraceProcfs>(
        new MockFtraceProcfs("/root/", cpu_count));
  }

  auto atrace_wrapper = std::make_unique<NiceMock<MockAtraceWrapper>>();

  auto table = FakeTable(ftrace_procfs.get());

  auto muxer =
      FakeMuxer(ftrace_procfs.get(), atrace_wrapper.get(), table.get());

  MockFtraceProcfs* raw_procfs = ftrace_procfs.get();
  return std::unique_ptr<TestFtraceController>(new TestFtraceController(
      std::move(ftrace_procfs), std::move(table), std::move(atrace_wrapper),
      std::move(muxer), std::move(runner), raw_procfs));
}

}  // namespace

TEST(FtraceControllerTest, NonExistentEventsDontCrash) {
  auto controller = CreateTestController(true /* nice procfs */);

  FtraceConfig config = CreateFtraceConfig({"not_an_event"});
  EXPECT_TRUE(controller->AddFakeDataSource(config));
}

TEST(FtraceControllerTest, RejectsBadEventNames) {
  auto controller = CreateTestController(true /* nice procfs */);

  FtraceConfig config = CreateFtraceConfig({"../try/to/escape"});
  EXPECT_FALSE(controller->AddFakeDataSource(config));
  config = CreateFtraceConfig({"/event"});
  EXPECT_FALSE(controller->AddFakeDataSource(config));
  config = CreateFtraceConfig({"event/"});
  EXPECT_FALSE(controller->AddFakeDataSource(config));
}

TEST(FtraceControllerTest, OneSink) {
  auto controller = CreateTestController(false /* nice procfs */);

  // No read tasks posted as part of adding the data source.
  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, _)).Times(0);

  FtraceConfig config = CreateFtraceConfig({"group/foo"});

  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "1"));

  auto data_source = controller->AddFakeDataSource(config);
  ASSERT_TRUE(data_source);

  // Verify that no read tasks have been posted. And set up expectation that
  // a single recurring read task will be posted as part of starting the data
  // source.
  Mock::VerifyAndClearExpectations(controller->runner());
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_percent", _))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, _)).Times(1);
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  ASSERT_TRUE(controller->StartDataSource(data_source.get()));

  // Verify single posted read task.
  Mock::VerifyAndClearExpectations(controller->runner());

  // State clearing on tracing teardown.
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(),
              WriteToFile("/root/buffer_size_kb", PageSizeKb()));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));

  data_source.reset();
  EXPECT_TRUE(controller->procfs()->is_tracing_on());
}

TEST(FtraceControllerTest, MultipleSinks) {
  auto controller = CreateTestController(false /* nice procfs */);

  FtraceConfig configA = CreateFtraceConfig({"group/foo"});
  FtraceConfig configB = CreateFtraceConfig({"group/foo", "group/bar"});

  // No read tasks posted as part of adding the data sources.
  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, _)).Times(0);

  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "1"));
  auto data_sourceA = controller->AddFakeDataSource(configA);
  EXPECT_CALL(*controller->procfs(), WriteToFile(kBarEnablePath, "1"));
  auto data_sourceB = controller->AddFakeDataSource(configB);

  // Verify that no read tasks have been posted. And set up expectation that
  // a single recurring read task will be posted as part of starting the data
  // sources.
  Mock::VerifyAndClearExpectations(controller->runner());
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_percent", _))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, _)).Times(1);
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  ASSERT_TRUE(controller->StartDataSource(data_sourceA.get()));
  ASSERT_TRUE(controller->StartDataSource(data_sourceB.get()));

  // Verify single posted read task.
  Mock::VerifyAndClearExpectations(controller->runner());

  data_sourceA.reset();
  EXPECT_TRUE(controller->procfs()->is_tracing_on());

  // State clearing on tracing teardown.
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kBarEnablePath, "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(),
              WriteToFile("/root/buffer_size_kb", PageSizeKb()));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  data_sourceB.reset();
  EXPECT_TRUE(controller->procfs()->is_tracing_on());
}

TEST(FtraceControllerTest, ControllerMayDieFirst) {
  auto controller = CreateTestController(false /* nice procfs */);

  FtraceConfig config = CreateFtraceConfig({"group/foo"});

  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "1"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_percent", _))
      .WillRepeatedly(Return(true));
  auto data_source = controller->AddFakeDataSource(config);

  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  ASSERT_TRUE(controller->StartDataSource(data_source.get()));

  // State clearing on tracing teardown.
  EXPECT_CALL(*controller->procfs(), WriteToFile(kFooEnablePath, "0"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(),
              WriteToFile("/root/buffer_size_kb", PageSizeKb()));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(*controller->procfs(), ClearFile("/root/trace"))
      .WillOnce(Return(true));
  EXPECT_CALL(*controller->procfs(),
              ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  controller.reset();
  data_source.reset();
}

TEST(FtraceControllerTest, BufferSize) {
  auto controller = CreateTestController(false /* nice procfs */);

  // For this test we don't care about most calls to WriteToFile/ClearFile.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), ClearFile(_)).Times(AnyNumber());

  // Every time a fake data source is destroyed, the controller will reset the
  // buffer size to a single page.
  EXPECT_CALL(*controller->procfs(),
              WriteToFile("/root/buffer_size_kb", PageSizeKb()))
      .Times(AnyNumber());

  {
    // No buffer size -> good default (exact value depends on the ram size of
    // the machine running this test).
    EXPECT_CALL(
        *controller->procfs(),
        WriteToFile("/root/buffer_size_kb", testing::AnyOf("2048", "8192")));
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    auto data_source = controller->AddFakeDataSource(config);
    ASSERT_TRUE(controller->StartDataSource(data_source.get()));
  }

  {
    // Your size ends up with less than 1 page per cpu -> 1 page (gmock already
    // covered by the cleanup expectation above).
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    config.set_buffer_size_kb(1);
    auto data_source = controller->AddFakeDataSource(config);
    ASSERT_TRUE(controller->StartDataSource(data_source.get()));
  }

  {
    // You picked a good size -> your size rounded to nearest page.
    EXPECT_CALL(*controller->procfs(),
                WriteToFile("/root/buffer_size_kb", "64"));
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    config.set_buffer_size_kb(65);
    auto data_source = controller->AddFakeDataSource(config);
    ASSERT_TRUE(controller->StartDataSource(data_source.get()));
  }

  {
    // You picked a good size -> your size rounded to nearest page.
    EXPECT_CALL(*controller->procfs(),
                WriteToFile("/root/buffer_size_kb", "64"));
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    ON_CALL(*controller->procfs(), NumberOfCpus()).WillByDefault(Return(2));
    config.set_buffer_size_kb(65);
    auto data_source = controller->AddFakeDataSource(config);
    ASSERT_TRUE(controller->StartDataSource(data_source.get()));
  }

  {
    // buffer_size_lower_bound -> default size no less than given.
    EXPECT_CALL(
        *controller->procfs(),
        WriteToFile("/root/buffer_size_kb", testing::AnyOf("4096", "8192")));
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    config.set_buffer_size_kb(4096);
    config.set_buffer_size_lower_bound(true);
    auto data_source = controller->AddFakeDataSource(config);
    ASSERT_TRUE(controller->StartDataSource(data_source.get()));
  }
}

TEST(FtraceControllerTest, PeriodicDrainConfig) {
  auto controller = CreateTestController(false /* nice procfs */);

  // For this test we don't care about calls to WriteToFile/ClearFile.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), ClearFile(_)).Times(AnyNumber());

  {
    // No period -> good default.
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    auto data_source = controller->AddFakeDataSource(config);
    controller->StartDataSource(data_source.get());
    EXPECT_EQ(100u, controller->tick_period_ms());
  }

  {
    // Pick a tiny value -> good default.
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    config.set_drain_period_ms(0);
    auto data_source = controller->AddFakeDataSource(config);
    controller->StartDataSource(data_source.get());
    EXPECT_EQ(100u, controller->tick_period_ms());
  }

  {
    // Pick a huge value -> good default.
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    config.set_drain_period_ms(1000 * 60 * 60);
    auto data_source = controller->AddFakeDataSource(config);
    controller->StartDataSource(data_source.get());
    EXPECT_EQ(100u, controller->tick_period_ms());
  }

  {
    // Pick a resonable value -> get that value.
    FtraceConfig config = CreateFtraceConfig({"group/foo"});
    config.set_drain_period_ms(200);
    auto data_source = controller->AddFakeDataSource(config);
    controller->StartDataSource(data_source.get());
    EXPECT_EQ(200u, controller->tick_period_ms());
  }
}

TEST(FtraceMetadataTest, Clear) {
  FtraceMetadata metadata;
  metadata.inode_and_device.insert(std::make_pair(1, 1));
  metadata.pids.insert(2);
  metadata.last_seen_device_id = 100;
  metadata.Clear();
  EXPECT_THAT(metadata.inode_and_device, IsEmpty());
  EXPECT_THAT(metadata.pids, IsEmpty());
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
              UnorderedElementsAre(Pair(2, 3), Pair(1, 3), Pair(3, 4)));
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

  protos::gen::TracePacket result_packet = writer->GetOnlyTracePacket();
  auto result = result_packet.ftrace_stats().cpu_stats()[0];
  EXPECT_EQ(result.cpu(), 0u);
  EXPECT_EQ(result.entries(), 1u);
  EXPECT_EQ(result.overrun(), 2u);
  auto kprobe_stats = result_packet.ftrace_stats().kprobe_stats();
  EXPECT_EQ(kprobe_stats.hits(), 0u);
  EXPECT_EQ(kprobe_stats.misses(), 0u);
}

TEST(FtraceStatsTest, WriteKprobeStats) {
  FtraceStats stats{};
  FtraceKprobeStats kprobe_stats{};
  kprobe_stats.hits = 1;
  kprobe_stats.misses = 2;
  stats.kprobe_stats = kprobe_stats;

  std::unique_ptr<TraceWriterForTesting> writer =
      std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
  {
    auto packet = writer->NewTracePacket();
    auto* out = packet->set_ftrace_stats();
    stats.Write(out);
  }

  protos::gen::TracePacket result_packet = writer->GetOnlyTracePacket();
  auto result = result_packet.ftrace_stats();
  EXPECT_EQ(result.kprobe_stats().hits(), 1u);
  EXPECT_EQ(result.kprobe_stats().misses(), 2u);
}

TEST(FtraceStatsTest, KprobeProfileParseEmpty) {
  std::string text = "";

  FtraceStats stats{};
  EXPECT_TRUE(DumpKprobeStats(text, &stats));
}

TEST(FtraceStatsTest, KprobeProfileParseEmptyLines) {
  std::string text = R"(

)";

  FtraceStats stats{};
  EXPECT_TRUE(DumpKprobeStats(text, &stats));
}

TEST(FtraceStatsTest, KprobeProfileParseValid) {
  std::string text = R"(  _binder_inner_proc_lock  1   8
  _binder_inner_proc_unlock                        2   9
  _binder_node_inner_unlock                        3  10
  _binder_node_unlock                              4  11
)";

  FtraceStats stats{};
  EXPECT_TRUE(DumpKprobeStats(text, &stats));

  EXPECT_EQ(stats.kprobe_stats.hits, 10u);
  EXPECT_EQ(stats.kprobe_stats.misses, 38u);
}

TEST(FtraceStatsTest, KprobeProfileMissingValuesParseInvalid) {
  std::string text = R"(  _binder_inner_proc_lock  1   8
  _binder_inner_proc_unlock                        2
)";

  FtraceStats stats{};
  EXPECT_FALSE(DumpKprobeStats(text, &stats));

  EXPECT_EQ(stats.kprobe_stats.hits, 0u);
  EXPECT_EQ(stats.kprobe_stats.misses, 0u);
}

TEST(FtraceControllerTest, OnlySecondaryInstance) {
  auto controller = CreateTestController(true /* nice procfs */);

  FtraceConfig config = CreateFtraceConfig({"group/foo"});
  config.set_instance_name("secondary");

  // Primary instance won't be touched throughout the entire test.
  // Exception: allow testing for kernel support of buffer_percent.
  EXPECT_CALL(*controller->procfs(), ClearFile(_)).Times(0);
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(0);
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_percent", _))
      .Times(AnyNumber())
      .WillRepeatedly(Return(true));

  // AddDataSource will initialise the tracefs instance, enable the event
  // through the muxer, but not yet enable tracing_on.
  auto secondary_procfs = std::unique_ptr<MockFtraceProcfs>(
      new NiceMock<MockFtraceProcfs>("/root/instances/secondary/", 1));
  EXPECT_CALL(*secondary_procfs, WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*secondary_procfs,
              WriteToFile("/root/instances/secondary/tracing_on", "0"));
  EXPECT_CALL(
      *secondary_procfs,
      WriteToFile("/root/instances/secondary/events/group/foo/enable", "1"));
  controller->PrepareMockProcfsForInstance("secondary",
                                           std::move(secondary_procfs));

  // No read tasks posted as part of adding the data source.
  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, _)).Times(0);

  std::unique_ptr<FtraceDataSource> data_source =
      controller->AddFakeDataSource(config);
  ASSERT_NE(nullptr, data_source);

  Mock::VerifyAndClearExpectations(
      controller->GetInstanceMockProcfs("secondary"));
  Mock::VerifyAndClearExpectations(controller->runner());

  // StartDataSource will simply enable the event and post a ReadTick.
  EXPECT_CALL(*controller->GetInstanceMockProcfs("secondary"),
              WriteToFile("/root/instances/secondary/tracing_on", "1"));
  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, _)).Times(1);

  ASSERT_TRUE(controller->StartDataSource(data_source.get()));

  Mock::VerifyAndClearExpectations(
      controller->GetInstanceMockProcfs("secondary"));
  Mock::VerifyAndClearExpectations(controller->runner());

  // RemoveDataSource will reset the tracefs instance.
  EXPECT_CALL(*controller->GetInstanceMockProcfs("secondary"),
              WriteToFile(_, _))
      .Times(AnyNumber());
  EXPECT_CALL(
      *controller->GetInstanceMockProcfs("secondary"),
      WriteToFile("/root/instances/secondary/events/group/foo/enable", "0"));
  EXPECT_CALL(
      *controller->GetInstanceMockProcfs("secondary"),
      WriteToFile("/root/instances/secondary/buffer_size_kb", PageSizeKb()));

  controller->RemoveDataSource(data_source.get());

  // Controller forgot about the instance.
  EXPECT_FALSE(controller->InstanceExists("secondary"));
}

TEST(FtraceControllerTest, DefaultAndSecondaryInstance) {
  auto controller = CreateTestController(true /* nice procfs */);

  FtraceConfig primary_cfg = CreateFtraceConfig({"group/foo"});
  FtraceConfig secondary_cfg = CreateFtraceConfig({"group/bar"});
  secondary_cfg.set_instance_name("secondary");

  // AddDataSource will initialise the tracefs instances, enable the events
  // through the muxers, but not yet enable tracing_on.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(*controller->procfs(),
              WriteToFile("/root/events/group/foo/enable", "1"));

  auto secondary_procfs = std::unique_ptr<MockFtraceProcfs>(
      new NiceMock<MockFtraceProcfs>("/root/instances/secondary/", 1));
  EXPECT_CALL(*secondary_procfs, WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*secondary_procfs,
              WriteToFile("/root/instances/secondary/tracing_on", "0"));
  EXPECT_CALL(
      *secondary_procfs,
      WriteToFile("/root/instances/secondary/events/group/bar/enable", "1"));
  controller->PrepareMockProcfsForInstance("secondary",
                                           std::move(secondary_procfs));

  // No read tasks posted as part of adding the data sources.
  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, _)).Times(0);

  std::unique_ptr<FtraceDataSource> primary_ds =
      controller->AddFakeDataSource(primary_cfg);
  std::unique_ptr<FtraceDataSource> secondary_ds =
      controller->AddFakeDataSource(secondary_cfg);
  ASSERT_NE(nullptr, primary_ds);
  ASSERT_NE(nullptr, secondary_ds);
  ASSERT_NE(primary_ds->config_id(), secondary_ds->config_id());

  Mock::VerifyAndClearExpectations(controller->procfs());
  Mock::VerifyAndClearExpectations(
      controller->GetInstanceMockProcfs("secondary"));
  Mock::VerifyAndClearExpectations(controller->runner());

  // StartDataSource will simply enable the events and post two ReadTicks (one
  // per instance having the first data source activated), with the first tick
  // becoming obsolete.
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(*controller->GetInstanceMockProcfs("secondary"),
              WriteToFile("/root/instances/secondary/tracing_on", "1"));
  EXPECT_CALL(*controller->procfs(), WriteToFile("/root/buffer_percent", _))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*controller->runner(), PostDelayedTask(_, _)).Times(2);

  ASSERT_TRUE(controller->StartDataSource(primary_ds.get()));
  ASSERT_TRUE(controller->StartDataSource(secondary_ds.get()));

  Mock::VerifyAndClearExpectations(controller->procfs());
  Mock::VerifyAndClearExpectations(
      controller->GetInstanceMockProcfs("secondary"));
  Mock::VerifyAndClearExpectations(controller->runner());

  // RemoveDataSource will reset the tracefs instances.
  EXPECT_CALL(*controller->procfs(), WriteToFile(_, _)).Times(AnyNumber());
  EXPECT_CALL(*controller->procfs(),
              WriteToFile("/root/events/group/foo/enable", "0"));

  EXPECT_CALL(*controller->GetInstanceMockProcfs("secondary"),
              WriteToFile(_, _))
      .Times(AnyNumber());
  EXPECT_CALL(
      *controller->GetInstanceMockProcfs("secondary"),
      WriteToFile("/root/instances/secondary/events/group/bar/enable", "0"));

  controller->RemoveDataSource(primary_ds.get());
  controller->RemoveDataSource(secondary_ds.get());

  // Controller forgot about the secondary instance.
  EXPECT_FALSE(controller->InstanceExists("secondary"));
}

TEST(FtraceControllerTest, TracefsInstanceFilepaths) {
  std::optional<std::string> path;
  path = FtraceController::AbsolutePathForInstance("/root/", "test");
  EXPECT_EQ(*path, "/root/instances/test/");

  // named directory should stay under instances/
  path = FtraceController::AbsolutePathForInstance("/root/", "test/test");
  EXPECT_FALSE(path.has_value());
  path = FtraceController::AbsolutePathForInstance("/root/", "..");
  EXPECT_FALSE(path.has_value());

  // special-cased pkvm path
  path = FtraceController::AbsolutePathForInstance("/root/", "hyp");
  EXPECT_EQ(*path, "/root/hyp/");
}

TEST(FtraceControllerTest, PollSupportedOnKernelVersion) {
  auto test = [](auto s) {
    return FtraceController::PollSupportedOnKernelVersion(s);
  };
  // Linux 6.9 or above are ok
  EXPECT_TRUE(test("6.9.13-1-amd64"));
  EXPECT_TRUE(test("6.9.0-1-amd64"));
  EXPECT_TRUE(test("6.9.25-android14-11-g"));
  // before 6.9
  EXPECT_FALSE(test("5.15.200-1-amd"));

  // Android: check allowlisted GKI versions

  // sublevel matters:
  EXPECT_TRUE(test("6.1.87-android14-4-0"));
  EXPECT_FALSE(test("6.1.80-android14-4-0"));
  // sublevel matters:
  EXPECT_TRUE(test("6.6.27-android15-8-suffix"));
  EXPECT_FALSE(test("6.6.26-android15-8-suffix"));
  // android13 instead of android14 (clarification: this is part of the kernel
  // version, and is unrelated to the system image version).
  EXPECT_FALSE(test("6.1.87-android13-4-0"));
}

}  // namespace perfetto
