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
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "proto_translation_table.h"

#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/ftrace_reader/test/scattered_stream_delegate_for_testing.h"

#include "protos/ftrace/ftrace_event.pb.h"
#include "protos/ftrace/ftrace_event.pbzero.h"
#include "protos/ftrace/ftrace_event_bundle.pb.h"
#include "protos/ftrace/ftrace_event_bundle.pbzero.h"
#include "src/ftrace_reader/test/test_messages.pb.h"
#include "src/ftrace_reader/test/test_messages.pbzero.h"

using testing::ElementsAreArray;

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
// protozero ftrace bundle writer and a method ParseProto() to attempt to
// parse whatever has been written so far into a proto message.
template <class ZeroT, class ProtoT>
class ProtoProvider {
 public:
  explicit ProtoProvider(size_t chunk_size)
      : chunk_size_(chunk_size), delegate_(chunk_size_), stream_(&delegate_) {
    delegate_.set_writer(&stream_);
    writer_.Reset(&stream_);
  }
  ~ProtoProvider() = default;

  ZeroT* writer() { return &writer_; }

  // Stitch together the scattered chunks into a single buffer then attempt
  // to parse the buffer as a FtraceEventBundle. Returns the FtraceEventBundle
  // on success and nullptr on failure.
  std::unique_ptr<ProtoT> ParseProto() {
    auto bundle = std::unique_ptr<ProtoT>(new ProtoT());
    size_t msg_size =
        delegate_.chunks().size() * chunk_size_ - stream_.bytes_available();
    std::unique_ptr<uint8_t[]> buffer = delegate_.StitchChunks(msg_size);
    if (!bundle->ParseFromArray(buffer.get(), static_cast<int>(msg_size)))
      return nullptr;
    return bundle;
  }

 private:
  ProtoProvider(const ProtoProvider&) = delete;
  ProtoProvider& operator=(const ProtoProvider&) = delete;

  size_t chunk_size_;
  perfetto::ScatteredStreamDelegateForTesting delegate_;
  protozero::ScatteredStreamWriter stream_;
  ZeroT writer_;
};

using BundleProvider =
    ProtoProvider<protos::pbzero::FtraceEventBundle, protos::FtraceEventBundle>;

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
    auto table = ProtoTranslationTable::Create(&ftrace, GetStaticEventInfo(),
                                               GetStaticCommonFieldsInfo());
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

class BinaryWriter {
 public:
  BinaryWriter()
      : size_(kPageSize), page_(new uint8_t[size_]), ptr_(page_.get()) {}

  template <typename T>
  void Write(T t) {
    memcpy(ptr_, &t, sizeof(T));
    ptr_ += sizeof(T);
    PERFETTO_CHECK(ptr_ < ptr_ + size_);
  }

  void WriteString(const char* s) {
    char c;
    while ((c = *s++)) {
      Write<char>(c);
    }
  }

  void WriteFixedString(size_t n, const char* s) {
    size_t length = strlen(s);
    PERFETTO_CHECK(length < n);
    char c;
    while ((c = *s++)) {
      Write<char>(c);
    }
    Write<char>('\0');
    for (size_t i = 0; i < n - length - 1; i++) {
      Write<char>('\xff');
    }
  }

  std::unique_ptr<uint8_t[]> GetCopy() {
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[written()]);
    memcpy(buffer.get(), page_.get(), written());
    return buffer;
  }

  size_t written() { return ptr_ - page_.get(); }

 private:
  size_t size_;
  std::unique_ptr<uint8_t[]> page_;
  uint8_t* ptr_;
};

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

TEST(CpuReaderTest, BinaryWriter) {
  BinaryWriter writer;
  writer.Write<uint64_t>(1);
  writer.Write<uint32_t>(2);
  writer.Write<uint16_t>(3);
  writer.Write<uint8_t>(4);
  auto buffer = writer.GetCopy();
  EXPECT_EQ(buffer.get()[0], 1);
  EXPECT_EQ(buffer.get()[1], 0);
  EXPECT_EQ(buffer.get()[2], 0);
  EXPECT_EQ(buffer.get()[3], 0);
  EXPECT_EQ(buffer.get()[4], 0);
  EXPECT_EQ(buffer.get()[5], 0);
  EXPECT_EQ(buffer.get()[6], 0);
  EXPECT_EQ(buffer.get()[7], 0);
  EXPECT_EQ(buffer.get()[8], 2);
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

  size_t bytes = CpuReader::ParsePage(42 /* cpu number */, page.get(), &filter,
                                      bundle_provider.writer(), table);
  EXPECT_EQ(bytes, 60ul);

  auto bundle = bundle_provider.ParseProto();
  ASSERT_TRUE(bundle);
  EXPECT_EQ(bundle->cpu(), 42ul);
  ASSERT_EQ(bundle->event().size(), 1);
  const protos::FtraceEvent& event = bundle->event().Get(0);
  EXPECT_EQ(event.pid(), 28712ul);
  EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 608934, 535199));
  EXPECT_EQ(event.print().buf(), "Hello, world!\n");
}

TEST(CpuReaderTest, FilterByEvent) {
  const ExamplePage* test_case = &g_single_print;

  BundleProvider bundle_provider(kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, std::set<std::string>());

  ASSERT_TRUE(CpuReader::ParsePage(42 /* cpu number */, page.get(), &filter,
                                   bundle_provider.writer(), table));

  auto bundle = bundle_provider.ParseProto();
  ASSERT_TRUE(bundle);
  EXPECT_EQ(bundle->cpu(), 42ul);
  ASSERT_EQ(bundle->event().size(), 0);
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

  ASSERT_TRUE(CpuReader::ParsePage(42 /* cpu number */, page.get(), &filter,
                                   bundle_provider.writer(), table));

  auto bundle = bundle_provider.ParseProto();
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

// # tracer: nop
// #
// # entries-in-buffer/entries-written: 6/6   #P:8
// #
// #                              _-----=> irqs-off
// #                             / _----=> need-resched
// #                            | / _---=> hardirq/softirq
// #                            || / _--=> preempt-depth
// #                            ||| /     delay
// #           TASK-PID   CPU#  ||||    TIMESTAMP  FUNCTION
// #              | |       |   ||||       |         |
//      ksoftirqd/0-3     [000] d..3 1045157.722134: sched_switch:
//      prev_comm=ksoftirqd/0 prev_pid=3 prev_prio=120 prev_state=S ==>
//      next_comm=sleep next_pid=3733 next_prio=120
//            sleep-3733  [000] d..3 1045157.725035: sched_switch:
//            prev_comm=sleep prev_pid=3733 prev_prio=120 prev_state=R+ ==>
//            next_comm=rcuop/0 next_pid=10 next_prio=120
//      rcu_preempt-7     [000] d..3 1045157.725182: sched_switch:
//      prev_comm=rcu_preempt prev_pid=7 prev_prio=120 prev_state=S ==>
//      next_comm=sleep next_pid=3733 next_prio=120
//            sleep-3733  [000] d..3 1045157.725671: sched_switch:
//            prev_comm=sleep prev_pid=3733 prev_prio=120 prev_state=R+ ==>
//            next_comm=sh next_pid=3513 next_prio=120
//               sh-3513  [000] d..3 1045157.726668: sched_switch: prev_comm=sh
//               prev_pid=3513 prev_prio=120 prev_state=S ==> next_comm=sleep
//               next_pid=3733 next_prio=120
//            sleep-3733  [000] d..3 1045157.726697: sched_switch:
//            prev_comm=sleep prev_pid=3733 prev_prio=120 prev_state=x ==>
//            next_comm=kworker/u16:3 next_pid=3681 next_prio=120
ExamplePage g_six_sched_switch{
    "synthetic",
    R"(
    00000000: 2b16 c3be 90b6 0300 a001 0000 0000 0000  +...............
    00000010: 1e00 0000 0000 0000 1000 0000 2f00 0103  ............/...
    00000020: 0300 0000 6b73 6f66 7469 7271 642f 3000  ....ksoftirqd/0.
    00000030: 0000 0000 0300 0000 7800 0000 0100 0000  ........x.......
    00000040: 0000 0000 736c 6565 7000 722f 3000 0000  ....sleep.r/0...
    00000050: 0000 0000 950e 0000 7800 0000 b072 8805  ........x....r..
    00000060: 2f00 0103 950e 0000 736c 6565 7000 722f  /.......sleep.r/
    00000070: 3000 0000 0000 0000 950e 0000 7800 0000  0...........x...
    00000080: 0008 0000 0000 0000 7263 756f 702f 3000  ........rcuop/0.
    00000090: 0000 0000 0000 0000 0a00 0000 7800 0000  ............x...
    000000a0: f0b0 4700 2f00 0103 0700 0000 7263 755f  ..G./.......rcu_
    000000b0: 7072 6565 6d70 7400 0000 0000 0700 0000  preempt.........
    000000c0: 7800 0000 0100 0000 0000 0000 736c 6565  x...........slee
    000000d0: 7000 722f 3000 0000 0000 0000 950e 0000  p.r/0...........
    000000e0: 7800 0000 1001 ef00 2f00 0103 950e 0000  x......./.......
    000000f0: 736c 6565 7000 722f 3000 0000 0000 0000  sleep.r/0.......
    00000100: 950e 0000 7800 0000 0008 0000 0000 0000  ....x...........
    00000110: 7368 0064 0065 722f 3000 0000 0000 0000  sh.d.er/0.......
    00000120: b90d 0000 7800 0000 f0c7 e601 2f00 0103  ....x......./...
    00000130: b90d 0000 7368 0064 0065 722f 3000 0000  ....sh.d.er/0...
    00000140: 0000 0000 b90d 0000 7800 0000 0100 0000  ........x.......
    00000150: 0000 0000 736c 6565 7000 722f 3000 0000  ....sleep.r/0...
    00000160: 0000 0000 950e 0000 7800 0000 d030 0e00  ........x....0..
    00000170: 2f00 0103 950e 0000 736c 6565 7000 722f  /.......sleep.r/
    00000180: 3000 0000 0000 0000 950e 0000 7800 0000  0...........x...
    00000190: 4000 0000 0000 0000 6b77 6f72 6b65 722f  @.......kworker/
    000001a0: 7531 363a 3300 0000 610e 0000 7800 0000  u16:3...a...x...
    000001b0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
    )",
};

TEST(CpuReaderTest, ParseSixSchedSwitch) {
  const ExamplePage* test_case = &g_six_sched_switch;

  BundleProvider bundle_provider(kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, std::set<std::string>({"sched_switch"}));

  ASSERT_TRUE(CpuReader::ParsePage(42 /* cpu number */, page.get(), &filter,
                                   bundle_provider.writer(), table));

  auto bundle = bundle_provider.ParseProto();
  ASSERT_TRUE(bundle);
  EXPECT_EQ(bundle->cpu(), 42ul);
  ASSERT_EQ(bundle->event().size(), 6);

  {
    const protos::FtraceEvent& event = bundle->event().Get(1);
    EXPECT_EQ(event.pid(), 3733ul);
    EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 1045157, 725035));
    EXPECT_EQ(event.sched_switch().prev_comm(), "sleep");
    EXPECT_EQ(event.sched_switch().prev_pid(), 3733);
    EXPECT_EQ(event.sched_switch().prev_prio(), 120);
    EXPECT_EQ(event.sched_switch().next_comm(), "rcuop/0");
    EXPECT_EQ(event.sched_switch().next_pid(), 10);
    EXPECT_EQ(event.sched_switch().next_prio(), 120);
  }
}

TEST(CpuReaderTest, ParseAllFields) {
  using FakeEventProvider =
      ProtoProvider<pbzero::FakeFtraceEvent, FakeFtraceEvent>;

  uint16_t ftrace_event_id = 102;

  std::vector<Field> common_fields;
  {
    common_fields.emplace_back(Field{});
    Field* field = &common_fields.back();
    field->ftrace_offset = 0;
    field->ftrace_size = 4;
    field->ftrace_type = kFtraceUint32;
    field->proto_field_id = 1;
    field->proto_field_type = kProtoUint32;
    SetTranslationStrategy(field->ftrace_type, field->proto_field_type,
                           &field->strategy);
  }

  std::vector<Event> events;
  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "";
    event->group = "";
    event->proto_field_id = 42;
    event->ftrace_event_id = ftrace_event_id;
    {
      // uint32 -> uint32
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 8;
      field->ftrace_size = 4;
      field->ftrace_type = kFtraceUint32;
      field->proto_field_id = 1;
      field->proto_field_type = kProtoUint32;
      SetTranslationStrategy(field->ftrace_type, field->proto_field_type,
                             &field->strategy);
    }
    {
      // char[16] -> string
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 12;
      field->ftrace_size = 16;
      field->ftrace_type = kFtraceFixedCString;
      field->proto_field_id = 500;
      field->proto_field_type = kProtoString;
      SetTranslationStrategy(field->ftrace_type, field->proto_field_type,
                             &field->strategy);
    }
    {
      // char -> string
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 28;
      field->ftrace_size = 0;
      field->ftrace_type = kFtraceCString;
      field->proto_field_id = 501;
      field->proto_field_type = kProtoString;
      SetTranslationStrategy(field->ftrace_type, field->proto_field_type,
                             &field->strategy);
    }
  }
  ProtoTranslationTable table(events, std::move(common_fields));

  FakeEventProvider provider(kPageSize);

  BinaryWriter writer;
  writer.Write<int32_t>(1001);  // Common field.
  writer.Write<int32_t>(9999);  // A gap we shouldn't read.
  writer.Write<int32_t>(1002);
  writer.WriteFixedString(16, "Hello");
  writer.WriteFixedString(300, "Goodbye");

  auto input = writer.GetCopy();
  auto length = writer.written();

  ASSERT_TRUE(CpuReader::ParseEvent(ftrace_event_id, input.get(),
                                    input.get() + length, &table,
                                    provider.writer()));

  auto event = provider.ParseProto();
  ASSERT_TRUE(event);
  EXPECT_EQ(event->common_field(), 1001ul);
  EXPECT_EQ(event->event_case(), FakeFtraceEvent::kAllFields);
  EXPECT_EQ(event->all_fields().field_uint32(), 1002ul);
  EXPECT_EQ(event->all_fields().field_char_16(), "Hello");
  EXPECT_EQ(event->all_fields().field_char(), "Goodbye");
}

}  // namespace perfetto
