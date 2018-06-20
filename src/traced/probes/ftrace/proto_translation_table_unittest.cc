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

#include "src/traced/probes/ftrace/proto_translation_table.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"

using testing::_;
using testing::Values;
using testing::ValuesIn;
using testing::TestWithParam;
using testing::Return;
using testing::AnyNumber;
using testing::IsNull;
using testing::Contains;
using testing::Eq;
using testing::Pointee;

namespace perfetto {
namespace {

class MockFtraceProcfs : public FtraceProcfs {
 public:
  MockFtraceProcfs() : FtraceProcfs("/root/") {}

  MOCK_CONST_METHOD0(ReadPageHeaderFormat, std::string());
  MOCK_CONST_METHOD2(ReadEventFormat,
                     std::string(const std::string& group,
                                 const std::string& name));
};

class AllTranslationTableTest : public TestWithParam<const char*> {
 public:
  void SetUp() override {
    std::string path =
        "src/traced/probes/ftrace/test/data/" + std::string(GetParam()) + "/";
    FtraceProcfs ftrace_procfs(path);
    table_ = ProtoTranslationTable::Create(&ftrace_procfs, GetStaticEventInfo(),
                                           GetStaticCommonFieldsInfo());
    PERFETTO_CHECK(table_);
  }

  std::unique_ptr<ProtoTranslationTable> table_;
};

class TranslationTableCreationTest : public TestWithParam<uint16_t> {};

const char* kDevices[] = {
    "android_seed_N2F62_3.10.49", "android_hammerhead_MRA59G_3.4.0",
};

TEST_P(AllTranslationTableTest, Create) {
  EXPECT_TRUE(table_);
  EXPECT_TRUE(table_->GetEventByName("print"));
  EXPECT_TRUE(table_->GetEventByName("sched_switch"));
  EXPECT_TRUE(table_->GetEventByName("sched_wakeup"));
  EXPECT_TRUE(table_->GetEventByName("ext4_da_write_begin"));
  for (const Event& event : table_->events()) {
    if (!event.ftrace_event_id)
      continue;
    EXPECT_TRUE(event.name);
    EXPECT_TRUE(event.group);
    EXPECT_TRUE(event.proto_field_id);
    for (const Field& field : event.fields) {
      EXPECT_TRUE(field.proto_field_id);
      EXPECT_TRUE(field.ftrace_type);
      EXPECT_TRUE(field.proto_field_type);
    }
  }
  ASSERT_EQ(table_->common_fields().size(), 1u);
  const Field& pid_field = table_->common_fields().at(0);
  EXPECT_EQ(std::string(pid_field.ftrace_name), "common_pid");
  EXPECT_EQ(pid_field.proto_field_id, 2u);

  {
    auto event = table_->GetEventByName("print");
    EXPECT_TRUE(event);
    EXPECT_EQ(std::string(event->name), "print");
    EXPECT_EQ(std::string(event->group), "ftrace");
    EXPECT_EQ(event->fields.at(1).proto_field_type, kProtoString);
    EXPECT_EQ(event->fields.at(1).ftrace_type, kFtraceCString);
    EXPECT_EQ(event->fields.at(1).strategy, kCStringToString);
  }
}

INSTANTIATE_TEST_CASE_P(ByDevice, AllTranslationTableTest, ValuesIn(kDevices));

TEST(TranslationTableTest, Seed) {
  std::string path =
      "src/traced/probes/ftrace/test/data/android_seed_N2F62_3.10.49/";
  FtraceProcfs ftrace_procfs(path);
  auto table = ProtoTranslationTable::Create(
      &ftrace_procfs, GetStaticEventInfo(), GetStaticCommonFieldsInfo());
  const Field& pid_field = table->common_fields().at(0);
  EXPECT_EQ(std::string(pid_field.ftrace_name), "common_pid");
  EXPECT_EQ(pid_field.proto_field_id, 2u);
  EXPECT_EQ(pid_field.ftrace_offset, 4u);
  EXPECT_EQ(pid_field.ftrace_size, 4u);

  {
    auto event = table->GetEventByName("sched_switch");
    EXPECT_EQ(std::string(event->name), "sched_switch");
    EXPECT_EQ(std::string(event->group), "sched");
    EXPECT_EQ(event->ftrace_event_id, 68ul);
    EXPECT_EQ(event->fields.at(0).ftrace_offset, 8u);
    EXPECT_EQ(event->fields.at(0).ftrace_size, 16u);
  }

  {
    auto event = table->GetEventByName("sched_wakeup");
    EXPECT_EQ(std::string(event->name), "sched_wakeup");
    EXPECT_EQ(std::string(event->group), "sched");
    EXPECT_EQ(event->ftrace_event_id, 70ul);
    EXPECT_EQ(event->fields.at(0).ftrace_offset, 8u);
    EXPECT_EQ(event->fields.at(0).ftrace_size, 16u);
  }

  {
    auto event = table->GetEventByName("cpufreq_interactive_target");
    EXPECT_EQ(std::string(event->name), "cpufreq_interactive_target");
    EXPECT_EQ(std::string(event->group), "cpufreq_interactive");
    EXPECT_EQ(event->ftrace_event_id, 509ul);
    EXPECT_EQ(event->fields.at(0).ftrace_offset, 8u);
    EXPECT_EQ(event->fields.at(0).ftrace_size, 4u);
  }

  {
    auto event = table->GetEventByName("ext4_da_write_begin");
    EXPECT_EQ(std::string(event->name), "ext4_da_write_begin");
    EXPECT_EQ(std::string(event->group), "ext4");
    EXPECT_EQ(event->ftrace_event_id, 303ul);
    EXPECT_EQ(event->fields.at(0).ftrace_offset, 8u);
    EXPECT_EQ(event->fields.at(0).ftrace_size, 4u);
  }
}

TEST_P(TranslationTableCreationTest, Create) {
  MockFtraceProcfs ftrace;
  std::vector<Field> common_fields;
  std::vector<Event> events;

  ON_CALL(ftrace, ReadPageHeaderFormat())
      .WillByDefault(Return(
          R"(	field: u64 timestamp;	offset:0;	size:8;	signed:0;
	field: local_t commit;	offset:8;	size:)" +
          std::to_string(GetParam()) + R"(;	signed:1;
	field: int overwrite;	offset:8;	size:1;	signed:1;
	field: char data;	offset:16;	size:4080;	signed:0;)"));
  ON_CALL(ftrace, ReadEventFormat(_, _)).WillByDefault(Return(""));
  ON_CALL(ftrace, ReadEventFormat("group", "foo"))
      .WillByDefault(Return(R"(name: foo
ID: 42
format:
	field:unsigned short common_type;	offset:0;	size:2;	signed:0;
	field:int common_pid;	offset:4;	size:4;	signed:1;

	field:char field_a[16];	offset:8;	size:16;	signed:0;
	field:int field_b;	offset:24;	size:4;	signed:1;
	field:int field_d;	offset:28;	size:4;	signed:1;
	field:u32 field_e;	offset:32;	size:4;	signed:0;

print fmt: "some format")"));
  ;

  EXPECT_CALL(ftrace, ReadPageHeaderFormat()).Times(AnyNumber());
  EXPECT_CALL(ftrace, ReadEventFormat(_, _)).Times(AnyNumber());

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "foo";
    event->group = "group";
    event->proto_field_id = 21;

    {
      // We should get this field.
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->proto_field_id = 501;
      field->proto_field_type = kProtoString;
      field->ftrace_name = "field_a";
    }

    {
      // We shouldn't get this field: don't know how to read int -> string.
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->proto_field_id = 502;
      field->proto_field_type = kProtoString;
      field->ftrace_name = "field_b";
    }

    {
      // We shouldn't get this field: no matching field in the format file.
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->proto_field_id = 503;
      field->proto_field_type = kProtoString;
      field->ftrace_name = "field_c";
    }

    {
      // We should get this field.
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->proto_field_id = 504;
      field->proto_field_type = kProtoUint64;
      field->ftrace_name = "field_e";
    }
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "bar";
    event->group = "group";
    event->proto_field_id = 22;
  }

  auto table = ProtoTranslationTable::Create(&ftrace, std::move(events),
                                             std::move(common_fields));
  EXPECT_EQ(table->largest_id(), 42ul);
  EXPECT_EQ(table->EventNameToFtraceId("foo"), 42ul);
  EXPECT_EQ(table->EventNameToFtraceId("bar"), 0ul);
  EXPECT_EQ(table->EventNameToFtraceId("bar"), 0ul);
  EXPECT_FALSE(table->GetEventById(43ul));
  ASSERT_TRUE(table->GetEventById(42ul));
  EXPECT_EQ(table->ftrace_page_header_spec().timestamp.size, 8);
  EXPECT_EQ(table->ftrace_page_header_spec().size.size, GetParam());
  EXPECT_EQ(table->ftrace_page_header_spec().overwrite.size, 1);
  auto event = table->GetEventById(42);
  EXPECT_EQ(event->ftrace_event_id, 42ul);
  EXPECT_EQ(event->proto_field_id, 21ul);
  EXPECT_EQ(event->size, 36u);
  EXPECT_EQ(std::string(event->name), "foo");
  EXPECT_EQ(std::string(event->group), "group");

  ASSERT_EQ(event->fields.size(), 2ul);
  auto field_a = event->fields.at(0);
  EXPECT_EQ(field_a.proto_field_id, 501ul);
  EXPECT_EQ(field_a.strategy, kFixedCStringToString);

  auto field_e = event->fields.at(1);
  EXPECT_EQ(field_e.proto_field_id, 504ul);
  EXPECT_EQ(field_e.strategy, kUint32ToUint64);
}

INSTANTIATE_TEST_CASE_P(BySize, TranslationTableCreationTest, Values(4, 8));

TEST(TranslationTableTest, InferFtraceType) {
  FtraceFieldType type;

  ASSERT_TRUE(InferFtraceType("char foo[16]", 16, false, &type));
  EXPECT_EQ(type, kFtraceFixedCString);

  ASSERT_TRUE(InferFtraceType("char[] foo", 8, false, &type));
  EXPECT_EQ(type, kFtraceStringPtr);

  ASSERT_TRUE(InferFtraceType("char * foo", 8, false, &type));
  EXPECT_EQ(type, kFtraceStringPtr);

  ASSERT_TRUE(InferFtraceType("char foo[64]", 64, false, &type));
  EXPECT_EQ(type, kFtraceFixedCString);

  ASSERT_TRUE(InferFtraceType("u32 foo", 4, false, &type));
  EXPECT_EQ(type, kFtraceUint32);

  ASSERT_TRUE(InferFtraceType("i_ino foo", 4, false, &type));
  ASSERT_EQ(type, kFtraceInode32);

  ASSERT_TRUE(InferFtraceType("i_ino foo", 8, false, &type));
  ASSERT_EQ(type, kFtraceInode64);

  ASSERT_TRUE(InferFtraceType("ino_t foo", 4, false, &type));
  ASSERT_EQ(type, kFtraceInode32);

  ASSERT_TRUE(InferFtraceType("ino_t foo", 8, false, &type));
  ASSERT_EQ(type, kFtraceInode64);

  ASSERT_TRUE(InferFtraceType("dev_t foo", 4, false, &type));
  ASSERT_EQ(type, kFtraceDevId32);

  ASSERT_TRUE(InferFtraceType("dev_t foo", 8, false, &type));
  ASSERT_EQ(type, kFtraceDevId64);

  ASSERT_TRUE(InferFtraceType("pid_t foo", 4, false, &type));
  ASSERT_EQ(type, kFtracePid32);

  ASSERT_TRUE(InferFtraceType("int common_pid", 4, false, &type));
  ASSERT_EQ(type, kFtraceCommonPid32);

  ASSERT_TRUE(InferFtraceType("char foo", 1, true, &type));
  ASSERT_EQ(type, kFtraceInt8);

  ASSERT_TRUE(InferFtraceType("__data_loc char[] foo", 4, false, &type));
  ASSERT_EQ(type, kFtraceDataLoc);
  ASSERT_FALSE(InferFtraceType("__data_loc char[] foo", 8, false, &type));

  EXPECT_FALSE(InferFtraceType("foo", 64, false, &type));
}

TEST(TranslationTableTest, Getters) {
  std::vector<Field> common_fields;
  std::vector<Event> events;

  {
    Event event;
    event.name = "foo";
    event.group = "group_one";
    event.ftrace_event_id = 1;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "bar";
    event.group = "group_one";
    event.ftrace_event_id = 2;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "baz";
    event.group = "group_two";
    event.ftrace_event_id = 100;
    events.push_back(event);
  }

  ProtoTranslationTable table(
      events, std::move(common_fields),
      ProtoTranslationTable::DefaultPageHeaderSpecForTesting());
  EXPECT_EQ(table.largest_id(), 100ul);
  EXPECT_EQ(table.EventNameToFtraceId("foo"), 1ul);
  EXPECT_EQ(table.EventNameToFtraceId("baz"), 100ul);
  EXPECT_EQ(table.EventNameToFtraceId("no_such_event"), 0ul);
  EXPECT_EQ(table.GetEventById(1)->name, "foo");
  EXPECT_EQ(table.GetEventById(3), nullptr);
  EXPECT_EQ(table.GetEventById(200), nullptr);
  EXPECT_EQ(table.GetEventById(0), nullptr);
  EXPECT_EQ(table.GetEventByName("foo")->ftrace_event_id, 1u);
  EXPECT_THAT(*table.GetEventsByGroup("group_one"),
              Contains(testing::Field(&Event::name, "foo")));
  EXPECT_THAT(*table.GetEventsByGroup("group_one"),
              Contains(testing::Field(&Event::name, "bar")));
  EXPECT_THAT(*table.GetEventsByGroup("group_two"),
              Contains(testing::Field(&Event::name, "baz")));
  EXPECT_THAT(table.GetEventsByGroup("group_three"), IsNull());
}

}  // namespace
}  // namespace perfetto
