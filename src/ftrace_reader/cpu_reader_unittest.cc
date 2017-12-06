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

#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/ftrace_reader/test/scattered_stream_delegate_for_testing.h"

#include "protos/ftrace/ftrace_event.pb.h"
#include "protos/ftrace/ftrace_event_bundle.pb.h"
#include "protos/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto {

namespace {

const size_t kPageSize = 4096;

std::unique_ptr<uint8_t[]> MakeBuffer(size_t size) {
  return std::unique_ptr<uint8_t[]>(new uint8_t[size]);
}

class BinaryWriter {
 public:
  BinaryWriter(uint8_t* ptr, size_t size) : ptr_(ptr), size_(size) {}

  template <typename T>
  void Write(T t) {
    memcpy(ptr_, &t, sizeof(T));
    ptr_ += sizeof(T);
    PERFETTO_CHECK(ptr_ < ptr_ + size_);
  }

  void WriteEventHeader(uint32_t time_delta, uint32_t entry_type) {
    // Entry header is a packed time delta (d) and type (t):
    // dddddddd dddddddd dddddddd dddttttt
    Write<uint32_t>((time_delta << 5) | (entry_type & 0x1f));
  }

  void WriteString(const char* s) {
    char c;
    while ((c = *s++)) {
      Write<char>(c);
    }
  }

 private:
  uint8_t* ptr_;
  size_t size_;
};

}  // namespace

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
  EXPECT_EQ(actual.timestamp, 42ul);
  EXPECT_EQ(actual.length, 999ul);
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
  EXPECT_EQ(actual.timestamp, 42ul);
  EXPECT_EQ(actual.length, 0xabababab);
  EXPECT_EQ(actual.overwrite, 0xCDu);
}

TEST(CpuReaderTest, ReadAndAdvanceOverruns) {
  uint64_t result = 42;
  uint8_t buffer[7] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  EXPECT_FALSE(CpuReader::ReadAndAdvance<uint64_t>(&ptr, ptr + 7, &result));
  EXPECT_EQ(ptr, start);
  EXPECT_EQ(result, 42ul);
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
  std::string path = "src/ftrace_reader/test/data/android_seed_N2F62_3.10.49/";
  FtraceProcfs ftrace_procfs(path);
  auto table = ProtoTranslationTable::Create(&ftrace_procfs);
  CpuReader(table.get(), 42, base::ScopedFile());
}

TEST(CpuReaderTest, ParseSimpleEvent) {
  std::string path = "src/ftrace_reader/test/data/android_seed_N2F62_3.10.49/";
  FtraceProcfs ftrace(path);
  auto table = ProtoTranslationTable::Create(&ftrace);

  std::unique_ptr<uint8_t[]> in_page = MakeBuffer(kPageSize);
  std::unique_ptr<uint8_t[]> out_page = MakeBuffer(kPageSize);

  BinaryWriter writer(in_page.get(), kPageSize);
  // Timestamp:
  writer.Write<uint64_t>(999);
  // Page length:
  writer.Write<uint64_t>(35);
  // 4 Header:
  writer.WriteEventHeader(1 /* time delta */, 8 /* entry type */);
  // 6 Event type:
  writer.Write<uint16_t>(5);
  // 7 Flags:
  writer.Write<uint8_t>(0);
  // 8 Preempt count:
  writer.Write<uint8_t>(0);
  // 12 PID:
  writer.Write<uint32_t>(72);
  // 20 Instruction pointer:
  writer.Write<uint64_t>(0);
  // 35 String:
  writer.WriteString("Hello, world!\n");

  EventFilter filter(*table, std::set<std::string>({"print"}));

  perfetto::ScatteredStreamDelegateForTesting delegate(kPageSize);
  protozero::ScatteredStreamWriter stream_writer(&delegate);
  delegate.set_writer(&stream_writer);
  protos::pbzero::FtraceEventBundle message;
  message.Reset(&stream_writer);

  CpuReader::ParsePage(42 /* cpu number */, in_page.get(), kPageSize, &filter,
                       &message, table.get());

  size_t msg_size =
      delegate.chunks().size() * kPageSize - stream_writer.bytes_available();
  std::unique_ptr<uint8_t[]> proto = delegate.StitchChunks(msg_size);

  protos::FtraceEventBundle proto_bundle;
  proto_bundle.ParseFromArray(proto.get(), static_cast<int>(msg_size));

  EXPECT_EQ(proto_bundle.cpu(), 42u);
  ASSERT_EQ(proto_bundle.event().size(), 1);
  const protos::FtraceEvent& proto_event = proto_bundle.event().Get(0);
  EXPECT_EQ(proto_event.pid(), 72u);
  EXPECT_TRUE(proto_event.has_print());
  // TODO(hjd): Check if this is the correct format.
  EXPECT_EQ(proto_event.print().buf(), "Hello, world!\n");
}

}  // namespace perfetto
