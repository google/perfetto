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
#include "gtest/gtest.h"

using testing::ValuesIn;
using testing::TestWithParam;

namespace perfetto {
namespace {

class AllTranslationTableTest : public TestWithParam<const char*> {
 public:
  void SetUp() override {
    std::string path =
        "src/ftrace_reader/test/data/" + std::string(GetParam()) + "/";
    FtraceProcfs ftrace_procfs(path);
    table_ =
        ProtoTranslationTable::Create(&ftrace_procfs, GetStaticEventInfo());
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
}

INSTANTIATE_TEST_CASE_P(ByDevice, AllTranslationTableTest, ValuesIn(kDevices));

TEST(TranslationTable, Seed) {
  std::string path = "src/ftrace_reader/test/data/android_seed_N2F62_3.10.49/";
  FtraceProcfs ftrace_procfs(path);
  auto table =
      ProtoTranslationTable::Create(&ftrace_procfs, GetStaticEventInfo());
  EXPECT_EQ(table->common_fields().at(0).ftrace_offset, 0u);
  EXPECT_EQ(table->common_fields().at(0).ftrace_size, 2u);

  auto sched_switch_event = table->GetEventByName("sched_switch");
  EXPECT_EQ(std::string(sched_switch_event->name), "sched_switch");
  EXPECT_EQ(std::string(sched_switch_event->group), "sched");
  EXPECT_EQ(sched_switch_event->ftrace_event_id, 68ul);
  EXPECT_EQ(sched_switch_event->fields.at(0).ftrace_offset, 8u);
  EXPECT_EQ(sched_switch_event->fields.at(0).ftrace_size, 16u);
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
