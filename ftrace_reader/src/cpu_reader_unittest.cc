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

#include "cpu_reader.h"

#include "ftrace_procfs.h"
#include "gtest/gtest.h"
#include "proto_translation_table.h"

namespace perfetto {

TEST(EventFilterTest, EventFilter) {
  using Event = ProtoTranslationTable::Event;
  using Field = ProtoTranslationTable::Field;

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
    event.ftrace_event_id = 10;
    events.push_back(event);
  }

  ProtoTranslationTable table(events, std::move(common_fields));
  EventFilter filter(table, std::set<std::string>({"foo"}));

  EXPECT_TRUE(filter.IsEventEnabled(1));
  EXPECT_FALSE(filter.IsEventEnabled(2));
  EXPECT_FALSE(filter.IsEventEnabled(10));
}

TEST(CpuReaderTest, ReadAndAdvanceNumber) {
  uint64_t expected = 42;
  uint64_t actual = 0;
  uint8_t buffer[8] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  memcpy(&buffer, &expected, 8);
  EXPECT_TRUE(CpuReader::ReadAndAdvance<uint64_t>(&ptr, ptr + 8, &actual));
  EXPECT_EQ(ptr, start + 8);
  EXPECT_EQ(actual, expected);
}

TEST(CpuReaderTest, ReadAndAdvancePlainStruct) {
  struct PlainStruct {
    uint64_t timestamp;
    uint64_t length;
  };

  uint64_t expected[2] = {42, 999};
  PlainStruct actual;
  uint8_t buffer[16] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  memcpy(&buffer, &expected, 16);
  EXPECT_TRUE(CpuReader::ReadAndAdvance<PlainStruct>(&ptr, ptr + 16, &actual));
  EXPECT_EQ(ptr, start + 16);
  EXPECT_EQ(actual.timestamp, 42);
  EXPECT_EQ(actual.length, 999);
}

TEST(CpuReaderTest, ReadAndAdvanceComplexStruct) {
  struct ComplexStruct {
    uint64_t timestamp;
    uint32_t length;
    uint32_t : 24;
    uint32_t overwrite : 8;
  };

  uint64_t expected[2] = {42, 0xcdffffffabababab};
  ComplexStruct actual = {};
  uint8_t buffer[16] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  memcpy(&buffer, &expected, 16);
  EXPECT_TRUE(
      CpuReader::ReadAndAdvance<ComplexStruct>(&ptr, ptr + 16, &actual));
  EXPECT_EQ(ptr, start + 16);
  EXPECT_EQ(actual.timestamp, 42);
  EXPECT_EQ(actual.length, 0xabababab);
  EXPECT_EQ(actual.overwrite, 0xcd);
}

TEST(CpuReaderTest, ReadAndAdvanceOverruns) {
  uint64_t result = 42;
  uint8_t buffer[7] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  EXPECT_FALSE(CpuReader::ReadAndAdvance<uint64_t>(&ptr, ptr + 7, &result));
  EXPECT_EQ(ptr, start);
  EXPECT_EQ(result, 42);
}

TEST(CpuReaderTest, ReadAndAdvanceAtEnd) {
  uint8_t result = 42;
  uint8_t buffer[8] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  EXPECT_FALSE(CpuReader::ReadAndAdvance<uint8_t>(&ptr, ptr, &result));
  EXPECT_EQ(ptr, start);
  EXPECT_EQ(result, 42);
}

TEST(CpuReaderTest, ReadAndAdvanceUnderruns) {
  uint64_t expected = 42;
  uint64_t actual = 0;
  uint8_t buffer[9] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  memcpy(&buffer, &expected, 8);
  EXPECT_TRUE(CpuReader::ReadAndAdvance<uint64_t>(&ptr, ptr + 8, &actual));
  EXPECT_EQ(ptr, start + 8);
  EXPECT_EQ(actual, expected);
}

TEST(CpuReaderTest, ParseEmpty) {
  std::string path = "ftrace_reader/test/data/android_seed_N2F62_3.10.49/";
  FtraceProcfs ftrace_procfs(path);
  auto table = ProtoTranslationTable::Create(path, &ftrace_procfs);
  CpuReader(table.get(), 42, base::ScopedFile());
}

}  // namespace perfetto
