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

#include "src/traced/probes/ftrace/ftrace_config_muxer.h"

#include <memory>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/traced/probes/ftrace/atrace_wrapper.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"

using testing::_;
using testing::AnyNumber;
using testing::MatchesRegex;
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
  MOCK_METHOD2(AppendToFile,
               bool(const std::string& path, const std::string& str));
  MOCK_METHOD1(ReadOneCharFromFile, char(const std::string& path));
  MOCK_METHOD1(ClearFile, bool(const std::string& path));
  MOCK_CONST_METHOD1(ReadFileIntoString, std::string(const std::string& path));
  MOCK_CONST_METHOD0(NumberOfCpus, size_t());
  MOCK_CONST_METHOD1(GetEventNamesForGroup,
                     const std::set<std::string>(const std::string& path));
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

class MockProtoTranslationTable : public ProtoTranslationTable {
 public:
  MockProtoTranslationTable(NiceMock<MockFtraceProcfs>* ftrace_procfs,
                            const std::vector<Event>& events,
                            std::vector<Field> common_fields,
                            FtracePageHeaderSpec ftrace_page_header_spec)
      : ProtoTranslationTable(ftrace_procfs,
                              events,
                              common_fields,
                              ftrace_page_header_spec) {}
  MOCK_METHOD1(GetOrCreateEvent, Event*(const GroupAndName& group_and_name));
  MOCK_CONST_METHOD1(GetEvent,
                     const Event*(const GroupAndName& group_and_name));
};

class FtraceConfigMuxerTest : public ::testing::Test {
 protected:
  std::unique_ptr<MockProtoTranslationTable> GetMockTable() {
    std::vector<Field> common_fields;
    std::vector<Event> events;
    return std::unique_ptr<MockProtoTranslationTable>(
        new MockProtoTranslationTable(
            &table_procfs_, events, std::move(common_fields),
            ProtoTranslationTable::DefaultPageHeaderSpecForTesting()));
  }
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
      event.ftrace_event_id = 11;
      events.push_back(event);
    }

    {
      Event event;
      event.name = "cgroup_mkdir";
      event.group = "cgroup";
      event.ftrace_event_id = 12;
      events.push_back(event);
    }

    {
      Event event;
      event.name = "mm_vmscan_direct_reclaim_begin";
      event.group = "vmscan";
      event.ftrace_event_id = 13;
      events.push_back(event);
    }

    {
      Event event;
      event.name = "lowmemory_kill";
      event.group = "lowmemorykiller";
      event.ftrace_event_id = 14;
      events.push_back(event);
    }

    {
      Event event;
      event.name = "print";
      event.group = "ftrace";
      event.ftrace_event_id = 20;
      events.push_back(event);
    }

    return std::unique_ptr<ProtoTranslationTable>(new ProtoTranslationTable(
        &table_procfs_, events, std::move(common_fields),
        ProtoTranslationTable::DefaultPageHeaderSpecForTesting()));
  }

  NiceMock<MockFtraceProcfs> table_procfs_;
  std::unique_ptr<ProtoTranslationTable> table_ = CreateFakeTable();
};

TEST_F(FtraceConfigMuxerTest, ComputeCpuBufferSizeInPages) {
  static constexpr size_t kMaxBufSizeInPages = 16 * 1024u;
  // No buffer size given: good default (128 pages = 2mb).
  EXPECT_EQ(ComputeCpuBufferSizeInPages(0), 512u);
  // Buffer size given way too big: good default.
  EXPECT_EQ(ComputeCpuBufferSizeInPages(10 * 1024 * 1024), kMaxBufSizeInPages);
  // The limit is 64mb per CPU, 512mb is too much.
  EXPECT_EQ(ComputeCpuBufferSizeInPages(512 * 1024), kMaxBufSizeInPages);
  // Your size ends up with less than 1 page per cpu -> 1 page.
  EXPECT_EQ(ComputeCpuBufferSizeInPages(3), 1u);
  // You picked a good size -> your size rounded to nearest page.
  EXPECT_EQ(ComputeCpuBufferSizeInPages(42), 10u);
}

TEST_F(FtraceConfigMuxerTest, AddGenericEvent) {
  auto mock_table = GetMockTable();
  MockFtraceProcfs ftrace;

  FtraceConfig config = CreateFtraceConfig({"power/cpu_frequency"});

  FtraceConfigMuxer model(&ftrace, mock_table.get());

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());

  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .Times(2)
      .WillRepeatedly(Return('0'));
  EXPECT_CALL(ftrace, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(ftrace, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/power/cpu_frequency/enable", "1"));
  EXPECT_CALL(*mock_table, GetEvent(GroupAndName("power", "cpu_frequency")))
      .Times(AnyNumber());

  Event event_to_return;
  event_to_return.name = "cpu_frequency";
  event_to_return.group = "power";
  event_to_return.ftrace_event_id = 1;
  ON_CALL(*mock_table, GetOrCreateEvent(GroupAndName("power", "cpu_frequency")))
      .WillByDefault(Return(&event_to_return));
  EXPECT_CALL(*mock_table,
              GetOrCreateEvent(GroupAndName("power", "cpu_frequency")));

  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_TRUE(model.ActivateConfig(id));
  const FtraceConfig* actual_config = model.GetConfigForTesting(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("power/cpu_frequency"));
}

TEST_F(FtraceConfigMuxerTest, AddSameNameEvents) {
  auto mock_table = GetMockTable();
  NiceMock<MockFtraceProcfs> ftrace;

  FtraceConfig config = CreateFtraceConfig({"group_one/foo", "group_two/foo"});

  FtraceConfigMuxer model(&ftrace, mock_table.get());

  Event event1;
  event1.name = "foo";
  event1.group = "group_one";
  event1.ftrace_event_id = 1;
  ON_CALL(*mock_table, GetOrCreateEvent(GroupAndName("group_one", "foo")))
      .WillByDefault(Return(&event1));
  EXPECT_CALL(*mock_table, GetOrCreateEvent(GroupAndName("group_one", "foo")));

  Event event2;
  event2.name = "foo";
  event2.group = "group_two";
  event2.ftrace_event_id = 2;
  ON_CALL(*mock_table, GetOrCreateEvent(GroupAndName("group_two", "foo")))
      .WillByDefault(Return(&event2));
  EXPECT_CALL(*mock_table, GetOrCreateEvent(GroupAndName("group_two", "foo")));

  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_TRUE(model.ActivateConfig(id));
  const FtraceConfig* actual_config = model.GetConfigForTesting(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("group_one/foo"));
  EXPECT_THAT(actual_config->ftrace_events(), Contains("group_two/foo"));
}

TEST_F(FtraceConfigMuxerTest, AddAllEvents) {
  auto mock_table = GetMockTable();
  MockFtraceProcfs ftrace;

  FtraceConfig config = CreateFtraceConfig({"sched/*"});

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());

  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .Times(2)
      .WillRepeatedly(Return('0'));
  EXPECT_CALL(ftrace, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(ftrace, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/sched/sched_switch/enable", "1"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/sched/sched_new_event/enable", "1"));

  FtraceConfigMuxer model(&ftrace, mock_table.get());
  std::set<std::string> n = {"sched_switch", "sched_new_event"};
  ON_CALL(ftrace, GetEventNamesForGroup("events/sched"))
      .WillByDefault(Return(n));
  EXPECT_CALL(ftrace, GetEventNamesForGroup("events/sched")).Times(1);

  // Non-generic event.
  std::map<std::string, const Event*> events;
  Event sched_switch = {"sched_switch", "sched"};
  sched_switch.ftrace_event_id = 1;
  ON_CALL(*mock_table, GetOrCreateEvent(GroupAndName("sched", "sched_switch")))
      .WillByDefault(Return(&sched_switch));
  EXPECT_CALL(*mock_table,
              GetOrCreateEvent(GroupAndName("sched", "sched_switch")))
      .Times(AnyNumber());

  // Generic event.
  Event event_to_return;
  event_to_return.name = "sched_new_event";
  event_to_return.group = "sched";
  event_to_return.ftrace_event_id = 2;
  ON_CALL(*mock_table,
          GetOrCreateEvent(GroupAndName("sched", "sched_new_event")))
      .WillByDefault(Return(&event_to_return));
  EXPECT_CALL(*mock_table,
              GetOrCreateEvent(GroupAndName("sched", "sched_new_event")));

  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_TRUE(id);
  ASSERT_TRUE(model.ActivateConfig(id));

  const FtraceConfig* actual_config = model.GetConfigForTesting(id);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("sched/sched_switch"));
  EXPECT_THAT(actual_config->ftrace_events(),
              Contains("sched/sched_new_event"));
}

TEST_F(FtraceConfigMuxerTest, TwoWildcardGroups) {
  auto mock_table = GetMockTable();
  NiceMock<MockFtraceProcfs> ftrace;

  FtraceConfig config = CreateFtraceConfig({"group_one/*", "group_two/*"});

  FtraceConfigMuxer model(&ftrace, mock_table.get());

  std::set<std::string> event_names = {"foo"};
  ON_CALL(ftrace, GetEventNamesForGroup("events/group_one"))
      .WillByDefault(Return(event_names));
  EXPECT_CALL(ftrace, GetEventNamesForGroup("events/group_one"))
      .Times(AnyNumber());

  ON_CALL(ftrace, GetEventNamesForGroup("events/group_two"))
      .WillByDefault(Return(event_names));
  EXPECT_CALL(ftrace, GetEventNamesForGroup("events/group_two"))
      .Times(AnyNumber());

  Event event1;
  event1.name = "foo";
  event1.group = "group_one";
  event1.ftrace_event_id = 1;
  ON_CALL(*mock_table, GetOrCreateEvent(GroupAndName("group_one", "foo")))
      .WillByDefault(Return(&event1));
  EXPECT_CALL(*mock_table, GetOrCreateEvent(GroupAndName("group_one", "foo")));

  Event event2;
  event2.name = "foo";
  event2.group = "group_two";
  event2.ftrace_event_id = 2;
  ON_CALL(*mock_table, GetOrCreateEvent(GroupAndName("group_two", "foo")))
      .WillByDefault(Return(&event2));
  EXPECT_CALL(*mock_table, GetOrCreateEvent(GroupAndName("group_two", "foo")));

  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_TRUE(model.ActivateConfig(id));
  const FtraceConfig* actual_config = model.GetConfigForTesting(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("group_one/foo"));
  EXPECT_THAT(actual_config->ftrace_events(), Contains("group_two/foo"));
}

TEST_F(FtraceConfigMuxerTest, TurnFtraceOnOff) {
  MockFtraceProcfs ftrace;

  FtraceConfig config = CreateFtraceConfig({"sched_switch", "foo"});

  FtraceConfigMuxer model(&ftrace, table_.get());

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());

  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .Times(2)
      .WillRepeatedly(Return('0'));
  EXPECT_CALL(ftrace, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(ftrace, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/sched/sched_switch/enable", "1"));
  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_TRUE(id);
  ASSERT_TRUE(model.ActivateConfig(id));

  const FtraceConfig* actual_config = model.GetConfigForTesting(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("sched/sched_switch"));
  EXPECT_THAT(actual_config->ftrace_events(), Not(Contains("foo")));

  EXPECT_CALL(ftrace, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace, WriteToFile("/root/buffer_size_kb", "0"));
  EXPECT_CALL(ftrace, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/sched/sched_switch/enable", "0"));
  EXPECT_CALL(ftrace, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  ASSERT_TRUE(model.RemoveConfig(id));
}

TEST_F(FtraceConfigMuxerTest, FtraceIsAlreadyOn) {
  MockFtraceProcfs ftrace;

  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});

  FtraceConfigMuxer model(&ftrace, table_.get());

  // If someone is using ftrace already don't stomp on what they are doing.
  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));
  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_FALSE(id);
}

TEST_F(FtraceConfigMuxerTest, Atrace) {
  NiceMock<MockFtraceProcfs> ftrace;
  MockRunAtrace atrace;

  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});
  *config.add_atrace_categories() = "sched";

  FtraceConfigMuxer model(&ftrace, table_.get());

  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('0'));
  EXPECT_CALL(atrace,
              RunAtrace(ElementsAreArray(
                  {"atrace", "--async_start", "--only_userspace", "sched"})))
      .WillOnce(Return(true));

  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_TRUE(id);

  const FtraceConfig* actual_config = model.GetConfigForTesting(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("sched/sched_switch"));
  EXPECT_THAT(actual_config->ftrace_events(), Contains("ftrace/print"));

  EXPECT_CALL(atrace, RunAtrace(ElementsAreArray(
                          {"atrace", "--async_stop", "--only_userspace"})))
      .WillOnce(Return(true));
  ASSERT_TRUE(model.RemoveConfig(id));
}

TEST_F(FtraceConfigMuxerTest, AtraceTwoApps) {
  NiceMock<MockFtraceProcfs> ftrace;
  MockRunAtrace atrace;

  FtraceConfig config = CreateFtraceConfig({});
  *config.add_atrace_apps() = "com.google.android.gms.persistent";
  *config.add_atrace_apps() = "com.google.android.gms";

  FtraceConfigMuxer model(&ftrace, table_.get());

  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('0'));
  EXPECT_CALL(
      atrace,
      RunAtrace(ElementsAreArray(
          {"atrace", "--async_start", "--only_userspace", "-a",
           "com.google.android.gms.persistent,com.google.android.gms"})))
      .WillOnce(Return(true));

  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_TRUE(id);

  const FtraceConfig* actual_config = model.GetConfigForTesting(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("ftrace/print"));

  EXPECT_CALL(atrace, RunAtrace(ElementsAreArray(
                          {"atrace", "--async_stop", "--only_userspace"})))
      .WillOnce(Return(true));
  ASSERT_TRUE(model.RemoveConfig(id));
}

TEST_F(FtraceConfigMuxerTest, SetupClockForTesting) {
  MockFtraceProcfs ftrace;
  FtraceConfig config;

  FtraceConfigMuxer model(&ftrace, table_.get());

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

TEST_F(FtraceConfigMuxerTest, GetFtraceEvents) {
  MockFtraceProcfs ftrace;
  FtraceConfigMuxer model(&ftrace, table_.get());

  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});
  std::set<GroupAndName> events =
      model.GetFtraceEventsForTesting(config, table_.get());

  EXPECT_THAT(events, Contains(GroupAndName("sched", "sched_switch")));
  EXPECT_THAT(events, Not(Contains(GroupAndName("ftrace", "print"))));
}

TEST_F(FtraceConfigMuxerTest, GetFtraceEventsAtrace) {
  MockFtraceProcfs ftrace;
  FtraceConfigMuxer model(&ftrace, table_.get());

  FtraceConfig config = CreateFtraceConfig({});
  *config.add_atrace_categories() = "sched";
  std::set<GroupAndName> events =
      model.GetFtraceEventsForTesting(config, table_.get());

  EXPECT_THAT(events, Contains(GroupAndName("sched", "sched_switch")));
  EXPECT_THAT(events, Contains(GroupAndName("sched", "sched_cpu_hotplug")));
  EXPECT_THAT(events, Contains(GroupAndName("ftrace", "print")));
}

TEST_F(FtraceConfigMuxerTest, GetFtraceEventsAtraceCategories) {
  MockFtraceProcfs ftrace;
  FtraceConfigMuxer model(&ftrace, table_.get());

  FtraceConfig config = CreateFtraceConfig({});
  *config.add_atrace_categories() = "sched";
  *config.add_atrace_categories() = "memreclaim";
  std::set<GroupAndName> events =
      model.GetFtraceEventsForTesting(config, table_.get());

  EXPECT_THAT(events, Contains(GroupAndName("sched", "sched_switch")));
  EXPECT_THAT(events, Contains(GroupAndName("sched", "sched_cpu_hotplug")));
  EXPECT_THAT(events, Contains(GroupAndName("cgroup", "cgroup_mkdir")));
  EXPECT_THAT(events, Contains(GroupAndName("vmscan",
                                            "mm_vmscan_direct_reclaim_begin")));
  EXPECT_THAT(events,
              Contains(GroupAndName("lowmemorykiller", "lowmemory_kill")));
  EXPECT_THAT(events, Contains(GroupAndName("ftrace", "print")));
}

// Tests the enabling fallback logic that tries to use the "set_event" interface
// if writing the individual xxx/enable file fails.
TEST_F(FtraceConfigMuxerTest, FallbackOnSetEvent) {
  MockFtraceProcfs ftrace;
  FtraceConfig config =
      CreateFtraceConfig({"sched/sched_switch", "cgroup/cgroup_mkdir"});
  FtraceConfigMuxer model(&ftrace, table_.get());

  ON_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());

  EXPECT_CALL(ftrace, ReadOneCharFromFile("/root/tracing_on"))
      .Times(2)
      .WillRepeatedly(Return('0'));
  EXPECT_CALL(ftrace, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(ftrace, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace, WriteToFile("/root/tracing_on", "1"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/sched/sched_switch/enable", "1"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/cgroup/cgroup_mkdir/enable", "1"))
      .WillOnce(Return(false));
  EXPECT_CALL(ftrace, AppendToFile("/root/set_event", "cgroup:cgroup_mkdir"))
      .WillOnce(Return(true));
  FtraceConfigId id = model.SetupConfig(config);
  ASSERT_TRUE(id);
  ASSERT_TRUE(model.ActivateConfig(id));

  const FtraceConfig* actual_config = model.GetConfigForTesting(id);
  EXPECT_TRUE(actual_config);
  EXPECT_THAT(actual_config->ftrace_events(), Contains("sched/sched_switch"));
  EXPECT_THAT(actual_config->ftrace_events(), Contains("cgroup/cgroup_mkdir"));

  EXPECT_CALL(ftrace, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace, WriteToFile("/root/buffer_size_kb", "0"));
  EXPECT_CALL(ftrace, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/sched/sched_switch/enable", "0"));
  EXPECT_CALL(ftrace,
              WriteToFile("/root/events/cgroup/cgroup_mkdir/enable", "0"))
      .WillOnce(Return(false));
  EXPECT_CALL(ftrace, AppendToFile("/root/set_event", "!cgroup:cgroup_mkdir"))
      .WillOnce(Return(true));
  EXPECT_CALL(ftrace, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  ASSERT_TRUE(model.RemoveConfig(id));
}

}  // namespace
}  // namespace perfetto
