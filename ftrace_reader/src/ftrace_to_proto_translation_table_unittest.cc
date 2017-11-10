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

#include "ftrace_to_proto_translation_table.h"

#include "gtest/gtest.h"

using testing::ValuesIn;
using testing::TestWithParam;

namespace perfetto {
namespace {

class AllTranslationTableTest : public TestWithParam<const char*> {
 public:
  void SetUp() override {
    std::string path =
        "ftrace_reader/test/data/" + std::string(GetParam()) + "/";
    table_ = FtraceToProtoTranslationTable::Create(path);
  }

  std::unique_ptr<FtraceToProtoTranslationTable> table_;
};

const char* kDevices[] = {"android_seed_N2F62_3.10.49",
                          "android_hammerhead_MRA59G_3.4.0"};

TEST_P(AllTranslationTableTest, Create) {
  EXPECT_TRUE(table_);
}

INSTANTIATE_TEST_CASE_P(ByDevice, AllTranslationTableTest, ValuesIn(kDevices));

TEST(TranslationTable, Seed) {
  std::string path = "ftrace_reader/test/data/android_seed_N2F62_3.10.49/";
  auto table = FtraceToProtoTranslationTable::Create(path);
  auto sched_switch_event = table->events().at(68);
  EXPECT_EQ(sched_switch_event.name, "sched_switch");
  EXPECT_EQ(sched_switch_event.group, "sched");
  EXPECT_EQ(sched_switch_event.ftrace_event_id, 68);
  EXPECT_EQ(sched_switch_event.fields.at(0).ftrace_offset, 8u);
  EXPECT_EQ(sched_switch_event.fields.at(0).ftrace_size, 16u);
}

}  // namespace
}  // namespace perfetto
