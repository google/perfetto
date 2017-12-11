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

#include "event_info.h"
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
const uint64_t kNanoInSecond = 1000 * 1000 * 1000;
const uint64_t kNanoInMicro = 1000;

::testing::AssertionResult WithinOneMicrosecond(uint64_t actual_ns,
                                                uint64_t expected_s,
                                                uint64_t expected_us) {
  // Round to closest us.
  uint64_t actual_us = (actual_ns + kNanoInMicro / 2) / kNanoInMicro;
  uint64_t total_expected_us = expected_s * 1000 * 1000 + expected_us;
  if (actual_us == total_expected_us) {
    return ::testing::AssertionSuccess();
  } else {
    return ::testing::AssertionFailure()
           << actual_ns / kNanoInSecond << "."
           << (actual_ns % kNanoInSecond) / kNanoInMicro << " vs. "
           << expected_s << "." << expected_us;
  }
}

struct ExamplePage {
  // The name of the format file set used in the collection of this example
  // page. Should name a directory under src/ftrace_reader/test/data
  const char* name;
  // The non-zero prefix of xxd'ing the page.
  const char* data;
};

// Single class to manage the whole protozero -> scattered stream -> chunks ->
// single buffer -> real proto dance. Has a method: writer() to get an
// protozero ftrace bundle writer and a method GetBundle() to attempt to
// parse whatever has been written so far into a proto message.
class BundleProvider {
 public:
  explicit BundleProvider(size_t chunk_size)
      : chunk_size_(chunk_size), delegate_(chunk_size_), stream_(&delegate_) {
    delegate_.set_writer(&stream_);
    writer_.Reset(&stream_);
  }
  ~BundleProvider() = default;

  protos::pbzero::FtraceEventBundle* writer() { return &writer_; }

  // Stitch together the scattered chunks into a single buffer then attempt
  // to parse the buffer as a FtraceEventBundle. Returns the FtraceEventBundle
  // on success and nullptr on failure.
  std::unique_ptr<protos::FtraceEventBundle> GetBundle() {
    auto bundle = std::unique_ptr<protos::FtraceEventBundle>(
        new protos::FtraceEventBundle());
    size_t msg_size =
        delegate_.chunks().size() * chunk_size_ - stream_.bytes_available();
    std::unique_ptr<uint8_t[]> buffer = delegate_.StitchChunks(msg_size);
    if (!bundle->ParseFromArray(buffer.get(), static_cast<int>(msg_size)))
      return nullptr;
    return bundle;
  }

 private:
  BundleProvider(const BundleProvider&) = delete;
  BundleProvider& operator=(const BundleProvider&) = delete;

  size_t chunk_size_;
  perfetto::ScatteredStreamDelegateForTesting delegate_;
  protozero::ScatteredStreamWriter stream_;
  protos::pbzero::FtraceEventBundle writer_;
};

// Create a ProtoTranslationTable uing the fomat files in
// directory |name|. Caches the table for subsequent lookups.
std::map<std::string, std::unique_ptr<ProtoTranslationTable>>* g_tables;
ProtoTranslationTable* GetTable(const std::string& name) {
  if (!g_tables)
    g_tables =
        new std::map<std::string, std::unique_ptr<ProtoTranslationTable>>();
  if (!g_tables->count(name)) {
    std::string path = "src/ftrace_reader/test/data/" + name + "/";
    FtraceProcfs ftrace(path);
    auto table = ProtoTranslationTable::Create(&ftrace, GetStaticEventInfo());
    g_tables->emplace(name, std::move(table));
  }
  return g_tables->at(name).get();
}

// Convert xxd output into binary data.
std::unique_ptr<uint8_t[]> PageFromXxd(const std::string& text) {
  auto buffer = std::unique_ptr<uint8_t[]>(new uint8_t[kPageSize]);
  const char* ptr = text.data();
  memset(buffer.get(), 0xfa, kPageSize);
  uint8_t* out = buffer.get();
  while (*ptr != '\0') {
    if (*(ptr++) != ':')
      continue;
    for (int i = 0; i < 8; i++) {
      PERFETTO_CHECK(text.size() >=
                     static_cast<size_t>((ptr - text.data()) + 5));
      PERFETTO_CHECK(*(ptr++) == ' ');
      int n = sscanf(ptr, "%02hhx%02hhx", out, out + 1);
      PERFETTO_CHECK(n == 2);
      out += n;
      ptr += 4;
    }
    while (*ptr != '\n')
      ptr++;
  }
  return buffer;
}

}  // namespace

TEST(PageFromXxdTest, OneLine) {
  std::string text = R"(
    00000000: 0000 0000 0000 0000 0000 0000 0000 0000  ................
    00000000: 0000 0000 5600 0000 0000 0000 0000 0000  ................
  )";
  auto page = PageFromXxd(text);
  EXPECT_EQ(page.get()[0x14], 0x56);
}

TEST(PageFromXxdTest, ManyLines) {
  std::string text = R"(
    00000000: 1234 0000 0000 0000 0000 0000 0000 0056  ................
    00000010: 7800 0000 0000 0000 0000 0000 0000 009a  ................
    00000020: 0000 0000 bc00 0000 00de 0000 0000 009a  ................
  )";
  auto page = PageFromXxd(text);
  EXPECT_EQ(page.get()[0x00], 0x12);
  EXPECT_EQ(page.get()[0x01], 0x34);
  EXPECT_EQ(page.get()[0x0f], 0x56);
  EXPECT_EQ(page.get()[0x10], 0x78);
  EXPECT_EQ(page.get()[0x1f], 0x9a);
  EXPECT_EQ(page.get()[0x24], 0xbc);
  EXPECT_EQ(page.get()[0x29], 0xde);
}

TEST(EventFilterTest, EventFilter) {
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

TEST(ReadAndAdvanceTest, Number) {
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

TEST(ReadAndAdvanceTest, PlainStruct) {
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

TEST(ReadAndAdvanceTest, ComplexStruct) {
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

TEST(ReadAndAdvanceTest, Overruns) {
  uint64_t result = 42;
  uint8_t buffer[7] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  EXPECT_FALSE(CpuReader::ReadAndAdvance<uint64_t>(&ptr, ptr + 7, &result));
  EXPECT_EQ(ptr, start);
  EXPECT_EQ(result, 42ul);
}

TEST(ReadAndAdvanceTest, AtEnd) {
  uint8_t result = 42;
  uint8_t buffer[8] = {};
  const uint8_t* start = buffer;
  const uint8_t* ptr = buffer;
  EXPECT_FALSE(CpuReader::ReadAndAdvance<uint8_t>(&ptr, ptr, &result));
  EXPECT_EQ(ptr, start);
  EXPECT_EQ(result, 42);
}

TEST(ReadAndAdvanceTest, Underruns) {
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

// # tracer: nop
// #
// # entries-in-buffer/entries-written: 1/1   #P:8
// #
// #                              _-----=> irqs-off
// #                             / _----=> need-resched
// #                            | / _---=> hardirq/softirq
// #                            || / _--=> preempt-depth
// #                            ||| /     delay
// #           TASK-PID   CPU#  ||||    TIMESTAMP  FUNCTION
// #              | |       |   ||||       |         |
//               sh-28712 [000] ...1 608934.535199: tracing_mark_write: Hello, world!
ExamplePage g_single_print{
    "synthetic",
    R"(
    00000000: ba12 6a33 c628 0200 2c00 0000 0000 0000  ..j3.(..,.......
    00000010: def0 ec67 8d21 0000 0800 0000 0500 0001  ...g.!..........
    00000020: 2870 0000 ac5d 1661 86ff ffff 4865 6c6c  (p...].a....Hell
    00000030: 6f2c 2077 6f72 6c64 210a 00ff 0000 0000  o, world!.......
  )",
};

TEST(CpuReaderTest, ParseSinglePrint) {
  const ExamplePage* test_case = &g_single_print;

  BundleProvider bundle_provider(kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, std::set<std::string>({"print"}));

  CpuReader::ParsePage(42 /* cpu number */, page.get(), &filter,
                       bundle_provider.writer(), table);

  auto bundle = bundle_provider.GetBundle();
  ASSERT_TRUE(bundle);
  EXPECT_EQ(bundle->cpu(), 42ul);
  ASSERT_EQ(bundle->event().size(), 1);
  const protos::FtraceEvent& event = bundle->event().Get(0);
  EXPECT_EQ(event.pid(), 28712ul);
  EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 608934, 535199));
  EXPECT_EQ(event.print().buf(), "Hello, world!\n");
}

// # tracer: nop
// #
// # entries-in-buffer/entries-written: 3/3   #P:8
// #
// #                              _-----=> irqs-off
// #                             / _----=> need-resched
// #                            | / _---=> hardirq/softirq
// #                            || / _--=> preempt-depth
// #                            ||| /     delay
// #           TASK-PID   CPU#  ||||    TIMESTAMP  FUNCTION
// #              | |       |   ||||       |         |
//               sh-30693 [000] ...1 615436.216806: tracing_mark_write: Hello, world!
//               sh-30693 [000] ...1 615486.377232: tracing_mark_write: Good afternoon, world!
//               sh-30693 [000] ...1 615495.632679: tracing_mark_write: Goodbye, world!
ExamplePage g_three_prints{
    "synthetic",
    R"(
    00000000: a3ab 1569 bc2f 0200 9400 0000 0000 0000  ...i./..........
    00000010: 1e00 0000 0000 0000 0800 0000 0500 0001  ................
    00000020: e577 0000 ac5d 1661 86ff ffff 4865 6c6c  .w...].a....Hell
    00000030: 6f2c 2077 6f72 6c64 210a 0000 5e32 6bb9  o, world!...^2k.
    00000040: 7501 0000 0b00 0000 0500 0001 e577 0000  u............w..
    00000050: ac5d 1661 86ff ffff 476f 6f64 2061 6674  .].a....Good aft
    00000060: 6572 6e6f 6f6e 2c20 776f 726c 6421 0a00  ernoon, world!..
    00000070: 0000 0000 9e6a 5df5 4400 0000 0900 0000  .....j].D.......
    00000080: 0500 0001 e577 0000 ac5d 1661 86ff ffff  .....w...].a....
    00000090: 476f 6f64 6279 652c 2077 6f72 6c64 210a  Goodbye, world!.
    000000a0: 0051 0000 0000 0000 0000 0000 0000 0000  .Q..............
  )",
};

TEST(CpuReaderTest, ParseThreePrint) {
  const ExamplePage* test_case = &g_three_prints;

  BundleProvider bundle_provider(kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, std::set<std::string>({"print"}));

  CpuReader::ParsePage(42 /* cpu number */, page.get(), &filter,
                       bundle_provider.writer(), table);

  auto bundle = bundle_provider.GetBundle();
  ASSERT_TRUE(bundle);
  EXPECT_EQ(bundle->cpu(), 42ul);
  ASSERT_EQ(bundle->event().size(), 3);

  {
    const protos::FtraceEvent& event = bundle->event().Get(0);
    EXPECT_EQ(event.pid(), 30693ul);
    EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 615436, 216806));
    EXPECT_EQ(event.print().buf(), "Hello, world!\n");
  }

  {
    const protos::FtraceEvent& event = bundle->event().Get(1);
    EXPECT_EQ(event.pid(), 30693ul);
    EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 615486, 377232));
    EXPECT_EQ(event.print().buf(), "Good afternoon, world!\n");
  }

  {
    const protos::FtraceEvent& event = bundle->event().Get(2);
    EXPECT_EQ(event.pid(), 30693ul);
    EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 615495, 632679));
    EXPECT_EQ(event.print().buf(), "Goodbye, world!\n");
  }
}

}  // namespace perfetto
