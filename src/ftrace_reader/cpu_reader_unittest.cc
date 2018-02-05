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
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "proto_translation_table.h"

#include "perfetto/base/utils.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/ftrace_reader/test/scattered_stream_delegate_for_testing.h"

#include "perfetto/trace/ftrace/ftrace_event.pb.h"
#include "perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_event_bundle.pb.h"
#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "src/ftrace_reader/test/cpu_reader_support.h"
#include "src/ftrace_reader/test/test_messages.pb.h"
#include "src/ftrace_reader/test/test_messages.pbzero.h"

using testing::ElementsAreArray;

namespace perfetto {

namespace {

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
  ScatteredStreamDelegateForTesting delegate_;
  protozero::ScatteredStreamWriter stream_;
  ZeroT writer_;
};

using BundleProvider =
    ProtoProvider<protos::pbzero::FtraceEventBundle, protos::FtraceEventBundle>;

class BinaryWriter {
 public:
  BinaryWriter()
      : size_(base::kPageSize), page_(new uint8_t[size_]), ptr_(page_.get()) {}

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
  EventFilter filter(table, {"foo"});

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

  BundleProvider bundle_provider(base::kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, {"print"});

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

// This event is as the event for ParseSinglePrint above except the string
// is extended to overflow the page size written in the header.
ExamplePage g_single_print_malformed{
    "synthetic",
    R"(
    00000000: ba12 6a33 c628 0200 2c00 0000 0000 0000  ................
    00000010: def0 ec67 8d21 0000 0800 0000 0500 0001  ................
    00000020: 2870 0000 ac5d 1661 86ff ffff 4865 6c6c  ................
    00000030: 6f2c 2077 6f72 6c64 2120 776f 726c 6421  ................
    00000040: 0a00 ff00 0000 0000 0000 0000 0000 0000  ................
  )",
};

TEST(CpuReaderTest, ParseSinglePrintMalformed) {
  const ExamplePage* test_case = &g_single_print_malformed;

  BundleProvider bundle_provider(base::kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, {"print"});

  ASSERT_FALSE(CpuReader::ParsePage(42 /* cpu number */, page.get(), &filter,
                                    bundle_provider.writer(), table));

  auto bundle = bundle_provider.ParseProto();
  ASSERT_TRUE(bundle);
  EXPECT_EQ(bundle->cpu(), 42ul);
  ASSERT_EQ(bundle->event().size(), 1);
  // Although one field is malformed we still see data for the rest
  // since we write the fields as we parse them for speed.
  const protos::FtraceEvent& event = bundle->event().Get(0);
  EXPECT_EQ(event.pid(), 28712ul);
  EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 608934, 535199));
  EXPECT_EQ(event.print().buf(), "");
}

TEST(CpuReaderTest, FilterByEvent) {
  const ExamplePage* test_case = &g_single_print;

  BundleProvider bundle_provider(base::kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, {});

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

  BundleProvider bundle_provider(base::kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, {"print"});

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

  BundleProvider bundle_provider(base::kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, {"sched_switch"});

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

  FakeEventProvider provider(base::kPageSize);

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

// # tracer: nop
// #
// # entries-in-buffer/entries-written: 86106/86106   #P:8
// #
// #                              _-----=> irqs-off
// #                             / _----=> need-resched
// #                            | / _---=> hardirq/softirq
// #                            || / _--=> preempt-depth
// #                            ||| /     delay
// #           TASK-PID   CPU#  ||||    TIMESTAMP  FUNCTION
// #              | |       |   ||||       |         |
//            <...>-3348  [000] d..3   112.247370: sched_switch: prev_comm=Jit
//            thread pool prev_pid=3348 prev_prio=129 prev_state=R+ ==>
//            next_comm=EventThread next_pid=624 next_prio=97
//      EventThread-624   [000] d..3   112.247400: sched_switch:
//      prev_comm=EventThread prev_pid=624 prev_prio=97 prev_state=S ==>
//      next_comm=Jit thread pool next_pid=3348 next_prio=129
//            <...>-3348  [000] d..3   112.255808: sched_switch: prev_comm=Jit
//            thread pool prev_pid=3348 prev_prio=129 prev_state=S ==>
//            next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.263558: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=DispSync next_pid=623 next_prio=97
//         DispSync-623   [000] d..3   112.263620: sched_switch:
//         prev_comm=DispSync prev_pid=623 prev_prio=97 prev_state=S ==>
//         next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.263896: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=EventThread next_pid=624 next_prio=97
//      EventThread-624   [000] d..3   112.263919: sched_switch:
//      prev_comm=EventThread prev_pid=624 prev_prio=97 prev_state=S ==>
//      next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.266159: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=sugov:0 next_pid=568 next_prio=49
//            <...>-568   [000] d..3   112.266200: sched_switch:
//            prev_comm=sugov:0 prev_pid=568 prev_prio=49 prev_state=S ==>
//            next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.267581: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=DispSync next_pid=623 next_prio=97
//         DispSync-623   [000] d..3   112.267615: sched_switch:
//         prev_comm=DispSync prev_pid=623 prev_prio=97 prev_state=S ==>
//         next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.267650: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=surfaceflinger next_pid=587 next_prio=98
//   surfaceflinger-587   [000] d..3   112.268143: sched_switch:
//   prev_comm=surfaceflinger prev_pid=587 prev_prio=98 prev_state=S ==>
//   next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.268348: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=mdss_fb0 next_pid=5207 next_prio=83
//         mdss_fb0-5207  [000] d..3   112.270246: sched_switch:
//         prev_comm=mdss_fb0 prev_pid=5207 prev_prio=83 prev_state=D ==>
//         next_comm=ksoftirqd/0 next_pid=3 next_prio=120
//            <...>-3     [000] d..3   112.270561: sched_switch:
//            prev_comm=ksoftirqd/0 prev_pid=3 prev_prio=120 prev_state=S ==>
//            next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.273353: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:6 next_pid=356 next_prio=120
//            <...>-356   [000] d..3   112.273438: sched_switch:
//            prev_comm=kworker/u16:6 prev_pid=356 prev_prio=120 prev_state=D
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.275699: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=Binder:2168_15 next_pid=5350 next_prio=120
//            <...>-5350  [000] d..3   112.275954: sched_switch:
//            prev_comm=Binder:2168_15 prev_pid=5350 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.276738: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:9 next_pid=1250 next_prio=120
//    kworker/u16:9-1250  [000] d..3   112.276755: sched_switch:
//    prev_comm=kworker/u16:9 prev_pid=1250 prev_prio=120 prev_state=S ==>
//    next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.276923: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:9 next_pid=1250 next_prio=120
//    kworker/u16:9-1250  [000] d..3   112.276939: sched_switch:
//    prev_comm=kworker/u16:9 prev_pid=1250 prev_prio=120 prev_state=S ==>
//    next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.277227: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:9 next_pid=1250 next_prio=120
//    kworker/u16:9-1250  [000] d..3   112.277235: sched_switch:
//    prev_comm=kworker/u16:9 prev_pid=1250 prev_prio=120 prev_state=S ==>
//    next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.280347: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=DispSync next_pid=623 next_prio=97
//         DispSync-623   [000] d..3   112.280403: sched_switch:
//         prev_comm=DispSync prev_pid=623 prev_prio=97 prev_state=S ==>
//         next_comm=ksoftirqd/0 next_pid=3 next_prio=120
//            <...>-3     [000] d..3   112.280470: sched_switch:
//            prev_comm=ksoftirqd/0 prev_pid=3 prev_prio=120 prev_state=S ==>
//            next_comm=dex2oat next_pid=7988 next_prio=130
//            <...>-7988  [000] d..3   112.280484: sched_switch:
//            prev_comm=dex2oat prev_pid=7988 prev_prio=130 prev_state=R ==>
//            next_comm=sugov:0 next_pid=568 next_prio=49
//            <...>-568   [000] d..3   112.280498: sched_switch:
//            prev_comm=sugov:0 prev_pid=568 prev_prio=49 prev_state=R+ ==>
//            next_comm=migration/0 next_pid=13 next_prio=0
//      migration/0-13    [000] d..3   112.280511: sched_switch:
//      prev_comm=migration/0 prev_pid=13 prev_prio=0 prev_state=S ==>
//      next_comm=sugov:0 next_pid=568 next_prio=49
//            <...>-568   [000] d..3   112.280563: sched_switch:
//            prev_comm=sugov:0 prev_pid=568 prev_prio=49 prev_state=S ==>
//            next_comm=dex2oat next_pid=7988 next_prio=130
//            <...>-7988  [000] d..3   112.280740: sched_switch:
//            prev_comm=dex2oat prev_pid=7988 prev_prio=130 prev_state=D ==>
//            next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.281141: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=android.hardwar next_pid=2770 next_prio=120
//            <...>-2770  [000] d..3   112.281182: sched_switch:
//            prev_comm=android.hardwar prev_pid=2770 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.281576: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=android.hardwar next_pid=2770 next_prio=120
//            <...>-2770  [000] d..3   112.281602: sched_switch:
//            prev_comm=android.hardwar prev_pid=2770 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.282168: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=android.hardwar next_pid=2770 next_prio=120
//            <...>-2770  [000] d..3   112.282193: sched_switch:
//            prev_comm=android.hardwar prev_pid=2770 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.282890: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=android.hardwar next_pid=2770 next_prio=120
//            <...>-2770  [000] d..3   112.282914: sched_switch:
//            prev_comm=android.hardwar prev_pid=2770 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.284356: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=DispSync next_pid=623 next_prio=97
//         DispSync-623   [000] d..3   112.284398: sched_switch:
//         prev_comm=DispSync prev_pid=623 prev_prio=97 prev_state=S ==>
//         next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.284434: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=surfaceflinger next_pid=587 next_prio=98
//   surfaceflinger-587   [000] d..3   112.284570: sched_switch:
//   prev_comm=surfaceflinger prev_pid=587 prev_prio=98 prev_state=S ==>
//   next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.289826: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:6 next_pid=356 next_prio=120
//            <...>-356   [000] d..3   112.289839: sched_switch:
//            prev_comm=kworker/u16:6 prev_pid=356 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.290021: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:6 next_pid=356 next_prio=120
//            <...>-356   [000] d..3   112.290061: sched_switch:
//            prev_comm=kworker/u16:6 prev_pid=356 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.290949: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:6 next_pid=356 next_prio=120
//            <...>-356   [000] d..3   112.290986: sched_switch:
//            prev_comm=kworker/u16:6 prev_pid=356 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.291102: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:6 next_pid=356 next_prio=120
//            <...>-356   [000] d..3   112.291111: sched_switch:
//            prev_comm=kworker/u16:6 prev_pid=356 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.291408: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:6 next_pid=356 next_prio=120
//            <...>-356   [000] d..3   112.291416: sched_switch:
//            prev_comm=kworker/u16:6 prev_pid=356 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.291435: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:6 next_pid=356 next_prio=120
//            <...>-356   [000] d..3   112.291442: sched_switch:
//            prev_comm=kworker/u16:6 prev_pid=356 prev_prio=120 prev_state=S
//            ==> next_comm=swapper/0 next_pid=0 next_prio=120
//           <idle>-0     [000] d..3   112.291512: sched_switch:
//           prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==>
//           next_comm=kworker/u16:6 next_pid=356 next_prio=120

ExamplePage g_full_page_sched_switch{
    "synthetic",
    R"(
00000000: 31f2 7622 1a00 0000 b40f 0000 0000 0000  1.v"............
00000010: 1e00 0000 0000 0000 1000 0000 2f00 0103  ............/...
00000020: 140d 0000 4a69 7420 7468 7265 6164 2070  ....Jit thread p
00000030: 6f6f 6c00 140d 0000 8100 0000 0008 0000  ool.............
00000040: 0000 0000 4576 656e 7454 6872 6561 6400  ....EventThread.
00000050: 6572 0000 7002 0000 6100 0000 f057 0e00  er..p...a....W..
00000060: 2f00 0103 7002 0000 4576 656e 7454 6872  /...p...EventThr
00000070: 6561 6400 6572 0000 7002 0000 6100 0000  ead.er..p...a...
00000080: 0100 0000 0000 0000 4a69 7420 7468 7265  ........Jit thre
00000090: 6164 2070 6f6f 6c00 140d 0000 8100 0000  ad pool.........
000000a0: 50c2 0910 2f00 0103 140d 0000 4a69 7420  P.../.......Jit 
000000b0: 7468 7265 6164 2070 6f6f 6c00 140d 0000  thread pool.....
000000c0: 8100 0000 0100 0000 0000 0000 7377 6170  ............swap
000000d0: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
000000e0: 7800 0000 901a c80e 2f00 0103 0000 0000  x......./.......
000000f0: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000100: 0000 0000 7800 0000 0000 0000 0000 0000  ....x...........
00000110: 4469 7370 5379 6e63 0069 6e67 6572 0000  DispSync.inger..
00000120: 6f02 0000 6100 0000 1064 1e00 2f00 0103  o...a....d../...
00000130: 6f02 0000 4469 7370 5379 6e63 0069 6e67  o...DispSync.ing
00000140: 6572 0000 6f02 0000 6100 0000 0100 0000  er..o...a.......
00000150: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000160: 0000 0000 0000 0000 7800 0000 9074 8600  ........x....t..
00000170: 2f00 0103 0000 0000 7377 6170 7065 722f  /.......swapper/
00000180: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000190: 0000 0000 0000 0000 4576 656e 7454 6872  ........EventThr
000001a0: 6561 6400 6572 0000 7002 0000 6100 0000  ead.er..p...a...
000001b0: d071 0b00 2f00 0103 7002 0000 4576 656e  .q../...p...Even
000001c0: 7454 6872 6561 6400 6572 0000 7002 0000  tThread.er..p...
000001d0: 6100 0000 0100 0000 0000 0000 7377 6170  a...........swap
000001e0: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
000001f0: 7800 0000 10cd 4504 2f00 0103 0000 0000  x.....E./.......
00000200: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000210: 0000 0000 7800 0000 0000 0000 0000 0000  ....x...........
00000220: 7375 676f 763a 3000 0000 0000 0000 0000  sugov:0.........
00000230: 3802 0000 3100 0000 30d6 1300 2f00 0103  8...1...0.../...
00000240: 3802 0000 7375 676f 763a 3000 0000 0000  8...sugov:0.....
00000250: 0000 0000 3802 0000 3100 0000 0100 0000  ....8...1.......
00000260: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000270: 0000 0000 0000 0000 7800 0000 3049 a202  ........x...0I..
00000280: 2f00 0103 0000 0000 7377 6170 7065 722f  /.......swapper/
00000290: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
000002a0: 0000 0000 0000 0000 4469 7370 5379 6e63  ........DispSync
000002b0: 0069 6e67 6572 0000 6f02 0000 6100 0000  .inger..o...a...
000002c0: d07a 1000 2f00 0103 6f02 0000 4469 7370  .z../...o...Disp
000002d0: 5379 6e63 0069 6e67 6572 0000 6f02 0000  Sync.inger..o...
000002e0: 6100 0000 0100 0000 0000 0000 7377 6170  a...........swap
000002f0: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000300: 7800 0000 d085 1100 2f00 0103 0000 0000  x......./.......
00000310: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000320: 0000 0000 7800 0000 0000 0000 0000 0000  ....x...........
00000330: 7375 7266 6163 6566 6c69 6e67 6572 0000  surfaceflinger..
00000340: 4b02 0000 6200 0000 907a f000 2f00 0103  K...b....z../...
00000350: 4b02 0000 7375 7266 6163 6566 6c69 6e67  K...surfacefling
00000360: 6572 0000 4b02 0000 6200 0000 0100 0000  er..K...b.......
00000370: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000380: 0000 0000 0000 0000 7800 0000 305a 6400  ........x...0Zd.
00000390: 2f00 0103 0000 0000 7377 6170 7065 722f  /.......swapper/
000003a0: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
000003b0: 0000 0000 0000 0000 6d64 7373 5f66 6230  ........mdss_fb0
000003c0: 0000 0000 0000 0000 5714 0000 5300 0000  ........W...S...
000003d0: 10b1 9e03 2f00 0103 5714 0000 6d64 7373  ..../...W...mdss
000003e0: 5f66 6230 0000 0000 0000 0000 5714 0000  _fb0........W...
000003f0: 5300 0000 0200 0000 0000 0000 6b73 6f66  S...........ksof
00000400: 7469 7271 642f 3000 0000 0000 0300 0000  tirqd/0.........
00000410: 7800 0000 90bb 9900 2f00 0103 0300 0000  x......./.......
00000420: 6b73 6f66 7469 7271 642f 3000 0000 0000  ksoftirqd/0.....
00000430: 0300 0000 7800 0000 0100 0000 0000 0000  ....x...........
00000440: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000450: 0000 0000 7800 0000 701e 5305 2f00 0103  ....x...p.S./...
00000460: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000470: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
00000480: 0000 0000 6b77 6f72 6b65 722f 7531 363a  ....kworker/u16:
00000490: 3600 0000 6401 0000 7800 0000 90a1 2900  6...d...x.....).
000004a0: 2f00 0103 6401 0000 6b77 6f72 6b65 722f  /...d...kworker/
000004b0: 7531 363a 3600 0000 6401 0000 7800 0000  u16:6...d...x...
000004c0: 0200 0000 0000 0000 7377 6170 7065 722f  ........swapper/
000004d0: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
000004e0: b0e5 4f04 2f00 0103 0000 0000 7377 6170  ..O./.......swap
000004f0: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000500: 7800 0000 0000 0000 0000 0000 4269 6e64  x...........Bind
00000510: 6572 3a32 3136 385f 3135 0000 e614 0000  er:2168_15......
00000520: 7800 0000 b0bd 7c00 2f00 0103 e614 0000  x.....|./.......
00000530: 4269 6e64 6572 3a32 3136 385f 3135 0000  Binder:2168_15..
00000540: e614 0000 7800 0000 0100 0000 0000 0000  ....x...........
00000550: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000560: 0000 0000 7800 0000 d0bd 7e01 2f00 0103  ....x.....~./...
00000570: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000580: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
00000590: 0000 0000 6b77 6f72 6b65 722f 7531 363a  ....kworker/u16:
000005a0: 3900 0000 e204 0000 7800 0000 7016 0800  9.......x...p...
000005b0: 2f00 0103 e204 0000 6b77 6f72 6b65 722f  /.......kworker/
000005c0: 7531 363a 3900 0000 e204 0000 7800 0000  u16:9.......x...
000005d0: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
000005e0: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
000005f0: 1004 5200 2f00 0103 0000 0000 7377 6170  ..R./.......swap
00000600: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000610: 7800 0000 0000 0000 0000 0000 6b77 6f72  x...........kwor
00000620: 6b65 722f 7531 363a 3900 0000 e204 0000  ker/u16:9.......
00000630: 7800 0000 d0db 0700 2f00 0103 e204 0000  x......./.......
00000640: 6b77 6f72 6b65 722f 7531 363a 3900 0000  kworker/u16:9...
00000650: e204 0000 7800 0000 0100 0000 0000 0000  ....x...........
00000660: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000670: 0000 0000 7800 0000 b0a2 8c00 2f00 0103  ....x......./...
00000680: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000690: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
000006a0: 0000 0000 6b77 6f72 6b65 722f 7531 363a  ....kworker/u16:
000006b0: 3900 0000 e204 0000 7800 0000 d02b 0400  9.......x....+..
000006c0: 2f00 0103 e204 0000 6b77 6f72 6b65 722f  /.......kworker/
000006d0: 7531 363a 3900 0000 e204 0000 7800 0000  u16:9.......x...
000006e0: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
000006f0: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000700: d064 ef05 2f00 0103 0000 0000 7377 6170  .d../.......swap
00000710: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000720: 7800 0000 0000 0000 0000 0000 4469 7370  x...........Disp
00000730: 5379 6e63 0069 6e67 6572 0000 6f02 0000  Sync.inger..o...
00000740: 6100 0000 f07d 1b00 2f00 0103 6f02 0000  a....}../...o...
00000750: 4469 7370 5379 6e63 0069 6e67 6572 0000  DispSync.inger..
00000760: 6f02 0000 6100 0000 0100 0000 0000 0000  o...a...........
00000770: 6b73 6f66 7469 7271 642f 3000 0000 0000  ksoftirqd/0.....
00000780: 0300 0000 7800 0000 304c 2000 2f00 0103  ....x...0L ./...
00000790: 0300 0000 6b73 6f66 7469 7271 642f 3000  ....ksoftirqd/0.
000007a0: 0000 0000 0300 0000 7800 0000 0100 0000  ........x.......
000007b0: 0000 0000 6465 7832 6f61 7400 3935 5f33  ....dex2oat.95_3
000007c0: 0000 0000 341f 0000 8200 0000 700b 0700  ....4.......p...
000007d0: 2f00 0103 341f 0000 6465 7832 6f61 7400  /...4...dex2oat.
000007e0: 3935 5f33 0000 0000 341f 0000 8200 0000  95_3....4.......
000007f0: 0000 0000 0000 0000 7375 676f 763a 3000  ........sugov:0.
00000800: 0000 0000 0000 0000 3802 0000 3100 0000  ........8...1...
00000810: 50b0 0600 2f00 0103 3802 0000 7375 676f  P.../...8...sugo
00000820: 763a 3000 0000 0000 0000 0000 3802 0000  v:0.........8...
00000830: 3100 0000 0008 0000 0000 0000 6d69 6772  1...........migr
00000840: 6174 696f 6e2f 3000 0000 0000 0d00 0000  ation/0.........
00000850: 0000 0000 d09c 0600 2f00 0103 0d00 0000  ......../.......
00000860: 6d69 6772 6174 696f 6e2f 3000 0000 0000  migration/0.....
00000870: 0d00 0000 0000 0000 0100 0000 0000 0000  ................
00000880: 7375 676f 763a 3000 0000 0000 0000 0000  sugov:0.........
00000890: 3802 0000 3100 0000 7061 1900 2f00 0103  8...1...pa../...
000008a0: 3802 0000 7375 676f 763a 3000 0000 0000  8...sugov:0.....
000008b0: 0000 0000 3802 0000 3100 0000 0100 0000  ....8...1.......
000008c0: 0000 0000 6465 7832 6f61 7400 3935 5f33  ....dex2oat.95_3
000008d0: 0000 0000 341f 0000 8200 0000 f03c 5600  ....4........<V.
000008e0: 2f00 0103 341f 0000 6465 7832 6f61 7400  /...4...dex2oat.
000008f0: 3935 5f33 0000 0000 341f 0000 8200 0000  95_3....4.......
00000900: 0200 0000 0000 0000 7377 6170 7065 722f  ........swapper/
00000910: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000920: 5013 c400 2f00 0103 0000 0000 7377 6170  P.../.......swap
00000930: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000940: 7800 0000 0000 0000 0000 0000 616e 6472  x...........andr
00000950: 6f69 642e 6861 7264 7761 7200 d20a 0000  oid.hardwar.....
00000960: 7800 0000 30c9 1300 2f00 0103 d20a 0000  x...0.../.......
00000970: 616e 6472 6f69 642e 6861 7264 7761 7200  android.hardwar.
00000980: d20a 0000 7800 0000 0100 0000 0000 0000  ....x...........
00000990: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
000009a0: 0000 0000 7800 0000 7097 c000 2f00 0103  ....x...p.../...
000009b0: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
000009c0: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
000009d0: 0000 0000 616e 6472 6f69 642e 6861 7264  ....android.hard
000009e0: 7761 7200 d20a 0000 7800 0000 305c 0c00  war.....x...0\..
000009f0: 2f00 0103 d20a 0000 616e 6472 6f69 642e  /.......android.
00000a00: 6861 7264 7761 7200 d20a 0000 7800 0000  hardwar.....x...
00000a10: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
00000a20: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000a30: d0aa 1401 2f00 0103 0000 0000 7377 6170  ..../.......swap
00000a40: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000a50: 7800 0000 0000 0000 0000 0000 616e 6472  x...........andr
00000a60: 6f69 642e 6861 7264 7761 7200 d20a 0000  oid.hardwar.....
00000a70: 7800 0000 903b 0c00 2f00 0103 d20a 0000  x....;../.......
00000a80: 616e 6472 6f69 642e 6861 7264 7761 7200  android.hardwar.
00000a90: d20a 0000 7800 0000 0100 0000 0000 0000  ....x...........
00000aa0: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000ab0: 0000 0000 7800 0000 f024 5401 2f00 0103  ....x....$T./...
00000ac0: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000ad0: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
00000ae0: 0000 0000 616e 6472 6f69 642e 6861 7264  ....android.hard
00000af0: 7761 7200 d20a 0000 7800 0000 f0f3 0b00  war.....x.......
00000b00: 2f00 0103 d20a 0000 616e 6472 6f69 642e  /.......android.
00000b10: 6861 7264 7761 7200 d20a 0000 7800 0000  hardwar.....x...
00000b20: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
00000b30: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000b40: d0b5 bf02 2f00 0103 0000 0000 7377 6170  ..../.......swap
00000b50: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000b60: 7800 0000 0000 0000 0000 0000 4469 7370  x...........Disp
00000b70: 5379 6e63 0069 6e67 6572 0000 6f02 0000  Sync.inger..o...
00000b80: 6100 0000 90cd 1400 2f00 0103 6f02 0000  a......./...o...
00000b90: 4469 7370 5379 6e63 0069 6e67 6572 0000  DispSync.inger..
00000ba0: 6f02 0000 6100 0000 0100 0000 0000 0000  o...a...........
00000bb0: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000bc0: 0000 0000 7800 0000 50a6 1100 2f00 0103  ....x...P.../...
00000bd0: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000be0: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
00000bf0: 0000 0000 7375 7266 6163 6566 6c69 6e67  ....surfacefling
00000c00: 6572 0000 4b02 0000 6200 0000 b04c 4200  er..K...b....LB.
00000c10: 2f00 0103 4b02 0000 7375 7266 6163 6566  /...K...surfacef
00000c20: 6c69 6e67 6572 0000 4b02 0000 6200 0000  linger..K...b...
00000c30: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
00000c40: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000c50: b025 060a 2f00 0103 0000 0000 7377 6170  .%../.......swap
00000c60: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000c70: 7800 0000 0000 0000 0000 0000 6b77 6f72  x...........kwor
00000c80: 6b65 722f 7531 363a 3600 0000 6401 0000  ker/u16:6...d...
00000c90: 7800 0000 d0b6 0600 2f00 0103 6401 0000  x......./...d...
00000ca0: 6b77 6f72 6b65 722f 7531 363a 3600 0000  kworker/u16:6...
00000cb0: 6401 0000 7800 0000 0100 0000 0000 0000  d...x...........
00000cc0: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000cd0: 0000 0000 7800 0000 f0a0 5800 2f00 0103  ....x.....X./...
00000ce0: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000cf0: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
00000d00: 0000 0000 6b77 6f72 6b65 722f 7531 363a  ....kworker/u16:
00000d10: 3600 0000 6401 0000 7800 0000 f07a 1300  6...d...x....z..
00000d20: 2f00 0103 6401 0000 6b77 6f72 6b65 722f  /...d...kworker/
00000d30: 7531 363a 3600 0000 6401 0000 7800 0000  u16:6...d...x...
00000d40: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
00000d50: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000d60: b080 b101 2f00 0103 0000 0000 7377 6170  ..../.......swap
00000d70: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000d80: 7800 0000 0000 0000 0000 0000 6b77 6f72  x...........kwor
00000d90: 6b65 722f 7531 363a 3600 0000 6401 0000  ker/u16:6...d...
00000da0: 7800 0000 103c 1200 2f00 0103 6401 0000  x....<../...d...
00000db0: 6b77 6f72 6b65 722f 7531 363a 3600 0000  kworker/u16:6...
00000dc0: 6401 0000 7800 0000 0100 0000 0000 0000  d...x...........
00000dd0: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000de0: 0000 0000 7800 0000 50ea 3800 2f00 0103  ....x...P.8./...
00000df0: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000e00: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
00000e10: 0000 0000 6b77 6f72 6b65 722f 7531 363a  ....kworker/u16:
00000e20: 3600 0000 6401 0000 7800 0000 5032 0400  6...d...x...P2..
00000e30: 2f00 0103 6401 0000 6b77 6f72 6b65 722f  /...d...kworker/
00000e40: 7531 363a 3600 0000 6401 0000 7800 0000  u16:6...d...x...
00000e50: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
00000e60: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000e70: 70f5 9000 2f00 0103 0000 0000 7377 6170  p.../.......swap
00000e80: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000e90: 7800 0000 0000 0000 0000 0000 6b77 6f72  x...........kwor
00000ea0: 6b65 722f 7531 363a 3600 0000 6401 0000  ker/u16:6...d...
00000eb0: 7800 0000 10d7 0300 2f00 0103 6401 0000  x......./...d...
00000ec0: 6b77 6f72 6b65 722f 7531 363a 3600 0000  kworker/u16:6...
00000ed0: 6401 0000 7800 0000 0100 0000 0000 0000  d...x...........
00000ee0: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000ef0: 0000 0000 7800 0000 907c 0900 2f00 0103  ....x....|../...
00000f00: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
00000f10: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
00000f20: 0000 0000 6b77 6f72 6b65 722f 7531 363a  ....kworker/u16:
00000f30: 3600 0000 6401 0000 7800 0000 7082 0300  6...d...x...p...
00000f40: 2f00 0103 6401 0000 6b77 6f72 6b65 722f  /...d...kworker/
00000f50: 7531 363a 3600 0000 6401 0000 7800 0000  u16:6...d...x...
00000f60: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
00000f70: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000f80: f0ec 2100 2f00 0103 0000 0000 7377 6170  ..!./.......swap
00000f90: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000fa0: 7800 0000 0000 0000 0000 0000 6b77 6f72  x...........kwor
00000fb0: 6b65 722f 7531 363a 3600 0000 6401 0000  ker/u16:6...d...
00000fc0: 7800 0000 0000 0000 0000 0000 0000 0000  x...............
00000fd0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000fe0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000ff0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
    )",
};

TEST(CpuReaderTest, ParseFullPageSchedSwitch) {
  const ExamplePage* test_case = &g_full_page_sched_switch;

  BundleProvider bundle_provider(base::kPageSize);
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  EventFilter filter(*table, {"sched_switch"});

  ASSERT_TRUE(CpuReader::ParsePage(42 /* cpu number */, page.get(), &filter,
                                   bundle_provider.writer(), table));

  auto bundle = bundle_provider.ParseProto();
  ASSERT_TRUE(bundle);
  EXPECT_EQ(bundle->cpu(), 42ul);
  EXPECT_EQ(bundle->event().size(), 59);
}

}  // namespace perfetto
