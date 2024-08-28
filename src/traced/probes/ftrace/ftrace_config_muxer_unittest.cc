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

#include "ftrace_config_muxer.h"
#include "perfetto/ext/base/utils.h"
#include "src/traced/probes/ftrace/atrace_wrapper.h"
#include "src/traced/probes/ftrace/compact_sched.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/ftrace/ftrace_stats.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"
#include "test/gtest_and_gmock.h"

using testing::_;
using testing::AnyNumber;
using testing::Contains;
using testing::ElementsAreArray;
using testing::Eq;
using testing::Invoke;
using testing::IsEmpty;
using testing::MatchesRegex;
using testing::NiceMock;
using testing::Not;
using testing::Return;
using testing::UnorderedElementsAre;

namespace perfetto {
namespace {

constexpr int kFakeSchedSwitchEventId = 1;
constexpr int kCgroupMkdirEventId = 12;
constexpr int kFakePrintEventId = 20;
constexpr int kSysEnterId = 329;

struct FakeSyscallTable {
  static constexpr char names[] =
      "sys_open\0"
      "sys_read\0";
  static constexpr SyscallTable::OffT offsets[]{0, 9};
};

std::string PageSizeKb() {
  return std::to_string(base::GetSysPageSize() / 1024);
}

class MockFtraceProcfs : public FtraceProcfs {
 public:
  MockFtraceProcfs() : FtraceProcfs("/root/") {
    ON_CALL(*this, NumberOfCpus()).WillByDefault(Return(1));
    ON_CALL(*this, WriteToFile(_, _)).WillByDefault(Return(true));
    ON_CALL(*this, ClearFile(_)).WillByDefault(Return(true));
    EXPECT_CALL(*this, NumberOfCpus()).Times(AnyNumber());
  }

  MOCK_METHOD(bool,
              WriteToFile,
              (const std::string& path, const std::string& str),
              (override));
  MOCK_METHOD(bool,
              AppendToFile,
              (const std::string& path, const std::string& str),
              (override));
  MOCK_METHOD(char, ReadOneCharFromFile, (const std::string& path), (override));
  MOCK_METHOD(bool, ClearFile, (const std::string& path), (override));
  MOCK_METHOD(std::string,
              ReadFileIntoString,
              (const std::string& path),
              (const, override));
  MOCK_METHOD(size_t, NumberOfCpus, (), (const, override));
  MOCK_METHOD(const std::set<std::string>,
              GetEventNamesForGroup,
              (const std::string& path),
              (const, override));
  MOCK_METHOD(std::string,
              ReadEventFormat,
              (const std::string& group, const std::string& name),
              (const, override));
};

class MockAtraceWrapper : public AtraceWrapper {
 public:
  MOCK_METHOD(bool, RunAtrace, (const std::vector<std::string>&, std::string*));
  MOCK_METHOD(bool, SupportsUserspaceOnly, ());
  MOCK_METHOD(bool, SupportsPreferSdk, ());
};

class MockProtoTranslationTable : public ProtoTranslationTable {
 public:
  MockProtoTranslationTable(NiceMock<MockFtraceProcfs>* ftrace_procfs,
                            const std::vector<Event>& events,
                            std::vector<Field> common_fields,
                            FtracePageHeaderSpec ftrace_page_header_spec,
                            CompactSchedEventFormat compact_sched_format)
      : ProtoTranslationTable(ftrace_procfs,
                              events,
                              common_fields,
                              ftrace_page_header_spec,
                              compact_sched_format,
                              PrintkMap()) {}
  MOCK_METHOD(Event*,
              GetOrCreateEvent,
              (const GroupAndName& group_and_name),
              (override));
  MOCK_METHOD(const Event*,
              GetEvent,
              (const GroupAndName& group_and_name),
              (const, override));
};

TEST(ComputeCpuBufferSizeInPagesTest, DifferentCases) {
  constexpr auto test = ComputeCpuBufferSizeInPages;
  auto KbToPages = [](uint64_t kb) {
    return kb * 1024 / base::GetSysPageSize();
  };
  int64_t kNoRamInfo = 0;
  bool kExactSize = false;
  bool kLowerBoundSize = true;
  int64_t kLowRamPages =
      static_cast<int64_t>(KbToPages(3 * (1ULL << 20) + 512 * (1ULL << 10)));
  int64_t kHighRamPages =
      static_cast<int64_t>(KbToPages(7 * (1ULL << 20) + 512 * (1ULL << 10)));

  // No buffer size given: good default.
  EXPECT_EQ(test(0, kExactSize, kNoRamInfo), KbToPages(2048));
  // Default depends on device ram size.
  EXPECT_EQ(test(0, kExactSize, kLowRamPages), KbToPages(2048));
  EXPECT_EQ(test(0, kExactSize, kHighRamPages), KbToPages(8192));

  // buffer_size_lower_bound lets us choose a higher default than given.
  // default > requested:
  EXPECT_EQ(test(4096, kExactSize, kHighRamPages), KbToPages(4096));
  EXPECT_EQ(test(4096, kLowerBoundSize, kHighRamPages), KbToPages(8192));
  // requested > default:
  EXPECT_EQ(test(4096, kExactSize, kLowRamPages), KbToPages(4096));
  EXPECT_EQ(test(4096, kLowerBoundSize, kLowRamPages), KbToPages(4096));
  // requested > default:
  EXPECT_EQ(test(16384, kExactSize, kHighRamPages), KbToPages(16384));
  EXPECT_EQ(test(16384, kLowerBoundSize, kHighRamPages), KbToPages(16384));

  // Your size ends up with less than 1 page per cpu -> 1 page.
  EXPECT_EQ(test(3, kExactSize, kNoRamInfo), 1u);
  // You picked a good size -> your size rounded to nearest page.
  EXPECT_EQ(test(42, kExactSize, kNoRamInfo), KbToPages(42));

  // Sysconf returning an error is ok.
  EXPECT_EQ(test(0, kExactSize, -1), KbToPages(2048));
  EXPECT_EQ(test(4096, kExactSize, -1), KbToPages(4096));
}

// Base fixture that provides some dependencies but doesn't construct a
// FtraceConfigMuxer.
class FtraceConfigMuxerTest : public ::testing::Test {
 protected:
  FtraceConfigMuxerTest() {
    ON_CALL(atrace_wrapper_, RunAtrace).WillByDefault(Return(true));
    ON_CALL(atrace_wrapper_, SupportsUserspaceOnly).WillByDefault(Return(true));
    ON_CALL(atrace_wrapper_, SupportsPreferSdk).WillByDefault(Return(true));
  }

  std::unique_ptr<MockProtoTranslationTable> GetMockTable() {
    std::vector<Field> common_fields;
    std::vector<Event> events;
    return std::unique_ptr<MockProtoTranslationTable>(
        new MockProtoTranslationTable(
            &ftrace_, events, std::move(common_fields),
            ProtoTranslationTable::DefaultPageHeaderSpecForTesting(),
            InvalidCompactSchedEventFormatForTesting()));
  }

  SyscallTable GetSyscallTable() {
    return SyscallTable::Load<FakeSyscallTable>();
  }

  std::unique_ptr<ProtoTranslationTable> CreateFakeTable(
      CompactSchedEventFormat compact_format =
          InvalidCompactSchedEventFormatForTesting()) {
    std::vector<Field> common_fields;
    std::vector<Event> events;
    {
      Event event = {};
      event.name = "sched_switch";
      event.group = "sched";
      event.ftrace_event_id = kFakeSchedSwitchEventId;
      events.push_back(event);
    }

    {
      Event event = {};
      event.name = "sched_wakeup";
      event.group = "sched";
      event.ftrace_event_id = 10;
      events.push_back(event);
    }

    {
      Event event = {};
      event.name = "sched_new";
      event.group = "sched";
      event.ftrace_event_id = 11;
      events.push_back(event);
    }

    {
      Event event = {};
      event.name = "cgroup_mkdir";
      event.group = "cgroup";
      event.ftrace_event_id = kCgroupMkdirEventId;
      events.push_back(event);
    }

    {
      Event event = {};
      event.name = "mm_vmscan_direct_reclaim_begin";
      event.group = "vmscan";
      event.ftrace_event_id = 13;
      events.push_back(event);
    }

    {
      Event event = {};
      event.name = "lowmemory_kill";
      event.group = "lowmemorykiller";
      event.ftrace_event_id = 14;
      events.push_back(event);
    }

    {
      Event event = {};
      event.name = "print";
      event.group = "ftrace";
      event.ftrace_event_id = kFakePrintEventId;
      events.push_back(event);
    }

    {
      Event event = {};
      event.name = "sys_enter";
      event.group = "raw_syscalls";
      event.ftrace_event_id = kSysEnterId;
      events.push_back(event);
    }

    return std::unique_ptr<ProtoTranslationTable>(new ProtoTranslationTable(
        &ftrace_, events, std::move(common_fields),
        ProtoTranslationTable::DefaultPageHeaderSpecForTesting(),
        compact_format, PrintkMap()));
  }

  NiceMock<MockFtraceProcfs> ftrace_;
  NiceMock<MockAtraceWrapper> atrace_wrapper_;
};

TEST_F(FtraceConfigMuxerTest, SecondaryInstanceDoNotSupportAtrace) {
  auto fake_table = CreateFakeTable();
  FtraceConfigMuxer model(&ftrace_, &atrace_wrapper_, fake_table.get(),
                          GetSyscallTable(), {},
                          /* secondary_instance= */ true);

  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});
  *config.add_atrace_categories() = "sched";

  ASSERT_FALSE(model.SetupConfig(/* id= */ 73, config));
}

TEST_F(FtraceConfigMuxerTest, CompactSchedConfig) {
  // Set scheduling event format as validated. The pre-parsed format itself
  // doesn't need to be sensible, as the tests won't use it.
  auto format_with_id = CompactSchedSwitchFormat{};
  format_with_id.event_id = kFakeSchedSwitchEventId;
  auto valid_compact_format = CompactSchedEventFormat{
      /*format_valid=*/true, format_with_id, CompactSchedWakingFormat{}};

  std::unique_ptr<ProtoTranslationTable> table =
      CreateFakeTable(valid_compact_format);
  FtraceConfigMuxer muxer(&ftrace_, &atrace_wrapper_, table.get(),
                          GetSyscallTable(), {});

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));

  {
    // Explicitly enabled.
    FtraceConfig cfg = CreateFtraceConfig({"sched/sched_switch"});
    cfg.mutable_compact_sched()->set_enabled(true);

    FtraceConfigId id = 42;
    ASSERT_TRUE(muxer.SetupConfig(id, cfg));
    const FtraceDataSourceConfig* ds_config = muxer.GetDataSourceConfig(id);
    ASSERT_TRUE(ds_config);
    EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
                Contains(kFakeSchedSwitchEventId));
    EXPECT_TRUE(ds_config->compact_sched.enabled);
  }
  {
    // Implicitly enabled (default).
    FtraceConfig cfg = CreateFtraceConfig({"sched/sched_switch"});

    FtraceConfigId id = 43;
    ASSERT_TRUE(muxer.SetupConfig(id, cfg));
    const FtraceDataSourceConfig* ds_config = muxer.GetDataSourceConfig(id);
    ASSERT_TRUE(ds_config);
    EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
                Contains(kFakeSchedSwitchEventId));
    EXPECT_TRUE(ds_config->compact_sched.enabled);
  }
  {
    // Explicitly disabled.
    FtraceConfig cfg = CreateFtraceConfig({"sched/sched_switch"});
    cfg.mutable_compact_sched()->set_enabled(false);

    FtraceConfigId id = 44;
    ASSERT_TRUE(muxer.SetupConfig(id, cfg));
    const FtraceDataSourceConfig* ds_config = muxer.GetDataSourceConfig(id);
    ASSERT_TRUE(ds_config);
    EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
                Contains(kFakeSchedSwitchEventId));
    EXPECT_FALSE(ds_config->compact_sched.enabled);
  }
  {
    // Disabled if not recording sched_switch.
    FtraceConfig cfg = CreateFtraceConfig({});

    FtraceConfigId id = 45;
    ASSERT_TRUE(muxer.SetupConfig(id, cfg));
    const FtraceDataSourceConfig* ds_config = muxer.GetDataSourceConfig(id);
    ASSERT_TRUE(ds_config);
    EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
                Not(Contains(kFakeSchedSwitchEventId)));
    EXPECT_FALSE(ds_config->compact_sched.enabled);
  }
}

// Fixture that constructs a FtraceConfigMuxer with a fake
// ProtoTranslationTable.
class FtraceConfigMuxerFakeTableTest : public FtraceConfigMuxerTest {
 protected:
  std::unique_ptr<ProtoTranslationTable> table_ = CreateFakeTable();
  FtraceConfigMuxer model_ = FtraceConfigMuxer(&ftrace_,
                                               &atrace_wrapper_,
                                               table_.get(),
                                               GetSyscallTable(),
                                               {});
};

TEST_F(FtraceConfigMuxerFakeTableTest, GenericSyscallFiltering) {
  FtraceConfig config = CreateFtraceConfig({"raw_syscalls/sys_enter"});
  *config.add_syscall_events() = "sys_open";
  *config.add_syscall_events() = "sys_read";

  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillOnce(Return("nop"));
  EXPECT_CALL(ftrace_, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));
  EXPECT_CALL(ftrace_, WriteToFile(_, _)).WillRepeatedly(Return(true));
  EXPECT_CALL(ftrace_, WriteToFile("/root/events/raw_syscalls/sys_enter/filter",
                                   "id == 0 || id == 1"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/events/raw_syscalls/sys_exit/filter",
                                   "id == 0 || id == 1"));

  FtraceConfigId id = 37;
  ASSERT_TRUE(model_.SetupConfig(id, config));
  ASSERT_TRUE(model_.ActivateConfig(id));

  const std::set<size_t>& filter = model_.GetSyscallFilterForTesting();
  ASSERT_THAT(filter, UnorderedElementsAre(0, 1));
}

TEST_F(FtraceConfigMuxerFakeTableTest, UnknownSyscallFilter) {
  FtraceConfig config = CreateFtraceConfig({"raw_syscalls/sys_enter"});
  config.add_syscall_events("sys_open");
  config.add_syscall_events("sys_not_a_call");

  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillOnce(Return("nop"));
  EXPECT_CALL(ftrace_, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));

  // Unknown syscall is ignored.
  ASSERT_TRUE(model_.SetupConfig(/*id = */ 73, config));
  ASSERT_THAT(model_.GetSyscallFilterForTesting(), UnorderedElementsAre(0));
}

TEST_F(FtraceConfigMuxerFakeTableTest, SyscallFilterMuxing) {
  FtraceConfig empty_config = CreateFtraceConfig({});

  FtraceConfig syscall_config = empty_config;
  syscall_config.add_ftrace_events("raw_syscalls/sys_enter");

  FtraceConfig syscall_open_config = syscall_config;
  syscall_open_config.add_syscall_events("sys_open");

  FtraceConfig syscall_read_config = syscall_config;
  syscall_read_config.add_syscall_events("sys_read");

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));

  // Expect no filter for non-syscall config.
  ASSERT_TRUE(model_.SetupConfig(/* id= */ 179239, empty_config));
  ASSERT_THAT(model_.GetSyscallFilterForTesting(), UnorderedElementsAre());

  // Expect no filter for syscall config with no specified events.
  FtraceConfigId syscall_id = 73;
  ASSERT_TRUE(model_.SetupConfig(syscall_id, syscall_config));
  ASSERT_THAT(model_.GetSyscallFilterForTesting(), UnorderedElementsAre());

  // Still expect no filter to satisfy this and the above.
  FtraceConfigId syscall_open_id = 101;
  ASSERT_TRUE(model_.SetupConfig(syscall_open_id, syscall_open_config));
  ASSERT_THAT(model_.GetSyscallFilterForTesting(), UnorderedElementsAre());

  // After removing the generic syscall trace, only the one with filter is left.
  ASSERT_TRUE(model_.RemoveConfig(syscall_id));
  ASSERT_THAT(model_.GetSyscallFilterForTesting(), UnorderedElementsAre(0));

  // With sys_read and sys_open traced separately, filter includes both.
  FtraceConfigId syscall_read_id = 57;
  ASSERT_TRUE(model_.SetupConfig(syscall_read_id, syscall_read_config));
  ASSERT_THAT(model_.GetSyscallFilterForTesting(), UnorderedElementsAre(0, 1));

  // After removing configs with filters, filter is reset to empty.
  ASSERT_TRUE(model_.RemoveConfig(syscall_open_id));
  ASSERT_TRUE(model_.RemoveConfig(syscall_read_id));
  ASSERT_THAT(model_.GetSyscallFilterForTesting(), UnorderedElementsAre());
}

TEST_F(FtraceConfigMuxerFakeTableTest, TurnFtraceOnOff) {
  FtraceConfig config = CreateFtraceConfig({"sched_switch", "foo"});

  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillOnce(Return("nop"));
  EXPECT_CALL(ftrace_, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));
  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace_, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace_, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());
  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(ftrace_, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/sched/sched_switch/enable", "1"));

  FtraceConfigId id = 97;
  ASSERT_TRUE(model_.SetupConfig(id, config));

  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "1"));
  ASSERT_TRUE(model_.ActivateConfig(id));

  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_TRUE(ds_config);
  ASSERT_THAT(ds_config->event_filter.GetEnabledEvents(),
              ElementsAreArray({kFakeSchedSwitchEventId}));

  const EventFilter* central_filter = model_.GetCentralEventFilterForTesting();
  ASSERT_THAT(central_filter->GetEnabledEvents(),
              ElementsAreArray({kFakeSchedSwitchEventId}));

  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&ftrace_));
  EXPECT_CALL(ftrace_, NumberOfCpus()).Times(AnyNumber());
  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_percent", _))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/sched/sched_switch/enable", "0"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_size_kb", PageSizeKb()));
  EXPECT_CALL(ftrace_, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace_, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace_, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "1"));

  ASSERT_TRUE(model_.RemoveConfig(id));
}

TEST_F(FtraceConfigMuxerFakeTableTest, FtraceIsAlreadyOn) {
  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});

  // If someone is using ftrace already don't stomp on what they are doing.
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillOnce(Return("function"));
  ASSERT_FALSE(model_.SetupConfig(/* id= */ 123, config));
}

TEST_F(FtraceConfigMuxerFakeTableTest, Atrace) {
  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});
  *config.add_atrace_categories() = "sched";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));
  EXPECT_CALL(atrace_wrapper_,
              RunAtrace(ElementsAreArray({"atrace", "--async_start",
                                          "--only_userspace", "sched"}),
                        _))
      .WillOnce(Return(true));

  FtraceConfigId id = 57;
  ASSERT_TRUE(model_.SetupConfig(id, config));

  // "ftrace" group events are always enabled, and therefore the "print" event
  // will show up in the per data source event filter (as we want to record it),
  // but not the central filter (as we're not enabling/disabling it).
  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_TRUE(ds_config);
  EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
              Contains(kFakeSchedSwitchEventId));
  EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
              Contains(kFakePrintEventId));

  const EventFilter* central_filter = model_.GetCentralEventFilterForTesting();
  EXPECT_THAT(central_filter->GetEnabledEvents(),
              Contains(kFakeSchedSwitchEventId));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(
          ElementsAreArray({"atrace", "--async_stop", "--only_userspace"}), _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id));
}

TEST_F(FtraceConfigMuxerFakeTableTest, AtraceTwoApps) {
  FtraceConfig config = CreateFtraceConfig({});
  *config.add_atrace_apps() = "com.google.android.gms.persistent";
  *config.add_atrace_apps() = "com.google.android.gms";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));
  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(
          ElementsAreArray(
              {"atrace", "--async_start", "--only_userspace", "-a",
               "com.google.android.gms,com.google.android.gms.persistent"}),
          _))
      .WillOnce(Return(true));

  FtraceConfigId id = 97;
  ASSERT_TRUE(model_.SetupConfig(id, config));

  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_TRUE(ds_config);
  ASSERT_THAT(ds_config->event_filter.GetEnabledEvents(),
              Contains(kFakePrintEventId));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(
          ElementsAreArray({"atrace", "--async_stop", "--only_userspace"}), _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id));
}

TEST_F(FtraceConfigMuxerFakeTableTest, AtraceMultipleConfigs) {
  FtraceConfig config_a = CreateFtraceConfig({});
  *config_a.add_atrace_apps() = "app_a";
  *config_a.add_atrace_categories() = "cat_a";

  FtraceConfig config_b = CreateFtraceConfig({});
  *config_b.add_atrace_apps() = "app_b";
  *config_b.add_atrace_categories() = "cat_b";

  FtraceConfig config_c = CreateFtraceConfig({});
  *config_c.add_atrace_apps() = "app_c";
  *config_c.add_atrace_categories() = "cat_c";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));
  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_a", "-a", "app_a"}),
                _))
      .WillOnce(Return(true));
  FtraceConfigId id_a = 3;
  ASSERT_TRUE(model_.SetupConfig(id_a, config_a));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_a", "cat_b", "-a", "app_a,app_b"}),
                _))
      .WillOnce(Return(true));
  FtraceConfigId id_b = 13;
  ASSERT_TRUE(model_.SetupConfig(id_b, config_b));

  EXPECT_CALL(atrace_wrapper_,
              RunAtrace(ElementsAreArray({"atrace", "--async_start",
                                          "--only_userspace", "cat_a", "cat_b",
                                          "cat_c", "-a", "app_a,app_b,app_c"}),
                        _))
      .WillOnce(Return(true));
  FtraceConfigId id_c = 23;
  ASSERT_TRUE(model_.SetupConfig(id_c, config_c));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_a", "cat_c", "-a", "app_a,app_c"}),
                _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_b));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_c", "-a", "app_c"}),
                _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_a));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(
          ElementsAreArray({"atrace", "--async_stop", "--only_userspace"}), _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_c));
}

TEST_F(FtraceConfigMuxerFakeTableTest, AtraceFailedConfig) {
  FtraceConfig config_a = CreateFtraceConfig({});
  *config_a.add_atrace_apps() = "app_1";
  *config_a.add_atrace_apps() = "app_2";
  *config_a.add_atrace_categories() = "cat_1";
  *config_a.add_atrace_categories() = "cat_2";

  FtraceConfig config_b = CreateFtraceConfig({});
  *config_b.add_atrace_apps() = "app_fail";
  *config_b.add_atrace_categories() = "cat_fail";

  FtraceConfig config_c = CreateFtraceConfig({});
  *config_c.add_atrace_apps() = "app_1";
  *config_c.add_atrace_apps() = "app_3";
  *config_c.add_atrace_categories() = "cat_1";
  *config_c.add_atrace_categories() = "cat_3";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));
  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_1", "cat_2", "-a", "app_1,app_2"}),
                _))
      .WillOnce(Return(true));
  FtraceConfigId id_a = 7;
  ASSERT_TRUE(model_.SetupConfig(id_a, config_a));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_1", "cat_2", "cat_fail", "-a",
                                  "app_1,app_2,app_fail"}),
                _))
      .WillOnce(Return(false));
  FtraceConfigId id_b = 17;
  ASSERT_TRUE(model_.SetupConfig(id_b, config_b));

  EXPECT_CALL(atrace_wrapper_,
              RunAtrace(ElementsAreArray({"atrace", "--async_start",
                                          "--only_userspace", "cat_1", "cat_2",
                                          "cat_3", "-a", "app_1,app_2,app_3"}),
                        _))
      .WillOnce(Return(true));
  FtraceConfigId id_c = 47;
  ASSERT_TRUE(model_.SetupConfig(id_c, config_c));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_1", "cat_2", "-a", "app_1,app_2"}),
                _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_c));

  // Removing the config we failed to enable doesn't change the atrace state
  // so we don't expect a call here.
  ASSERT_TRUE(model_.RemoveConfig(id_b));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(
          ElementsAreArray({"atrace", "--async_stop", "--only_userspace"}), _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_a));
}

TEST_F(FtraceConfigMuxerFakeTableTest, AtraceDuplicateConfigs) {
  FtraceConfig config_a = CreateFtraceConfig({});
  *config_a.add_atrace_apps() = "app_1";
  *config_a.add_atrace_categories() = "cat_1";

  FtraceConfig config_b = CreateFtraceConfig({});
  *config_b.add_atrace_apps() = "app_1";
  *config_b.add_atrace_categories() = "cat_1";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));
  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_1", "-a", "app_1"}),
                _))
      .WillOnce(Return(true));
  FtraceConfigId id_a = 19;
  ASSERT_TRUE(model_.SetupConfig(id_a, config_a));

  FtraceConfigId id_b = 29;
  ASSERT_TRUE(model_.SetupConfig(id_b, config_b));

  ASSERT_TRUE(model_.RemoveConfig(id_a));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(
          ElementsAreArray({"atrace", "--async_stop", "--only_userspace"}), _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_b));
}

TEST_F(FtraceConfigMuxerFakeTableTest, AtraceAndFtraceConfigs) {
  FtraceConfig config_a = CreateFtraceConfig({"sched/sched_cpu_hotplug"});

  FtraceConfig config_b = CreateFtraceConfig({"sched/sched_switch"});
  *config_b.add_atrace_categories() = "b";

  FtraceConfig config_c = CreateFtraceConfig({"sched/sched_switch"});

  FtraceConfig config_d = CreateFtraceConfig({"sched/sched_cpu_hotplug"});
  *config_d.add_atrace_categories() = "d";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));
  FtraceConfigId id_a = 179;
  ASSERT_TRUE(model_.SetupConfig(id_a, config_a));

  EXPECT_CALL(atrace_wrapper_,
              RunAtrace(ElementsAreArray({"atrace", "--async_start",
                                          "--only_userspace", "b"}),
                        _))
      .WillOnce(Return(true));
  FtraceConfigId id_b = 239;
  ASSERT_TRUE(model_.SetupConfig(id_b, config_b));

  FtraceConfigId id_c = 101;
  ASSERT_TRUE(model_.SetupConfig(id_c, config_c));

  EXPECT_CALL(atrace_wrapper_,
              RunAtrace(ElementsAreArray({"atrace", "--async_start",
                                          "--only_userspace", "b", "d"}),
                        _))
      .WillOnce(Return(true));
  FtraceConfigId id_d = 47;
  ASSERT_TRUE(model_.SetupConfig(id_d, config_d));

  EXPECT_CALL(atrace_wrapper_,
              RunAtrace(ElementsAreArray({"atrace", "--async_start",
                                          "--only_userspace", "b"}),
                        _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_d));

  ASSERT_TRUE(model_.RemoveConfig(id_c));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(
          ElementsAreArray({"atrace", "--async_stop", "--only_userspace"}), _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_b));

  ASSERT_TRUE(model_.RemoveConfig(id_a));
}

TEST_F(FtraceConfigMuxerFakeTableTest, AtraceErrorsPropagated) {
  FtraceConfig config = CreateFtraceConfig({});
  *config.add_atrace_categories() = "cat_1";
  *config.add_atrace_categories() = "cat_2";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_1", "cat_2"}),
                _))
      .WillOnce(Invoke([](const std::vector<std::string>&, std::string* err) {
        EXPECT_NE(err, nullptr);
        if (err)
          err->append("foo\nbar\n");
        return true;
      }));

  FtraceSetupErrors errors{};
  FtraceConfigId id_a = 23;
  ASSERT_TRUE(model_.SetupConfig(id_a, config, &errors));
  EXPECT_EQ(errors.atrace_errors, "foo\nbar\n");
}

TEST_F(FtraceConfigMuxerFakeTableTest, AtracePreferTrackEvent) {
  const FtraceConfigId id_a = 3;
  FtraceConfig config_a = CreateFtraceConfig({});
  *config_a.add_atrace_categories() = "cat_1";
  *config_a.add_atrace_categories() = "cat_2";
  *config_a.add_atrace_categories() = "cat_3";
  *config_a.add_atrace_categories_prefer_sdk() = "cat_1";
  *config_a.add_atrace_categories_prefer_sdk() = "cat_2";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));
  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_1", "cat_2", "cat_3"}),
                _))
      .WillOnce(Return(true));
  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--prefer_sdk", "cat_1", "cat_2"}),
                _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.SetupConfig(id_a, config_a));

  const FtraceConfigId id_b = 13;
  FtraceConfig config_b = CreateFtraceConfig({});
  *config_b.add_atrace_categories() = "cat_1";
  *config_b.add_atrace_categories() = "cat_2";
  *config_b.add_atrace_categories() = "cat_3";
  *config_b.add_atrace_categories_prefer_sdk() = "cat_2";
  *config_b.add_atrace_categories_prefer_sdk() = "cat_3";

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--prefer_sdk", "cat_2"}), _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.SetupConfig(id_b, config_b));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--prefer_sdk", "cat_1", "cat_2"}),
                _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_b));

  const FtraceConfigId id_c = 13;
  FtraceConfig config_c = CreateFtraceConfig({});
  *config_c.add_atrace_categories() = "cat_1";
  *config_c.add_atrace_categories() = "cat_2";
  *config_c.add_atrace_categories() = "cat_3";
  *config_c.add_atrace_categories() = "cat_4";
  *config_c.add_atrace_categories_prefer_sdk() = "cat_1";
  *config_c.add_atrace_categories_prefer_sdk() = "cat_3";
  *config_c.add_atrace_categories_prefer_sdk() = "cat_4";

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_1", "cat_2", "cat_3", "cat_4"}),
                _))
      .WillOnce(Return(true));
  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--prefer_sdk", "cat_1", "cat_4"}),
                _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.SetupConfig(id_c, config_c));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--async_start", "--only_userspace",
                                  "cat_1", "cat_2", "cat_3"}),
                _))
      .WillOnce(Return(true));
  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(ElementsAreArray({"atrace", "--prefer_sdk", "cat_1", "cat_2"}),
                _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_c));

  EXPECT_CALL(
      atrace_wrapper_,
      RunAtrace(
          ElementsAreArray({"atrace", "--async_stop", "--only_userspace"}), _))
      .WillOnce(Return(true));
  EXPECT_CALL(atrace_wrapper_,
              RunAtrace(ElementsAreArray({"atrace", "--prefer_sdk"}), _))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.RemoveConfig(id_a));
}

TEST_F(FtraceConfigMuxerFakeTableTest, SetupClockForTesting) {
  FtraceConfig config;

  namespace pb0 = protos::pbzero;

  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());

  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/trace_clock", "boot"));
  model_.SetupClockForTesting(config);
  // unspecified = boot.
  EXPECT_EQ(model_.ftrace_clock(),
            static_cast<int>(pb0::FTRACE_CLOCK_UNSPECIFIED));

  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/trace_clock", "global"));
  model_.SetupClockForTesting(config);
  EXPECT_EQ(model_.ftrace_clock(), static_cast<int>(pb0::FTRACE_CLOCK_GLOBAL));

  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return(""));
  model_.SetupClockForTesting(config);
  EXPECT_EQ(model_.ftrace_clock(), static_cast<int>(pb0::FTRACE_CLOCK_UNKNOWN));

  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("local [global]"));
  model_.SetupClockForTesting(config);
  EXPECT_EQ(model_.ftrace_clock(), static_cast<int>(pb0::FTRACE_CLOCK_GLOBAL));
}

TEST_F(FtraceConfigMuxerFakeTableTest, GetFtraceEvents) {
  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});
  std::set<GroupAndName> events =
      model_.GetFtraceEventsForTesting(config, table_.get());

  EXPECT_THAT(events, Contains(GroupAndName("sched", "sched_switch")));
  EXPECT_THAT(events, Not(Contains(GroupAndName("ftrace", "print"))));
}

TEST_F(FtraceConfigMuxerFakeTableTest, GetFtraceEventsAtrace) {
  FtraceConfig config = CreateFtraceConfig({});
  *config.add_atrace_categories() = "sched";
  std::set<GroupAndName> events =
      model_.GetFtraceEventsForTesting(config, table_.get());

  EXPECT_THAT(events, Contains(GroupAndName("sched", "sched_switch")));
  EXPECT_THAT(events, Contains(GroupAndName("sched", "sched_cpu_hotplug")));
  EXPECT_THAT(events, Contains(GroupAndName("ftrace", "print")));
}

TEST_F(FtraceConfigMuxerFakeTableTest, GetFtraceEventsAtraceCategories) {
  FtraceConfig config = CreateFtraceConfig({});
  *config.add_atrace_categories() = "sched";
  *config.add_atrace_categories() = "memreclaim";
  std::set<GroupAndName> events =
      model_.GetFtraceEventsForTesting(config, table_.get());

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
TEST_F(FtraceConfigMuxerFakeTableTest, FallbackOnSetEvent) {
  FtraceConfig config =
      CreateFtraceConfig({"sched/sched_switch", "cgroup/cgroup_mkdir"});

  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_percent", _))
      .WillRepeatedly(Return(true));

  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillOnce(Return("nop"));
  EXPECT_CALL(ftrace_, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));
  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace_, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace_, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());
  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(ftrace_, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/sched/sched_switch/enable", "1"));
  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/cgroup/cgroup_mkdir/enable", "1"))
      .WillOnce(Return(false));
  EXPECT_CALL(ftrace_, AppendToFile("/root/set_event", "cgroup:cgroup_mkdir"))
      .WillOnce(Return(true));
  FtraceConfigId id = 97;
  ASSERT_TRUE(model_.SetupConfig(id, config));

  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "1"));
  ASSERT_TRUE(model_.ActivateConfig(id));

  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_TRUE(ds_config);
  EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
              Contains(kFakeSchedSwitchEventId));
  EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
              Contains(kCgroupMkdirEventId));

  const EventFilter* central_filter = model_.GetCentralEventFilterForTesting();
  EXPECT_THAT(central_filter->GetEnabledEvents(),
              Contains(kFakeSchedSwitchEventId));
  EXPECT_THAT(central_filter->GetEnabledEvents(),
              Contains(kCgroupMkdirEventId));

  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/sched/sched_switch/enable", "0"));
  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/cgroup/cgroup_mkdir/enable", "0"))
      .WillOnce(Return(false));
  EXPECT_CALL(ftrace_, AppendToFile("/root/set_event", "!cgroup:cgroup_mkdir"))
      .WillOnce(Return(true));
  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_size_kb", PageSizeKb()));
  EXPECT_CALL(ftrace_, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace_, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace_, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "1"));
  ASSERT_TRUE(model_.RemoveConfig(id));
}

TEST_F(FtraceConfigMuxerFakeTableTest, CompactSchedConfigWithInvalidFormat) {
  // Request compact encoding.
  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});
  config.mutable_compact_sched()->set_enabled(true);

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));

  FtraceConfigId id = 67;
  ASSERT_TRUE(model_.SetupConfig(id, config));

  // The translation table says that the scheduling events' format didn't match
  // compile-time assumptions, so we won't enable compact events even if
  // requested.
  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_TRUE(ds_config);
  EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
              Contains(kFakeSchedSwitchEventId));
  EXPECT_FALSE(ds_config->compact_sched.enabled);
}

TEST_F(FtraceConfigMuxerFakeTableTest, SkipGenericEventsOption) {
  static constexpr int kFtraceGenericEventId = 42;
  ON_CALL(ftrace_, ReadEventFormat("sched", "generic"))
      .WillByDefault(Return(R"(name: generic
ID: 42
format:
	field:int common_pid;	offset:0;	size:4;	signed:1;

	field:u32 field_a;	offset:4;	size:4;	signed:0;
	field:int field_b;	offset:8;	size:4;	signed:1;

print fmt: "unused")"));

  // Data source asking for one known and one generic event.
  FtraceConfig config_default =
      CreateFtraceConfig({"sched/sched_switch", "sched/generic"});

  // As above, but with an option to suppress generic events.
  FtraceConfig config_with_disable =
      CreateFtraceConfig({"sched/sched_switch", "sched/generic"});
  config_with_disable.set_disable_generic_events(true);

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));

  {
    FtraceConfigId id = 123;
    ASSERT_TRUE(model_.SetupConfig(id, config_default));
    const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
    ASSERT_TRUE(ds_config);
    // Both events enabled for the data source by default.
    EXPECT_THAT(
        ds_config->event_filter.GetEnabledEvents(),
        UnorderedElementsAre(kFakeSchedSwitchEventId, kFtraceGenericEventId));
  }
  {
    FtraceConfigId id = 321;
    ASSERT_TRUE(model_.SetupConfig(id, config_with_disable));
    const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
    ASSERT_TRUE(ds_config);
    // Only the statically known event is enabled.
    EXPECT_THAT(ds_config->event_filter.GetEnabledEvents(),
                UnorderedElementsAre(kFakeSchedSwitchEventId));
  }
}

TEST_F(FtraceConfigMuxerFakeTableTest, Funcgraph) {
  FtraceConfig config;
  config.set_enable_function_graph(true);
  *config.add_function_filters() = "sched*";
  *config.add_function_filters() = "handle_mm_fault";

  *config.add_function_graph_roots() = "sched*";
  *config.add_function_graph_roots() = "*mm_fault";

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));

  EXPECT_CALL(ftrace_, WriteToFile(_, _)).WillRepeatedly(Return(true));

  EXPECT_CALL(ftrace_, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace_, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));

  // Set up config, assert that the tracefs writes happened:
  EXPECT_CALL(ftrace_, ClearFile("/root/set_ftrace_filter"));
  EXPECT_CALL(ftrace_, ClearFile("/root/set_graph_function"));
  EXPECT_CALL(ftrace_, AppendToFile("/root/set_ftrace_filter",
                                    "sched*\nhandle_mm_fault"))
      .WillOnce(Return(true));
  EXPECT_CALL(ftrace_,
              AppendToFile("/root/set_graph_function", "sched*\n*mm_fault"))
      .WillOnce(Return(true));
  EXPECT_CALL(ftrace_, WriteToFile("/root/current_tracer", "function_graph"))
      .WillOnce(Return(true));
  FtraceConfigId id = 43;
  ASSERT_TRUE(model_.SetupConfig(id, config));
  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&ftrace_));
  // Toggle config on and off, tracer won't be reset yet:
  ASSERT_TRUE(model_.ActivateConfig(id));
  ASSERT_TRUE(model_.RemoveConfig(id));
  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&ftrace_));

  // Emulate ftrace_controller's call to ResetCurrentTracer (see impl comments
  // for why RemoveConfig is insufficient).
  EXPECT_CALL(ftrace_, ClearFile("/root/set_ftrace_filter"));
  EXPECT_CALL(ftrace_, ClearFile("/root/set_graph_function"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/current_tracer", "nop"))
      .WillOnce(Return(true));
  ASSERT_TRUE(model_.ResetCurrentTracer());
  ASSERT_TRUE(testing::Mock::VerifyAndClearExpectations(&ftrace_));
}

TEST_F(FtraceConfigMuxerFakeTableTest, PreserveFtraceBufferNotSetBufferSizeKb) {
  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});

  config.set_preserve_ftrace_buffer(true);
  EXPECT_CALL(ftrace_, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));
  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());
  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_size_kb", _)).Times(0);
  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/sched/sched_switch/enable", "1"));

  FtraceConfigId id = 44;
  ASSERT_TRUE(model_.SetupConfig(id, config));
}

// Fixture that constructs a FtraceConfigMuxer with a mock
// ProtoTranslationTable.
class FtraceConfigMuxerMockTableTest : public FtraceConfigMuxerTest {
 protected:
  std::unique_ptr<MockProtoTranslationTable> mock_table_ = GetMockTable();
  FtraceConfigMuxer model_ = FtraceConfigMuxer(&ftrace_,
                                               &atrace_wrapper_,
                                               mock_table_.get(),
                                               GetSyscallTable(),
                                               {});
};

TEST_F(FtraceConfigMuxerMockTableTest, AddGenericEvent) {
  FtraceConfig config = CreateFtraceConfig({"power/cpu_frequency"});

  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillOnce(Return("nop"));
  EXPECT_CALL(ftrace_, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));
  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace_, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace_, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());
  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(ftrace_, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/power/cpu_frequency/enable", "1"));
  EXPECT_CALL(*mock_table_, GetEvent(GroupAndName("power", "cpu_frequency")))
      .Times(AnyNumber());

  static constexpr int kExpectedEventId = 77;
  Event event_to_return;
  event_to_return.name = "cpu_frequency";
  event_to_return.group = "power";
  event_to_return.ftrace_event_id = kExpectedEventId;
  ON_CALL(*mock_table_,
          GetOrCreateEvent(GroupAndName("power", "cpu_frequency")))
      .WillByDefault(Return(&event_to_return));
  EXPECT_CALL(*mock_table_,
              GetOrCreateEvent(GroupAndName("power", "cpu_frequency")));

  FtraceConfigId id = 7;
  ASSERT_TRUE(model_.SetupConfig(id, config));

  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "1"));
  ASSERT_TRUE(model_.ActivateConfig(id));

  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_TRUE(ds_config);
  ASSERT_THAT(ds_config->event_filter.GetEnabledEvents(),
              ElementsAreArray({kExpectedEventId}));

  const EventFilter* central_filter = model_.GetCentralEventFilterForTesting();
  ASSERT_THAT(central_filter->GetEnabledEvents(),
              ElementsAreArray({kExpectedEventId}));
}

TEST_F(FtraceConfigMuxerMockTableTest, AddAllEvents) {
  FtraceConfig config = CreateFtraceConfig({"sched/*"});

  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillOnce(Return("nop"));
  EXPECT_CALL(ftrace_, ReadOneCharFromFile("/root/tracing_on"))
      .WillOnce(Return('1'));
  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "0"));
  EXPECT_CALL(ftrace_, WriteToFile("/root/events/enable", "0"));
  EXPECT_CALL(ftrace_, ClearFile("/root/trace"));
  EXPECT_CALL(ftrace_, ClearFile(MatchesRegex("/root/per_cpu/cpu[0-9]/trace")));
  ON_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .WillByDefault(Return("[local] global boot"));
  EXPECT_CALL(ftrace_, ReadFileIntoString("/root/trace_clock"))
      .Times(AnyNumber());
  EXPECT_CALL(ftrace_, WriteToFile("/root/buffer_size_kb", _));
  EXPECT_CALL(ftrace_, WriteToFile("/root/trace_clock", "boot"));
  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/sched/sched_switch/enable", "1"));
  EXPECT_CALL(ftrace_,
              WriteToFile("/root/events/sched/sched_new_event/enable", "1"));

  std::set<std::string> n = {"sched_switch", "sched_new_event"};
  ON_CALL(ftrace_, GetEventNamesForGroup("events/sched"))
      .WillByDefault(Return(n));
  EXPECT_CALL(ftrace_, GetEventNamesForGroup("events/sched")).Times(1);

  // Non-generic event.
  static constexpr int kSchedSwitchEventId = 1;
  Event sched_switch = {"sched_switch", "sched", {}, 0, 0, 0};
  sched_switch.ftrace_event_id = kSchedSwitchEventId;
  ON_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("sched", "sched_switch")))
      .WillByDefault(Return(&sched_switch));
  EXPECT_CALL(*mock_table_,
              GetOrCreateEvent(GroupAndName("sched", "sched_switch")))
      .Times(AnyNumber());

  // Generic event.
  static constexpr int kGenericEventId = 2;
  Event event_to_return;
  event_to_return.name = "sched_new_event";
  event_to_return.group = "sched";
  event_to_return.ftrace_event_id = kGenericEventId;
  ON_CALL(*mock_table_,
          GetOrCreateEvent(GroupAndName("sched", "sched_new_event")))
      .WillByDefault(Return(&event_to_return));
  EXPECT_CALL(*mock_table_,
              GetOrCreateEvent(GroupAndName("sched", "sched_new_event")));

  FtraceConfigId id = 13;
  ASSERT_TRUE(model_.SetupConfig(id, config));
  ASSERT_TRUE(id);

  EXPECT_CALL(ftrace_, WriteToFile("/root/tracing_on", "1"));
  ASSERT_TRUE(model_.ActivateConfig(id));

  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_TRUE(ds_config);
  ASSERT_THAT(ds_config->event_filter.GetEnabledEvents(),
              ElementsAreArray({kSchedSwitchEventId, kGenericEventId}));

  const EventFilter* central_filter = model_.GetCentralEventFilterForTesting();
  ASSERT_THAT(central_filter->GetEnabledEvents(),
              ElementsAreArray({kSchedSwitchEventId, kGenericEventId}));
}

TEST_F(FtraceConfigMuxerMockTableTest, TwoWildcardGroups) {
  FtraceConfig config = CreateFtraceConfig({"group_one/*", "group_two/*"});

  std::set<std::string> event_names = {"foo"};
  ON_CALL(ftrace_, GetEventNamesForGroup("events/group_one"))
      .WillByDefault(Return(event_names));
  EXPECT_CALL(ftrace_, GetEventNamesForGroup("events/group_one"))
      .Times(AnyNumber());

  ON_CALL(ftrace_, GetEventNamesForGroup("events/group_two"))
      .WillByDefault(Return(event_names));
  EXPECT_CALL(ftrace_, GetEventNamesForGroup("events/group_two"))
      .Times(AnyNumber());

  static constexpr int kEventId1 = 1;
  Event event1;
  event1.name = "foo";
  event1.group = "group_one";
  event1.ftrace_event_id = kEventId1;
  ON_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("group_one", "foo")))
      .WillByDefault(Return(&event1));
  EXPECT_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("group_one", "foo")));

  static constexpr int kEventId2 = 2;
  Event event2;
  event2.name = "foo";
  event2.group = "group_two";
  event2.ftrace_event_id = kEventId2;
  ON_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("group_two", "foo")))
      .WillByDefault(Return(&event2));
  EXPECT_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("group_two", "foo")));

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));

  FtraceConfigId id = 23;
  ASSERT_TRUE(model_.SetupConfig(id, config));
  ASSERT_TRUE(model_.ActivateConfig(id));

  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_TRUE(ds_config);
  ASSERT_THAT(ds_config->event_filter.GetEnabledEvents(),
              ElementsAreArray({kEventId1, kEventId2}));

  const EventFilter* central_filter = model_.GetCentralEventFilterForTesting();
  ASSERT_THAT(central_filter->GetEnabledEvents(),
              ElementsAreArray({kEventId1, kEventId2}));
}

TEST_F(FtraceConfigMuxerMockTableTest, AddSameNameEvents) {
  FtraceConfig config = CreateFtraceConfig({"group_one/foo", "group_two/foo"});

  static constexpr int kEventId1 = 1;
  Event event1;
  event1.name = "foo";
  event1.group = "group_one";
  event1.ftrace_event_id = kEventId1;
  ON_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("group_one", "foo")))
      .WillByDefault(Return(&event1));
  EXPECT_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("group_one", "foo")));

  static constexpr int kEventId2 = 2;
  Event event2;
  event2.name = "foo";
  event2.group = "group_two";
  event2.ftrace_event_id = kEventId2;
  ON_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("group_two", "foo")))
      .WillByDefault(Return(&event2));
  EXPECT_CALL(*mock_table_, GetOrCreateEvent(GroupAndName("group_two", "foo")));

  ON_CALL(ftrace_, ReadFileIntoString("/root/current_tracer"))
      .WillByDefault(Return("nop"));
  ON_CALL(ftrace_, ReadFileIntoString("/root/events/enable"))
      .WillByDefault(Return("0"));

  FtraceConfigId id = 5;
  ASSERT_TRUE(model_.SetupConfig(id, config));
  ASSERT_TRUE(model_.ActivateConfig(id));

  const FtraceDataSourceConfig* ds_config = model_.GetDataSourceConfig(id);
  ASSERT_THAT(ds_config->event_filter.GetEnabledEvents(),
              ElementsAreArray({kEventId1, kEventId2}));

  const EventFilter* central_filter = model_.GetCentralEventFilterForTesting();
  ASSERT_THAT(central_filter->GetEnabledEvents(),
              ElementsAreArray({kEventId1, kEventId2}));
}

}  // namespace
}  // namespace perfetto
