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

#include "src/base/test/utils.h"
#include "src/traced/probes/ftrace/compact_sched.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/event_info_constants.h"
#include "src/traced/probes/ftrace/tracefs.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/descriptor.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"

using testing::_;
using testing::AllOf;
using testing::AnyNumber;
using testing::Contains;
using testing::ElementsAre;
using testing::Eq;
using testing::IsNull;
using testing::NiceMock;
using testing::Pointee;
using testing::Property;
using testing::Return;
using testing::StrEq;
using testing::TestWithParam;
using testing::Values;
using testing::ValuesIn;

namespace perfetto {
namespace {
using protozero::proto_utils::ProtoSchemaType;

MATCHER_P(FtraceFieldMatcher, expected_struct, "") {
  return ExplainMatchResult(
      AllOf(testing::Field("ftrace_name", &Field::ftrace_name,
                           StrEq(expected_struct.ftrace_name)),
            testing::Field("ftrace_type", &Field::ftrace_type,
                           expected_struct.ftrace_type),
            testing::Field("ftrace_offset", &Field::ftrace_offset,
                           expected_struct.ftrace_offset),
            testing::Field("ftrace_size", &Field::ftrace_size,
                           expected_struct.ftrace_size),
            testing::Field("proto_field_id", &Field::proto_field_id,
                           expected_struct.proto_field_id),
            testing::Field("proto_field_type", &Field::proto_field_type,
                           expected_struct.proto_field_type),
            testing::Field("strategy", &Field::strategy,
                           expected_struct.strategy)),
      arg, result_listener);
}

class MockTracefs : public Tracefs {
 public:
  MockTracefs() : Tracefs("/root/") {}

  MOCK_METHOD(std::string, ReadPageHeaderFormat, (), (const, override));
  MOCK_METHOD(std::string,
              ReadEventFormat,
              (const std::string& group, const std::string& name),
              (const, override));
};

class AllTranslationTableTest : public TestWithParam<const char*> {
 public:
  void SetUp() override {
    std::string path = base::GetTestDataPath(
        "src/traced/probes/ftrace/test/data/" + std::string(GetParam()) + "/");
    Tracefs tracefs(path);
    table_ = ProtoTranslationTable::Create(&tracefs, GetStaticEventInfo(),
                                           GetStaticCommonFieldsInfo());
    PERFETTO_CHECK(table_);
  }

  std::unique_ptr<ProtoTranslationTable> table_;
};

class TranslationTableCreationTest : public TestWithParam<uint16_t> {};

const char* kDevices[] = {
    "android_seed_N2F62_3.10.49",
    "android_hammerhead_MRA59G_3.4.0",
};

TEST_P(AllTranslationTableTest, Create) {
  EXPECT_TRUE(table_);
  EXPECT_TRUE(table_->GetEvent(GroupAndName("ftrace", "print")));
  EXPECT_TRUE(table_->GetEvent(GroupAndName("sched", "sched_switch")));
  EXPECT_TRUE(table_->GetEvent(GroupAndName("sched", "sched_wakeup")));
  EXPECT_TRUE(table_->GetEvent(GroupAndName("ext4", "ext4_da_write_begin")));
  for (const Event& event : table_->events()) {
    if (!event.ftrace_event_id)
      continue;
    EXPECT_TRUE(event.name);
    EXPECT_TRUE(event.group);
    EXPECT_TRUE(event.proto_field_id);
    for (const Field& field : event.fields) {
      EXPECT_TRUE(field.proto_field_id);
      EXPECT_TRUE(field.ftrace_type);
      EXPECT_TRUE(static_cast<int>(field.proto_field_type));
    }
  }
  ASSERT_LT(0u, table_->common_fields().size());
  const Field& pid_field = table_->common_fields().at(0);
  EXPECT_EQ(std::string(pid_field.ftrace_name), "common_pid");
  EXPECT_EQ(pid_field.proto_field_id, 2u);

  {
    auto event = table_->GetEvent(GroupAndName("ftrace", "print"));
    EXPECT_TRUE(event);
    EXPECT_EQ(std::string(event->name), "print");
    EXPECT_EQ(std::string(event->group), "ftrace");

    EXPECT_EQ(event->fields.at(0).proto_field_type, ProtoSchemaType::kString);
    EXPECT_EQ(event->fields.at(0).ftrace_type, kFtraceCString);
    EXPECT_EQ(event->fields.at(0).strategy, kCStringToString);
  }
}

INSTANTIATE_TEST_SUITE_P(ByDevice, AllTranslationTableTest, ValuesIn(kDevices));

TEST(TranslationTableTest, Seed) {
  std::string path = base::GetTestDataPath(
      "src/traced/probes/ftrace/test/data/android_seed_N2F62_3.10.49/");
  Tracefs tracefs(path);
  auto table = ProtoTranslationTable::Create(&tracefs, GetStaticEventInfo(),
                                             GetStaticCommonFieldsInfo());
  PERFETTO_CHECK(table);
  const Field& pid_field = table->common_fields().at(0);
  EXPECT_EQ(std::string(pid_field.ftrace_name), "common_pid");
  EXPECT_EQ(pid_field.proto_field_id, 2u);
  EXPECT_EQ(pid_field.ftrace_offset, 4u);
  EXPECT_EQ(pid_field.ftrace_size, 4u);

  {
    auto event = table->GetEvent(GroupAndName("sched", "sched_switch"));
    EXPECT_EQ(std::string(event->name), "sched_switch");
    EXPECT_EQ(std::string(event->group), "sched");
    EXPECT_EQ(event->ftrace_event_id, 68ul);
    EXPECT_EQ(event->fields.at(0).ftrace_offset, 8u);
    EXPECT_EQ(event->fields.at(0).ftrace_size, 16u);
  }

  {
    auto event = table->GetEvent(GroupAndName("sched", "sched_wakeup"));
    EXPECT_EQ(std::string(event->name), "sched_wakeup");
    EXPECT_EQ(std::string(event->group), "sched");
    EXPECT_EQ(event->ftrace_event_id, 70ul);
    EXPECT_EQ(event->fields.at(0).ftrace_offset, 8u);
    EXPECT_EQ(event->fields.at(0).ftrace_size, 16u);
  }

  {
    auto event = table->GetEvent(GroupAndName("ext4", "ext4_da_write_begin"));
    EXPECT_EQ(std::string(event->name), "ext4_da_write_begin");
    EXPECT_EQ(std::string(event->group), "ext4");
    EXPECT_EQ(event->ftrace_event_id, 303ul);
    EXPECT_EQ(event->fields.at(0).ftrace_offset, 8u);
    EXPECT_EQ(event->fields.at(0).ftrace_size, 4u);
  }
}

TEST_P(TranslationTableCreationTest, Create) {
  MockTracefs ftrace;
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
      field->proto_field_type = ProtoSchemaType::kString;
      field->ftrace_name = "field_a";
    }

    {
      // We shouldn't get this field: don't know how to read int -> string.
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->proto_field_id = 502;
      field->proto_field_type = ProtoSchemaType::kString;
      field->ftrace_name = "field_b";
    }

    {
      // We shouldn't get this field: no matching field in the format file.
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->proto_field_id = 503;
      field->proto_field_type = ProtoSchemaType::kString;
      field->ftrace_name = "field_c";
    }

    {
      // We should get this field.
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->proto_field_id = 504;
      field->proto_field_type = ProtoSchemaType::kUint64;
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
  PERFETTO_CHECK(table);
  EXPECT_EQ(table->EventToFtraceId(GroupAndName("group", "foo")), 42ul);
  EXPECT_EQ(table->EventToFtraceId(GroupAndName("group", "bar")), 0ul);
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

INSTANTIATE_TEST_SUITE_P(BySize, TranslationTableCreationTest, Values(4, 8));

TEST(TranslationTableTest, CompactSchedFormatParsingWalleyeData) {
  std::string path = base::GetTestDataPath(
      "src/traced/probes/ftrace/test/data/"
      "android_walleye_OPM5.171019.017.A1_4.4.88/");
  Tracefs tracefs(path);
  auto table = ProtoTranslationTable::Create(&tracefs, GetStaticEventInfo(),
                                             GetStaticCommonFieldsInfo());
  PERFETTO_CHECK(table);
  const CompactSchedEventFormat& format = table->compact_sched_format();

  // Format matches compile-time assumptions.
  ASSERT_TRUE(format.format_valid);

  // Check exact sched_switch format (note: 64 bit long prev_state).
  EXPECT_EQ(47u, format.sched_switch.event_id);
  EXPECT_EQ(64u, format.sched_switch.size);
  EXPECT_EQ(56u, format.sched_switch.next_pid_offset);
  EXPECT_EQ(FtraceFieldType::kFtracePid32, format.sched_switch.next_pid_type);
  EXPECT_EQ(60u, format.sched_switch.next_prio_offset);
  EXPECT_EQ(FtraceFieldType::kFtraceInt32, format.sched_switch.next_prio_type);
  EXPECT_EQ(32u, format.sched_switch.prev_state_offset);
  EXPECT_EQ(FtraceFieldType::kFtraceInt64, format.sched_switch.prev_state_type);
  EXPECT_EQ(40u, format.sched_switch.next_comm_offset);

  // Check exact sched_waking format.
  EXPECT_EQ(44u, format.sched_waking.event_id);
  EXPECT_EQ(40u, format.sched_waking.size);
  EXPECT_EQ(24u, format.sched_waking.pid_offset);
  EXPECT_EQ(FtraceFieldType::kFtracePid32, format.sched_waking.pid_type);
  EXPECT_EQ(36u, format.sched_waking.target_cpu_offset);
  EXPECT_EQ(FtraceFieldType::kFtraceInt32, format.sched_waking.target_cpu_type);
  EXPECT_EQ(28u, format.sched_waking.prio_offset);
  EXPECT_EQ(FtraceFieldType::kFtraceInt32, format.sched_waking.prio_type);
  EXPECT_EQ(8u, format.sched_waking.comm_offset);
}

TEST(TranslationTableTest, CompactSchedFormatParsingSeedData) {
  std::string path =
      "src/traced/probes/ftrace/test/data/android_seed_N2F62_3.10.49/";
  Tracefs tracefs(path);
  auto table = ProtoTranslationTable::Create(&tracefs, GetStaticEventInfo(),
                                             GetStaticCommonFieldsInfo());
  PERFETTO_CHECK(table);
  const CompactSchedEventFormat& format = table->compact_sched_format();

  // We consider the entire format invalid as there's no sched_waking event
  // available. This is a simplifying assumption. We could instead look at each
  // event independently (and in this case, sched_switch does match compile-time
  // assumptions).
  ASSERT_FALSE(format.format_valid);
}

TEST(TranslationTableTest, InferFtraceType) {
  FtraceFieldType type;

  ASSERT_TRUE(InferFtraceType("char foo[16]", 16, false, &type));
  EXPECT_EQ(type, kFtraceFixedCString);

  ASSERT_TRUE(InferFtraceType("char comm[TASK_COMM_LEN]", 16, false, &type));
  EXPECT_EQ(type, kFtraceFixedCString);

  ASSERT_TRUE(InferFtraceType("char identifier22[16]", 16, false, &type));
  EXPECT_EQ(type, kFtraceFixedCString);

  EXPECT_FALSE(InferFtraceType("char 2invalid[16]", 16, false, &type));

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

  ASSERT_TRUE(InferFtraceType("unsigned long args[6]", 24, true, &type));
  ASSERT_EQ(type, kFtraceUint32);
  ASSERT_TRUE(InferFtraceType("unsigned long args[6]", 48, true, &type));
  ASSERT_EQ(type, kFtraceUint64);
  ASSERT_FALSE(InferFtraceType("unsigned long args[6]", 96, true, &type));

  EXPECT_FALSE(InferFtraceType("foo", 64, false, &type));
}

TEST(TranslationTableTest, Getters) {
  MockTracefs ftrace;
  std::vector<Field> common_fields;
  std::vector<Event> events;

  {
    Event event{};
    event.name = "foo";
    event.group = "group_one";
    event.ftrace_event_id = 1;
    events.push_back(event);
  }

  {
    Event event{};
    event.name = "bar";
    event.group = "group_one";
    event.ftrace_event_id = 2;
    events.push_back(event);
  }

  {
    Event event{};
    event.name = "baz";
    event.group = "group_two";
    event.ftrace_event_id = 100;
    events.push_back(event);
  }

  ProtoTranslationTable table(
      &ftrace, events, std::move(common_fields),
      ProtoTranslationTable::DefaultPageHeaderSpecForTesting(),
      InvalidCompactSchedEventFormatForTesting(), PrintkMap());

  EXPECT_EQ(table.EventToFtraceId(GroupAndName("group_one", "foo")), 1ul);
  EXPECT_EQ(table.EventToFtraceId(GroupAndName("group_two", "baz")), 100ul);
  EXPECT_EQ(table.EventToFtraceId(GroupAndName("group_one", "no_such_event")),
            0ul);
  EXPECT_EQ(table.GetEventById(1)->name, "foo");
  EXPECT_EQ(table.GetEventById(3), nullptr);
  EXPECT_EQ(table.GetEventById(200), nullptr);
  EXPECT_EQ(table.GetEventById(0), nullptr);
  EXPECT_EQ(table.GetEvent(GroupAndName("group_one", "foo"))->ftrace_event_id,
            1u);
  EXPECT_THAT(*table.GetEventsByGroup("group_one"),
              Contains(testing::Field(&Event::name, "foo")));
  EXPECT_THAT(*table.GetEventsByGroup("group_one"),
              Contains(testing::Field(&Event::name, "bar")));
  EXPECT_THAT(*table.GetEventsByGroup("group_two"),
              Contains(testing::Field(&Event::name, "baz")));
  EXPECT_THAT(table.GetEventsByGroup("group_three"), IsNull());
}

TEST(TranslationTableTest, GenericEvent) {
  MockTracefs ftrace;
  std::vector<Field> common_fields;
  std::vector<Event> events;

  ON_CALL(ftrace, ReadPageHeaderFormat())
      .WillByDefault(Return(
          R"(	field: u64 timestamp;	offset:0;	size:8;	signed:0;
	field: local_t commit;	offset:8;	size:4;	signed:1;
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
	field:bool field_b;	offset:24;	size:1;	signed:0;
	field:int field_c;	offset:25;	size:4;	signed:1;
	field:u32 field_d;	offset:33;	size:4;	signed:0;

print fmt: "some format")"));

  EXPECT_CALL(ftrace, ReadPageHeaderFormat()).Times(AnyNumber());
  EXPECT_CALL(ftrace, ReadEventFormat(_, _)).Times(AnyNumber());

  auto table = ProtoTranslationTable::Create(&ftrace, std::move(events),
                                             std::move(common_fields));
  PERFETTO_CHECK(table);
  GroupAndName group_and_name("group", "foo");
  const Event* e = table->CreateGenericEvent(group_and_name);
  EXPECT_EQ(table->EventToFtraceId(group_and_name), 42ul);

  // Check getters
  EXPECT_TRUE(
      table->IsGenericEventProtoId(table->GetEventById(42)->proto_field_id));
  EXPECT_TRUE(table->IsGenericEventProtoId(
      table->GetEvent(group_and_name)->proto_field_id));
  EXPECT_EQ(table->GetEventsByGroup("group")->front()->name,
            group_and_name.name());

  //
  // Assert expected field descriptions.
  //

  // field:char field_a[16]; offset:8; size:16; signed:0;
  Field f1{};
  f1.ftrace_name = "field_a";
  f1.ftrace_type = kFtraceFixedCString;
  f1.ftrace_offset = 8;
  f1.ftrace_size = 16;
  f1.proto_field_id = 1;  // 1st field
  f1.proto_field_type = ProtoSchemaType::kString;
  f1.strategy = kFixedCStringToString;

  // field:bool field_b; offset:24; size:1; signed:0;
  Field f2{};
  f2.ftrace_name = "field_b";
  f2.ftrace_type = FtraceFieldType::kFtraceBool;
  f2.ftrace_offset = 24;
  f2.ftrace_size = 1;
  f2.proto_field_id = 2;  // 2nd field
  f2.proto_field_type = ProtoSchemaType::kUint64;
  f2.strategy = kBoolToUint64;

  // field:int field_c; offset:25; size:4; signed:1;
  Field f3{};
  f3.ftrace_name = "field_c";
  f3.ftrace_type = FtraceFieldType::kFtraceInt32;
  f3.ftrace_offset = 25;
  f3.ftrace_size = 4;
  f3.proto_field_id = 3;  // 3rd field
  f3.proto_field_type = ProtoSchemaType::kInt64;
  f3.strategy = kInt32ToInt64;

  // field:u32 field_d; offset:33; size:4; signed:0;
  Field f4{};
  f4.ftrace_name = "field_d";
  f4.ftrace_type = FtraceFieldType::kFtraceUint32;
  f4.ftrace_offset = 33;
  f4.ftrace_size = 4;
  f4.proto_field_id = 4;  // 4th field
  f4.proto_field_type = ProtoSchemaType::kUint64;
  f4.strategy = kUint32ToUint64;

  EXPECT_THAT(e->fields,
              ElementsAre(FtraceFieldMatcher(f1), FtraceFieldMatcher(f2),
                          FtraceFieldMatcher(f3), FtraceFieldMatcher(f4)));

  //
  // Verify the generated protobuf descriptors.
  //

  uint32_t pb_id = table->GetEventById(42)->proto_field_id;
  auto* descriptors = &table->generic_evt_pb_descriptors()->descriptors;
  ASSERT_TRUE(descriptors->Find(pb_id));

  std::vector<uint8_t> serialised_descriptor = *descriptors->Find(pb_id);
  protos::gen::FtraceEventBundle::GenericEventDescriptor outer_descriptor;
  outer_descriptor.ParseFromArray(serialised_descriptor.data(),
                                  serialised_descriptor.size());

  EXPECT_STREQ(outer_descriptor.group_name().c_str(), "group");

  protos::gen::DescriptorProto event_descriptor;
  event_descriptor.ParseFromString(outer_descriptor.event_descriptor());

  EXPECT_STREQ(event_descriptor.name().c_str(), "foo");
  const auto& fields = event_descriptor.field();
  EXPECT_EQ(fields.size(), 4u);

  using FDP = protos::gen::FieldDescriptorProto;
  EXPECT_THAT(fields,
              ElementsAre(AllOf(Property(&FDP::name, StrEq("field_a")),
                                Property(&FDP::number, Eq(1)),
                                Property(&FDP::type, Eq(FDP::TYPE_STRING))),
                          AllOf(Property(&FDP::name, StrEq("field_b")),
                                Property(&FDP::number, Eq(2)),
                                Property(&FDP::type, Eq(FDP::TYPE_UINT64))),
                          AllOf(Property(&FDP::name, StrEq("field_c")),
                                Property(&FDP::number, Eq(3)),
                                Property(&FDP::type, Eq(FDP::TYPE_INT64))),
                          AllOf(Property(&FDP::name, StrEq("field_d")),
                                Property(&FDP::number, Eq(4)),
                                Property(&FDP::type, Eq(FDP::TYPE_UINT64)))));
}

TEST(EventFilterTest, EnableEventsFrom) {
  EventFilter filter;
  filter.AddEnabledEvent(1);
  filter.AddEnabledEvent(17);

  EventFilter or_filter;
  or_filter.AddEnabledEvent(4);
  or_filter.AddEnabledEvent(17);

  filter.EnableEventsFrom(or_filter);
  EXPECT_TRUE(filter.IsEventEnabled(4));
  EXPECT_TRUE(filter.IsEventEnabled(17));
  EXPECT_TRUE(filter.IsEventEnabled(1));
  EXPECT_FALSE(filter.IsEventEnabled(2));

  EventFilter empty_filter;
  filter.EnableEventsFrom(empty_filter);
  EXPECT_TRUE(filter.IsEventEnabled(4));
  EXPECT_TRUE(filter.IsEventEnabled(17));
  EXPECT_TRUE(filter.IsEventEnabled(1));

  empty_filter.EnableEventsFrom(filter);
  EXPECT_TRUE(empty_filter.IsEventEnabled(4));
  EXPECT_TRUE(empty_filter.IsEventEnabled(17));
  EXPECT_TRUE(empty_filter.IsEventEnabled(1));
}

TEST(TranslationTableTest, FuncgraphEvents) {
  std::string path =
      base::GetTestDataPath("src/traced/probes/ftrace/test/data/synthetic/");
  Tracefs tracefs(path);
  auto table = ProtoTranslationTable::Create(&tracefs, GetStaticEventInfo(),
                                             GetStaticCommonFieldsInfo());
  PERFETTO_CHECK(table);

  {
    auto* event = table->GetEvent(GroupAndName("ftrace", "funcgraph_entry"));
    EXPECT_EQ(std::string(event->name), "funcgraph_entry");
    EXPECT_EQ(std::string(event->group), "ftrace");

    // field:unsigned long func;  offset:8;   size:8;  signed:0;
    // field:int depth;           offset:16;  size:4;  signed:1;
    ASSERT_EQ(event->fields.size(), 2u);

    // note: fields in struct are ordered as in the proto, not the format file
    EXPECT_THAT(
        event->fields,
        Contains(
            AllOf(testing::Field(&Field::ftrace_name, StrEq("func")),
                  testing::Field(&Field::ftrace_offset, Eq(8u)),
                  testing::Field(&Field::ftrace_type, kFtraceSymAddr64),
                  testing::Field(&Field::strategy, kFtraceSymAddr64ToUint64))));
  }
  {
    auto* event = table->GetEvent(GroupAndName("ftrace", "funcgraph_exit"));
    EXPECT_EQ(std::string(event->name), "funcgraph_exit");
    EXPECT_EQ(std::string(event->group), "ftrace");

    // field:unsigned long func;           offset:8;   size:8;  signed:0;
    // field:int depth;                    offset:16;  size:4;  signed:1;
    // field:unsigned int overrun;         offset:20;  size:4;  signed:0;
    // field:unsigned long long calltime;  offset:24;  size:8;  signed:0;
    // field:unsigned long long rettime;   offset:32;  size:8;  signed:0;
    ASSERT_EQ(event->fields.size(), 5u);
    // note: fields in struct are ordered as in the proto, not the format file
    EXPECT_THAT(
        event->fields,
        Contains(
            AllOf(testing::Field(&Field::ftrace_name, StrEq("func")),
                  testing::Field(&Field::ftrace_offset, Eq(8u)),
                  testing::Field(&Field::ftrace_type, kFtraceSymAddr64),
                  testing::Field(&Field::strategy, kFtraceSymAddr64ToUint64))));
  }
}

TEST(TranslationTableTest, CreateRemoveKprobeEvent) {
  NiceMock<MockTracefs> ftrace;
  ON_CALL(ftrace, ReadEventFormat(_, _)).WillByDefault(Return(""));
  ON_CALL(ftrace, ReadPageHeaderFormat())
      .WillByDefault(Return(
          R"(	field: u64 timestamp;	offset:0;	size:8;	signed:0;
	field: local_t commit;	offset:8;	size:4;	signed:1;
	field: int overwrite;	offset:8;	size:1;	signed:1;
	field: char data;	offset:16;	size:4080;	signed:0;)"));
  auto table = ProtoTranslationTable::Create(&ftrace, GetStaticEventInfo(),
                                             GetStaticCommonFieldsInfo());
  PERFETTO_CHECK(table);

  EXPECT_CALL(ftrace,
              ReadEventFormat("perfetto_kprobe", "fuse_file_write_iter"))
      .WillOnce(Return(R"format(name: fuse_file_write_iter
ID: 1535
format:
        field:unsigned short common_type;       offset:0;       size:2; signed:0;
        field:unsigned char common_flags;       offset:2;       size:1; signed:0;
        field:unsigned char common_preempt_count;       offset:3;       size:1; signed:0;
        field:int common_pid;   offset:4;       size:4; signed:1;

        field:unsigned long __probe_ip; offset:8;       size:8; signed:0;

print fmt: "(%lx)", REC->__probe_ip
)format"));
  const Event* event =
      table->CreateKprobeEvent({"perfetto_kprobe", "fuse_file_write_iter"});
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->ftrace_event_id, 1535u);
  EXPECT_EQ(table->GetEventByName("fuse_file_write_iter"), event);
  EXPECT_THAT(table->GetEventsByGroup("perfetto_kprobe"),
              Pointee(ElementsAre(event)));
  EXPECT_EQ(table->GetEventById(1535), event);

  table->RemoveEvent({"perfetto_kprobe", "fuse_file_write_iter"});
  EXPECT_EQ(table->GetEventByName("fuse_file_write_iter"), nullptr);
  EXPECT_EQ(table->GetEventsByGroup("perfetto_kprobe"), nullptr);
  EXPECT_EQ(table->GetEventById(1535), nullptr);

  EXPECT_CALL(ftrace,
              ReadEventFormat("perfetto_kprobe", "fuse_file_write_iter"))
      .WillOnce(Return(R"format(name: fuse_file_write_iter
ID: 1536
format:
        field:unsigned short common_type;       offset:0;       size:2; signed:0;
        field:unsigned char common_flags;       offset:2;       size:1; signed:0;
        field:unsigned char common_preempt_count;       offset:3;       size:1; signed:0;
        field:int common_pid;   offset:4;       size:4; signed:1;

        field:unsigned long __probe_ip; offset:8;       size:8; signed:0;

print fmt: "(%lx)", REC->__probe_ip
)format"));
  event = table->CreateKprobeEvent({"perfetto_kprobe", "fuse_file_write_iter"});
  ASSERT_NE(event, nullptr);
  EXPECT_EQ(event->ftrace_event_id, 1536u);
  EXPECT_EQ(table->GetEventByName("fuse_file_write_iter"), event);
  EXPECT_THAT(table->GetEventsByGroup("perfetto_kprobe"),
              Pointee(ElementsAre(event)));
  EXPECT_EQ(table->GetEventById(1536), event);
}

}  // namespace
}  // namespace perfetto
