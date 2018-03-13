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

#include "src/ftrace_reader/ftrace_config_muxer.h"

#include <memory>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "atrace_wrapper.h"
#include "ftrace_procfs.h"
#include "proto_translation_table.h"

using testing::_;
using testing::AnyNumber;
using testing::Contains;
using testing::ElementsAreArray;
using testing::Eq;
using testing::IsEmpty;
using testing::NiceMock;
using testing::Not;
using testing::Return;
using testing::UnorderedElementsAre;

namespace perfetto {
namespace {

class MockFtraceProcfs : public FtraceProcfs {
 public:
  MockFtraceProcfs() : FtraceProcfs("/root/") {
    ON_CALL(*this, NumberOfCpus()).WillByDefault(Return(1));
    ON_CALL(*this, WriteToFile(_, _)).WillByDefault(Return(true));
    ON_CALL(*this, ClearFile(_)).WillByDefault(Return(true));
    EXPECT_CALL(*this, NumberOfCpus()).Times(AnyNumber());
  }

  MOCK_METHOD2(WriteToFile,
               bool(const std::string& path, const std::string& str));
  MOCK_METHOD1(ReadOneCharFromFile, char(const std::string& path));
  MOCK_METHOD1(ClearFile, bool(const std::string& path));
  MOCK_CONST_METHOD1(ReadFileIntoString, std::string(const std::string& path));
  MOCK_CONST_METHOD0(NumberOfCpus, size_t());
};

struct MockRunAtrace {
  MockRunAtrace() {
    static MockRunAtrace* instance;
    instance = this;
    SetRunAtraceForTesting([](const std::vector<std::string>& args) {
      return instance->RunAtrace(args);
    });
  }

  ~MockRunAtrace() { SetRunAtraceForTesting(nullptr); }

  MOCK_METHOD1(RunAtrace, bool(const std::vector<std::string>&));
};

std::unique_ptr<ProtoTranslationTable> CreateFakeTable() {
  std::vector<Field> common_fields;
  std::vector<Event> events;

  {
    Event event;
    event.name = "sched_switch";
    event.group = "sched";
    event.ftrace_event_id = 1;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "sched_wakeup";
    event.group = "sched";
    event.ftrace_event_id = 10;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "sched_new";
    event.group = "sched";
    event.ftrace_event_id = 20;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "print";
    event.group = "ftrace";
    event.ftrace_event_id = 20;
    events.push_back(event);
  }

  return std::unique_ptr<ProtoTranslationTable>(
      new ProtoTranslationTable(events, std::move(common_fields)));
}

TEST(FtraceConfigMuxerTest, ComputeCpuBufferSizeInPages) {
  // No buffer size given: good default (128 pages = 512kb).
  EXPECT_EQ(ComputeCpuBufferSizeInPages(0), 128u);
  // Buffer size given way too big: good default.
  EXPECT_EQ(ComputeCpuBufferSizeInPages(10 * 1024 * 1024), 128u);
  // The limit is 2mb per CPU, 3mb is too much.
  EXPECT_EQ(ComputeCpuBufferSizeInPages(3 * 1024), 128u);
  // Your size ends up with less than 1 page per cpu -> 1 page.
  EXPECT_EQ(ComputeCpuBufferSizeInPages(3), 1u);
  // You picked a good size -> your size rounded to nearest page.
  EXPECT_EQ(ComputeCpuBufferSizeInPages(42), 10u);
}

TEST(FtraceConfigMuxerTest, TurnFtraceOnOff) {
  std::unique_ptr<ProtoTranslationTable> table = CreateFakeTable();
  MockFtraceProcfs ftrace;

  FtraceConfig config = CreateFtraceConfig({"sched_switch", "foo"});

  FtraceConfigMuxer model(&ftrace, table.get());

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());

  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('0'));
  EXPECT_CALL(ftrace, WriteToFile("/root/buffer_size_kb", "512"));
  EXPECT_CALL(ftrace, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/sched/sched_switch/enable", "1"));
  FtraceConfigId id = model.RequestConfig(config);
  ASSERT_TRUE(id);

  const FtraceConfig* actual_config = model.GetConfig(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("sched_switch"));
  EXPECT_THAT(actual_config->ftrace_events(), Not(Contains("foo")));

  EXPECT_CALL(ftrace, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace, WriteToFile("/root/buffer_size_kb", "0"));
  EXPECT_CALL(ftrace, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/sched/sched_switch/enable", "0"));
  EXPECT_CALL(ftrace, ClearFile("/root/trace"));
  ASSERT_TRUE(model.RemoveConfig(id));
}

TEST(FtraceConfigMuxerTest, FtraceIsAlreadyOn) {
  std::unique_ptr<ProtoTranslationTable> table = CreateFakeTable();
  MockFtraceProcfs ftrace;

  FtraceConfig config = CreateFtraceConfig({"sched_switch"});

  FtraceConfigMuxer model(&ftrace, table.get());

  // If someone is using ftrace already don't stomp on what they are doing.
  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));
  FtraceConfigId id = model.RequestConfig(config);
  ASSERT_FALSE(id);
}

TEST(FtraceConfigMuxerTest, Atrace) {
  std::unique_ptr<ProtoTranslationTable> table = CreateFakeTable();
  NiceMock<MockFtraceProcfs> ftrace;
  MockRunAtrace atrace;

  FtraceConfig config = CreateFtraceConfig({"sched_switch"});
  *config.add_atrace_categories() = "sched";

  FtraceConfigMuxer model(&ftrace, table.get());

  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('0'));
  EXPECT_CALL(atrace,
              RunAtrace(ElementsAreArray({"atrace", "--async_start", "sched"})))
      .WillOnce(Return(true));

  FtraceConfigId id = model.RequestConfig(config);
  ASSERT_TRUE(id);

  const FtraceConfig* actual_config = model.GetConfig(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("sched_switch"));
  EXPECT_THAT(actual_config->ftrace_events(), Contains("print"));

  EXPECT_CALL(atrace, RunAtrace(ElementsAreArray({"atrace", "--async_stop"})))
      .WillOnce(Return(true));
  ASSERT_TRUE(model.RemoveConfig(id));
}

TEST(FtraceConfigMuxerTest, SetupClockForTesting) {
  std::unique_ptr<ProtoTranslationTable> table = CreateFakeTable();
  MockFtraceProcfs ftrace;
  FtraceConfig config;

  FtraceConfigMuxer model(&ftrace, table.get());

  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace, WriteToFile("/root/trace_clock", "boot"));
  model.SetupClockForTesting(config);

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global"));
  EXPECT_CALL(ftrace, WriteToFile("/root/trace_clock", "global"));
  model.SetupClockForTesting(config);

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return(""));
  model.SetupClockForTesting(config);

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("local [global]"));
  model.SetupClockForTesting(config);
}

TEST(FtraceConfigMuxerTest, GetFtraceEvents) {
  FtraceConfig config = CreateFtraceConfig({"sched_switch"});
  std::set<std::string> events = GetFtraceEvents(config);

  EXPECT_THAT(events, Contains("sched_switch"));
  EXPECT_THAT(events, Not(Contains("print")));
}

TEST(FtraceConfigMuxerTest, GetFtraceEventsAtrace) {
  FtraceConfig config = CreateFtraceConfig({"sched_switch"});
  *config.add_atrace_categories() = "sched";
  std::set<std::string> events = GetFtraceEvents(config);

  EXPECT_THAT(events, Contains("sched_switch"));
  EXPECT_THAT(events, Contains("print"));
}

}  // namespace
}  // namespace perfetto
