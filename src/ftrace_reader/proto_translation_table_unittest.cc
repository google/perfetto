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

#include "proto_translation_table.h"

#include "ftrace_procfs.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::ValuesIn;
using testing::TestWithParam;
using testing::Return;
using testing::AnyNumber;

namespace perfetto {
namespace {

class MockFtraceProcfs : public FtraceProcfs {
 public:
  MockFtraceProcfs() : FtraceProcfs("/root/") {}

  MOCK_CONST_METHOD2(ReadEventFormat,
                     std::string(const std::string& group,
                                 const std::string& name));
};

class AllTranslationTableTest : public TestWithParam<const char*> {
 public:
  void SetUp() override {
    std::string path =
        "src/ftrace_reader/test/data/" + std::string(GetParam()) + "/";
    FtraceProcfs ftrace_procfs(path);
    table_ = ProtoTranslationTable::Create(&ftrace_procfs, GetStaticEventInfo(),
                                           GetStaticCommonFieldsInfo());
  }

  std::unique_ptr<ProtoTranslationTable> table_;
};

const char* kDevices[] = {
    "android_seed_N2F62_3.10.49", "android_hammerhead_MRA59G_3.4.0",
    "synthetic",
};

TEST_P(AllTranslationTableTest, Create) {
  EXPECT_TRUE(table_);
  EXPECT_TRUE(table_->GetEventByName("print"));
  EXPECT_TRUE(table_->GetEventByName("sched_switch"));
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
}

INSTANTIATE_TEST_CASE_P(ByDevice, AllTranslationTableTest, ValuesIn(kDevices));

TEST(TranslationTable, Seed) {
  std::string path = "src/ftrace_reader/test/data/android_seed_N2F62_3.10.49/";
  FtraceProcfs ftrace_procfs(path);
  auto table = ProtoTranslationTable::Create(
      &ftrace_procfs, GetStaticEventInfo(), GetStaticCommonFieldsInfo());
  const Field& pid_field = table->common_fields().at(0);
  EXPECT_EQ(std::string(pid_field.ftrace_name), "common_pid");
  EXPECT_EQ(pid_field.proto_field_id, 2u);
  EXPECT_EQ(pid_field.ftrace_offset, 4u);
  EXPECT_EQ(pid_field.ftrace_size, 4u);

  auto sched_switch_event = table->GetEventByName("sched_switch");
  EXPECT_EQ(std::string(sched_switch_event->name), "sched_switch");
  EXPECT_EQ(std::string(sched_switch_event->group), "sched");
  EXPECT_EQ(sched_switch_event->ftrace_event_id, 68ul);
  EXPECT_EQ(sched_switch_event->fields.at(0).ftrace_offset, 8u);
  EXPECT_EQ(sched_switch_event->fields.at(0).ftrace_size, 16u);
}

TEST(TranslationTable, Create) {
  MockFtraceProcfs ftrace;
  std::vector<Field> common_fields;
  std::vector<Event> events;

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

TEST(TranslationTable, InferFtraceType) {
  FtraceFieldType type;

  ASSERT_TRUE(InferFtraceType("char * foo", 0, false, &type));
  EXPECT_EQ(type, kFtraceCString);

  ASSERT_TRUE(InferFtraceType("char foo[16]", 16, false, &type));
  EXPECT_EQ(type, kFtraceFixedCString);

  ASSERT_TRUE(InferFtraceType("char foo[64]", 64, false, &type));
  EXPECT_EQ(type, kFtraceFixedCString);

  ASSERT_TRUE(InferFtraceType("u32 foo", 4, false, &type));
  EXPECT_EQ(type, kFtraceUint32);

  EXPECT_FALSE(InferFtraceType("foo", 64, false, &type));
}

TEST(TranslationTable, Getters) {
  std::vector<Field> common_fields;
  std::vector<Event> events;

  {
    Event event;
    event.name = "foo";
    event.ftrace_event_id = 1;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "bar";
    event.ftrace_event_id = 2;
    events.push_back(event);
  }

  {
    Event event;
    event.name = "baz";
    event.ftrace_event_id = 100;
    events.push_back(event);
  }

  ProtoTranslationTable table(events, std::move(common_fields));
  EXPECT_EQ(table.largest_id(), 100ul);
  EXPECT_EQ(table.EventNameToFtraceId("foo"), 1ul);
  EXPECT_EQ(table.EventNameToFtraceId("baz"), 100ul);
  EXPECT_EQ(table.EventNameToFtraceId("no_such_event"), 0ul);
  EXPECT_EQ(table.GetEventById(1)->name, "foo");
  EXPECT_EQ(table.GetEventById(3), nullptr);
  EXPECT_EQ(table.GetEventById(200), nullptr);
  EXPECT_EQ(table.GetEventById(0), nullptr);
}

}  // namespace
}  // namespace perfetto
