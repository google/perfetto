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

#include "src/traced/probes/ftrace/cpu_reader.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "protos/perfetto/trace/ftrace/generic.pbzero.h"
#include "src/base/test/tmp_dir_tree.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"
#include "src/traced/probes/ftrace/test/cpu_reader_support.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/dpu.gen.h"
#include "protos/perfetto/trace/ftrace/f2fs.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.pbzero.h"
#include "protos/perfetto/trace/ftrace/generic.gen.h"
#include "protos/perfetto/trace/ftrace/power.gen.h"
#include "protos/perfetto/trace/ftrace/raw_syscalls.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/ftrace/task.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "src/traced/probes/ftrace/test/test_messages.gen.h"
#include "src/traced/probes/ftrace/test/test_messages.pbzero.h"

using protozero::proto_utils::ProtoSchemaType;
using testing::_;
using testing::AnyNumber;
using testing::Contains;
using testing::Each;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::EndsWith;
using testing::Eq;
using testing::IsEmpty;
using testing::NiceMock;
using testing::Not;
using testing::Pair;
using testing::Property;
using testing::Return;
using testing::SizeIs;
using testing::StartsWith;

namespace perfetto {
namespace {

using FtraceParseStatus = protos::pbzero::FtraceParseStatus;

FtraceDataSourceConfig EmptyConfig() {
  return FtraceDataSourceConfig{EventFilter{},
                                EventFilter{},
                                DisabledCompactSchedConfigForTesting(),
                                std::nullopt,
                                {},
                                {},
                                {},
                                false /*symbolize_ksyms*/,
                                50u,
                                {}};
}

constexpr uint64_t kNanoInSecond = 1000 * 1000 * 1000;
constexpr uint64_t kNanoInMicro = 1000;

::testing::AssertionResult WithinOneMicrosecond(uint64_t actual_ns,
                                                uint64_t expected_s,
                                                uint64_t expected_us) {
  // Round to closest us.
  uint64_t actual_us = (actual_ns + kNanoInMicro / 2) / kNanoInMicro;
  uint64_t total_expected_us = expected_s * 1000 * 1000 + expected_us;
  if (actual_us == total_expected_us)
    return ::testing::AssertionSuccess();

  return ::testing::AssertionFailure()
         << actual_ns / kNanoInSecond << "."
         << (actual_ns % kNanoInSecond) / kNanoInMicro << " vs. " << expected_s
         << "." << expected_us;
}

class MockFtraceProcfs : public FtraceProcfs {
 public:
  MockFtraceProcfs() : FtraceProcfs("/root/") {
    ON_CALL(*this, NumberOfCpus()).WillByDefault(Return(1));
    ON_CALL(*this, WriteToFile(_, _)).WillByDefault(Return(true));
    ON_CALL(*this, ClearFile(_)).WillByDefault(Return(true));
    EXPECT_CALL(*this, NumberOfCpus()).Times(AnyNumber());
  }

  MOCK_METHOD(bool,
              WriteToFile,
              (const std::string& path, const std::string& str),
              (override));
  MOCK_METHOD(char, ReadOneCharFromFile, (const std::string& path), (override));
  MOCK_METHOD(bool, ClearFile, (const std::string& path), (override));
  MOCK_METHOD(std::string,
              ReadFileIntoString,
              (const std::string& path),
              (const, override));
  MOCK_METHOD(size_t, NumberOfCpus, (), (const, override));
};

class CpuReaderTableTest : public ::testing::Test {
 protected:
  NiceMock<MockFtraceProcfs> ftrace_;
};

// Single class to manage the whole protozero -> scattered stream -> chunks ->
// single buffer -> real proto dance. Has a method: writer() to get an
// protozero ftrace bundle writer and a method ParseProto() to attempt to
// parse whatever has been written so far into a proto message.
template <class ZeroT, class ProtoT>
class ProtoProvider {
 public:
  explicit ProtoProvider(size_t chunk_size) : chunk_size_(chunk_size) {}
  ~ProtoProvider() = default;

  ZeroT* writer() { return writer_.get(); }
  void ResetWriter() { writer_.Reset(); }

  // Stitch together the scattered chunks into a single buffer then attempt
  // to parse the buffer as a FtraceEventBundle. Returns the FtraceEventBundle
  // on success and nullptr on failure.
  std::unique_ptr<ProtoT> ParseProto() {
    auto bundle = std::make_unique<ProtoT>();
    std::vector<uint8_t> buffer = writer_.SerializeAsArray();
    if (!bundle->ParseFromArray(buffer.data(), buffer.size()))
      return nullptr;
    return bundle;
  }

 private:
  ProtoProvider(const ProtoProvider&) = delete;
  ProtoProvider& operator=(const ProtoProvider&) = delete;

  size_t chunk_size_;
  protozero::HeapBuffered<ZeroT> writer_;
};

using BundleProvider = ProtoProvider<protos::pbzero::FtraceEventBundle,
                                     protos::gen::FtraceEventBundle>;

class BinaryWriter {
 public:
  BinaryWriter()
      : size_(base::GetSysPageSize()),
        page_(new uint8_t[size_]),
        ptr_(page_.get()) {}

  template <typename T>
  void Write(T t) {
    memcpy(ptr_, &t, sizeof(T));
    ptr_ += sizeof(T);
    PERFETTO_CHECK(ptr_ < ptr_ + size_);
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

  size_t written() { return static_cast<size_t>(ptr_ - page_.get()); }

 private:
  size_t size_;
  std::unique_ptr<uint8_t[]> page_;
  uint8_t* ptr_;
};

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

TEST(ParsePageHeaderTest, WithOverrun) {
  std::string text = R"(
    00000000: 3ef3 db77 67a2 0100 f00f 0080 ffff ffff
    )";
  auto page = PageFromXxd(text);

  // parse as if we're on a 32 bit kernel (4 byte "commit" field)
  {
    const uint8_t* ptr = page.get();
    auto ret = CpuReader::ParsePageHeader(&ptr, 4u);
    ASSERT_TRUE(ret.has_value());
    CpuReader::PageHeader parsed = ret.value();

    ASSERT_EQ(parsed.timestamp, 0x0001A26777DBF33Eull);  // first 8 bytes
    ASSERT_EQ(parsed.size, 0x0ff0u);                     // 4080
    ASSERT_TRUE(parsed.lost_events);

    // pointer advanced past the header (8+4 bytes)
    ASSERT_EQ(ptr, page.get() + 12);
  }

  // parse as if we're on a 64 bit kernel (8 byte "commit" field)
  {
    const uint8_t* ptr = page.get();
    auto ret = CpuReader::ParsePageHeader(&ptr, 8u);
    ASSERT_TRUE(ret.has_value());
    CpuReader::PageHeader parsed = ret.value();

    ASSERT_EQ(parsed.timestamp, 0x0001A26777DBF33Eull);  // first 8 bytes
    ASSERT_EQ(parsed.size, 0x0ff0u);                     // 4080
    ASSERT_TRUE(parsed.lost_events);

    // pointer advanced past the header (8+8 bytes)
    ASSERT_EQ(ptr, page.get() + 16);
  }
}

// clang-format off
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
// clang-format on

static ExamplePage g_single_print{
    "synthetic",
    R"(
    00000000: ba12 6a33 c628 0200 2c00 0000 0000 0000  ..j3.(..,.......
    00000010: def0 ec67 8d21 0000 0800 0000 0500 0001  ...g.!..........
    00000020: 2870 0000 ac5d 1661 86ff ffff 4865 6c6c  (p...].a....Hell
    00000030: 6f2c 2077 6f72 6c64 210a 00ff 0000 0000  o, world!.......
  )",
};

class CpuReaderParsePagePayloadTest : public testing::Test {
 protected:
  CpuReader::Bundler* CreateBundler(const FtraceDataSourceConfig& ds_config) {
    PERFETTO_CHECK(!bundler_.has_value());
    writer_.emplace();
    compact_sched_buf_ = std::make_unique<CompactSchedBuffer>();
    bundler_.emplace(&writer_.value(), &metadata_, /*symbolizer=*/nullptr,
                     /*cpu=*/0,
                     /*ftrace_clock_snapshot=*/nullptr,
                     protos::pbzero::FTRACE_CLOCK_UNSPECIFIED,
                     compact_sched_buf_.get(), ds_config.compact_sched.enabled,
                     /*last_read_event_ts=*/0);
    return &bundler_.value();
  }

  protos::gen::FtraceEventBundle GetBundle() {
    PERFETTO_CHECK(bundler_.has_value());
    PERFETTO_CHECK(writer_.has_value());
    bundler_.reset();
    protos::gen::FtraceEventBundle bundle =
        writer_->GetOnlyTracePacket().ftrace_events();
    writer_.reset();
    return bundle;
  }

  std::vector<protos::gen::TracePacket> AllTracePackets() {
    PERFETTO_CHECK(bundler_.has_value());
    PERFETTO_CHECK(writer_.has_value());
    bundler_.reset();
    std::vector<protos::gen::TracePacket> packets =
        writer_->GetAllTracePackets();
    writer_.reset();
    return packets;
  }

  FtraceMetadata metadata_;
  std::optional<TraceWriterForTesting> writer_;
  std::unique_ptr<CompactSchedBuffer> compact_sched_buf_;
  std::optional<CpuReader::Bundler> bundler_;
  uint64_t last_read_event_ts_ = 0;
};

TEST_F(CpuReaderParsePagePayloadTest, ParseSinglePrint) {
  const ExamplePage* test_case = &g_single_print;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("ftrace", "print")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_EQ(44ul, page_header->size);
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();
  ASSERT_EQ(bundle.event().size(), 1u);
  const protos::gen::FtraceEvent& event = bundle.event()[0];
  EXPECT_EQ(event.pid(), 28712ul);
  EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 608934, 535199));
  EXPECT_EQ(event.print().buf(), "Hello, world!\n");
}

// clang-format off
// # tracer: nop
// #
// # entries-in-buffer/entries-written: 2/2   #P:8
// #
// #                                      _-----=> irqs-off
// #                                     / _----=> need-resched
// #                                    | / _---=> hardirq/softirq
// #                                    || / _--=> preempt-depth
// #                                    ||| /     delay
// #           TASK-PID    TGID   CPU#  ||||    TIMESTAMP  FUNCTION
// #              | |        |      |   ||||       |         |
//             echo-6908  ( 6908) [000] ...1 282762.884473: tracing_mark_write: qwertyuiopqwrtyuiopqwertyuiopqwertyuiopqwer[...]
//             echo-6908  ( 6908) [000] ...1 282762.884492: tracing_mark_write:
// clang-format on

static ExamplePage g_really_long_event{
    "synthetic",
    R"(
      00000000: 6be0 48dd 2b01 0100 e403 0000 0000 0000  k.H.+...........
      00000010: 1e00 0000 0000 0000 0000 0000 c003 0000  ................
      00000020: 0500 0001 fc1a 0000 4096 3615 9cff ffff  ........@.6.....
      00000030: 7177 6572 7479 7569 6f70 7177 7274 7975  qwertyuiopqwrtyu
      00000040: 696f 7071 7765 7274 7975 696f 7071 7765  iopqwertyuiopqwe
      00000050: 7274 7975 696f 7071 7765 7274 7975 696f  rtyuiopqwertyuio
      00000060: 7071 7772 7479 7569 6f70 7177 6572 7479  pqwrtyuiopqwerty
      00000070: 7569 6f70 7177 6572 7479 7569 6f71 7765  uiopqwertyuioqwe
      00000080: 7274 7975 696f 7071 7772 7479 7569 6f70  rtyuiopqwrtyuiop
      00000090: 7177 6572 7479 7569 6f70 7177 6572 7479  qwertyuiopqwerty
      000000a0: 7569 6f71 7765 7274 7975 696f 7071 7772  uioqwertyuiopqwr
      000000b0: 7479 7569 6f70 7177 6572 7479 7569 6f70  tyuiopqwertyuiop
      000000c0: 7177 6572 7479 7569 6f70 7070 7177 6572  qwertyuiopppqwer
      000000d0: 7479 7569 6f70 7177 7274 7975 696f 7071  tyuiopqwrtyuiopq
      000000e0: 7765 7274 7975 696f 7071 7765 7274 7975  wertyuiopqwertyu
      000000f0: 696f 7071 7765 7274 7975 696f 7071 7772  iopqwertyuiopqwr
      00000100: 7479 7569 6f70 7177 6572 7479 7569 6f70  tyuiopqwertyuiop
      00000110: 7177 6572 7479 7569 6f71 7765 7274 7975  qwertyuioqwertyu
      00000120: 696f 7071 7772 7479 7569 6f70 7177 6572  iopqwrtyuiopqwer
      00000130: 7479 7569 6f70 7177 6572 7479 7569 6f71  tyuiopqwertyuioq
      00000140: 7765 7274 7975 696f 7071 7772 7479 7569  wertyuiopqwrtyui
      00000150: 6f70 7177 6572 7479 7569 6f70 7177 6572  opqwertyuiopqwer
      00000160: 7479 7569 6f70 7070 7177 6572 7479 7569  tyuiopppqwertyui
      00000170: 6f70 7177 7274 7975 696f 7071 7765 7274  opqwrtyuiopqwert
      00000180: 7975 696f 7071 7765 7274 7975 696f 7071  yuiopqwertyuiopq
      00000190: 7765 7274 7975 696f 7071 7772 7479 7569  wertyuiopqwrtyui
      000001a0: 6f70 7177 6572 7479 7569 6f70 7177 6572  opqwertyuiopqwer
      000001b0: 7479 7569 6f71 7765 7274 7975 696f 7071  tyuioqwertyuiopq
      000001c0: 7772 7479 7569 6f70 7177 6572 7479 7569  wrtyuiopqwertyui
      000001d0: 6f70 7177 6572 7479 7569 6f71 7765 7274  opqwertyuioqwert
      000001e0: 7975 696f 7071 7772 7479 7569 6f70 7177  yuiopqwrtyuiopqw
      000001f0: 6572 7479 7569 6f70 7177 6572 7479 7569  ertyuiopqwertyui
      00000200: 6f70 7070 7177 6572 7479 7569 6f70 7177  opppqwertyuiopqw
      00000210: 7274 7975 696f 7071 7765 7274 7975 696f  rtyuiopqwertyuio
      00000220: 7071 7765 7274 7975 696f 7071 7765 7274  pqwertyuiopqwert
      00000230: 7975 696f 7071 7772 7479 7569 6f70 7177  yuiopqwrtyuiopqw
      00000240: 6572 7479 7569 6f70 7177 6572 7479 7569  ertyuiopqwertyui
      00000250: 6f71 7765 7274 7975 696f 7071 7772 7479  oqwertyuiopqwrty
      00000260: 7569 6f70 7177 6572 7479 7569 6f70 7177  uiopqwertyuiopqw
      00000270: 6572 7479 7569 6f71 7765 7274 7975 696f  ertyuioqwertyuio
      00000280: 7071 7772 7479 7569 6f70 7177 6572 7479  pqwrtyuiopqwerty
      00000290: 7569 6f70 7177 6572 7479 7569 6f70 7070  uiopqwertyuioppp
      000002a0: 7177 6572 7479 7569 6f70 7177 7274 7975  qwertyuiopqwrtyu
      000002b0: 696f 7071 7765 7274 7975 696f 7071 7765  iopqwertyuiopqwe
      000002c0: 7274 7975 696f 7071 7765 7274 7975 696f  rtyuiopqwertyuio
      000002d0: 7071 7772 7479 7569 6f70 7177 6572 7479  pqwrtyuiopqwerty
      000002e0: 7569 6f70 7177 6572 7479 7569 6f71 7765  uiopqwertyuioqwe
      000002f0: 7274 7975 696f 7071 7772 7479 7569 6f70  rtyuiopqwrtyuiop
      00000300: 7177 6572 7479 7569 6f70 7177 6572 7479  qwertyuiopqwerty
      00000310: 7569 6f71 7765 7274 7975 696f 7071 7772  uioqwertyuiopqwr
      00000320: 7479 7569 6f70 7177 6572 7479 7569 6f70  tyuiopqwertyuiop
      00000330: 7177 6572 7479 7569 6f70 7070 7177 6572  qwertyuiopppqwer
      00000340: 7479 7569 6f70 7177 7274 7975 696f 7071  tyuiopqwrtyuiopq
      00000350: 7765 7274 7975 696f 7071 7765 7274 7975  wertyuiopqwertyu
      00000360: 696f 7071 7765 7274 7975 696f 7071 7772  iopqwertyuiopqwr
      00000370: 7479 7569 6f70 7177 6572 7479 7569 6f70  tyuiopqwertyuiop
      00000380: 7177 6572 7479 7569 6f71 7765 7274 7975  qwertyuioqwertyu
      00000390: 696f 7071 7772 7479 7569 6f70 7177 6572  iopqwrtyuiopqwer
      000003a0: 7479 7569 6f70 7177 6572 7479 7569 6f71  tyuiopqwertyuioq
      000003b0: 7765 7274 7975 696f 7071 7772 7479 7569  wertyuiopqwrtyui
      000003c0: 6f70 7177 6572 7479 7569 6f70 7177 6572  opqwertyuiopqwer
      000003d0: 7479 7569 6f70 7070 0a00 5115 6562 0900  tyuioppp..Q.eb..
      000003e0: 0500 0001 fc1a 0000 4096 3615 9cff ffff  ........@.6.....
      000003f0: 0a00 0000 0000 0000 0000 0000 0000 0000  ................
      00000400: 0000 0000 0000 0000 0000 0000 0000 0000  ................
      00000410: 0000 0000 0000 0000 0000 0000 0000 0000  ................
      00000420: 0000 0000 0000 0000 0000 0000 0000 0000  ................
  )",
};

TEST_F(CpuReaderParsePagePayloadTest, ReallyLongEvent) {
  const ExamplePage* test_case = &g_really_long_event;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("ftrace", "print")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  CpuReader::ParsePagePayload(parse_pos, &page_header.value(), table,
                              &ds_config, CreateBundler(ds_config), &metadata_,
                              &last_read_event_ts_);

  auto bundle = GetBundle();
  const protos::gen::FtraceEvent& long_print = bundle.event()[0];
  EXPECT_THAT(long_print.print().buf(), StartsWith("qwerty"));
  EXPECT_THAT(long_print.print().buf(), EndsWith("ppp\n"));
  const protos::gen::FtraceEvent& newline = bundle.event()[1];
  EXPECT_EQ(newline.print().buf(), "\n");
}

// This event is as the event for ParseSinglePrint above except the string
// is extended and not null terminated.
static ExamplePage g_single_print_non_null_terminated{
    "synthetic",
    R"(
    00000000: ba12 6a33 c628 0200 2c00 0000 0000 0000  ..j3.(..,.......
    00000010: def0 ec67 8d21 0000 0800 0000 0500 0001  ...g.!..........
    00000020: 2870 0000 ac5d 1661 86ff ffff 4865 6c6c  (p...].a....Hell
    00000030: 6f2c 2077 6f72 6c64 2161 6161 6161 6161  o, world!aaaaaaa
    00000040: 6161 6161 6161 6161 6161 6161 6161 6161  aaaaaaaaaaaaaaaa
  )",
};

TEST_F(CpuReaderParsePagePayloadTest, ParseSinglePrintNonNullTerminated) {
  const ExamplePage* test_case = &g_single_print_non_null_terminated;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("ftrace", "print")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();
  ASSERT_EQ(bundle.event().size(), 1u);
  const protos::gen::FtraceEvent& event = bundle.event()[0];
  EXPECT_EQ(event.pid(), 28712ul);
  EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 608934, 535199));
  EXPECT_EQ(event.print().buf(), "Hello, world!aaa");
}

static ExamplePage g_single_print_zero_size{
    "synthetic",
    R"(
    00000000: ba12 6a33 c628 0200 2c00 0000 0000 0000  ..j3.(..,.......
    00000010: def0 ec67 8d21 0000 0800 0000 0500 0001  ...g.!..........
    00000020: 2870 0000 ac5d 1661 86ff ffff 0000 0000  (p...].a........
    00000030: 0000 0000 0000 0000 0000 0000 0000 0000  ................
    00000040: 0000 0000 0000 0000 0000 0000 0000 0000  ................
  )",
};

TEST_F(CpuReaderParsePagePayloadTest, ParseSinglePrintZeroSize) {
  const ExamplePage* test_case = &g_single_print_zero_size;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("ftrace", "print")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();
  ASSERT_EQ(bundle.event().size(), 1u);
  const protos::gen::FtraceEvent& event = bundle.event()[0];
  EXPECT_EQ(event.pid(), 28712ul);
  EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 608934, 535199));
  EXPECT_TRUE(event.print().has_buf());
  EXPECT_EQ(event.print().buf(), "");
}

TEST_F(CpuReaderParsePagePayloadTest, FilterByEvent) {
  const ExamplePage* test_case = &g_single_print;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  EXPECT_THAT(AllTracePackets(), IsEmpty());
}

// clang-format off
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
// clang-format on

static ExamplePage g_three_prints{
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

TEST_F(CpuReaderParsePagePayloadTest, ParseThreePrint) {
  const ExamplePage* test_case = &g_three_prints;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("ftrace", "print")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();
  ASSERT_EQ(bundle.event().size(), 3u);

  {
    const protos::gen::FtraceEvent& event = bundle.event()[0];
    EXPECT_EQ(event.pid(), 30693ul);
    EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 615436, 216806));
    EXPECT_EQ(event.print().buf(), "Hello, world!\n");
  }

  {
    const protos::gen::FtraceEvent& event = bundle.event()[1];
    EXPECT_EQ(event.pid(), 30693ul);
    EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 615486, 377232));
    EXPECT_EQ(event.print().buf(), "Good afternoon, world!\n");
  }

  {
    const protos::gen::FtraceEvent& event = bundle.event()[2];
    EXPECT_EQ(event.pid(), 30693ul);
    EXPECT_TRUE(WithinOneMicrosecond(event.timestamp(), 615495, 632679));
    EXPECT_EQ(event.print().buf(), "Goodbye, world!\n");
  }
}

TEST_F(CpuReaderParsePagePayloadTest, ParsePrintWithAndWithoutFilter) {
  using FtraceEventBundle = protos::gen::FtraceEventBundle;
  using FtraceEvent = protos::gen::FtraceEvent;
  using PrintFtraceEvent = protos::gen::PrintFtraceEvent;

  const ExamplePage* test_case = &g_three_prints;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  ASSERT_FALSE(page_header->lost_events);
  ASSERT_LE(parse_pos + page_header->size, page_end);
  {
    FtraceDataSourceConfig ds_config_no_filter = EmptyConfig();
    ds_config_no_filter.event_filter.AddEnabledEvent(
        table->EventToFtraceId(GroupAndName("ftrace", "print")));

    FtraceParseStatus status = CpuReader::ParsePagePayload(
        parse_pos, &page_header.value(), table, &ds_config_no_filter,
        CreateBundler(ds_config_no_filter), &metadata_, &last_read_event_ts_);
    EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

    auto bundle = GetBundle();
    EXPECT_THAT(
        bundle,
        Property(
            &FtraceEventBundle::event,
            ElementsAre(
                Property(&FtraceEvent::print,
                         Property(&PrintFtraceEvent::buf, "Hello, world!\n")),
                Property(&FtraceEvent::print,
                         Property(&PrintFtraceEvent::buf,
                                  "Good afternoon, world!\n")),
                Property(&FtraceEvent::print, Property(&PrintFtraceEvent::buf,
                                                       "Goodbye, world!\n")))));
  }

  {
    FtraceDataSourceConfig ds_config_with_filter = EmptyConfig();
    ds_config_with_filter.event_filter.AddEnabledEvent(
        table->EventToFtraceId(GroupAndName("ftrace", "print")));

    FtraceConfig::PrintFilter conf;
    auto* rule = conf.add_rules();
    rule->set_prefix("Good ");
    rule->set_allow(false);
    ds_config_with_filter.print_filter =
        FtracePrintFilterConfig::Create(conf, table);
    ASSERT_TRUE(ds_config_with_filter.print_filter.has_value());

    FtraceParseStatus status = CpuReader::ParsePagePayload(
        parse_pos, &page_header.value(), table, &ds_config_with_filter,
        CreateBundler(ds_config_with_filter), &metadata_, &last_read_event_ts_);
    EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

    auto bundle = GetBundle();
    EXPECT_THAT(
        bundle,
        Property(
            &FtraceEventBundle::event,
            ElementsAre(
                Property(&FtraceEvent::print,
                         Property(&PrintFtraceEvent::buf, "Hello, world!\n")),
                Property(&FtraceEvent::print, Property(&PrintFtraceEvent::buf,
                                                       "Goodbye, world!\n")))));
  }
}

TEST(CpuReaderTest, ProcessPagesForDataSourceNoEmptyPackets) {
  const ExamplePage* test_case = &g_three_prints;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  std::vector<const void*> test_page_order = {
      page.get(), page.get(), page.get(), page.get(),
      page.get(), page.get(), page.get(), page.get()};

  // Prepare a buffer with 8 contiguous pages, with the above contents.
  static constexpr size_t kTestPages = 8;

  std::unique_ptr<uint8_t[]> buf(
      new uint8_t[base::GetSysPageSize() * kTestPages]());
  for (size_t i = 0; i < kTestPages; i++) {
    void* dest = buf.get() + (i * base::GetSysPageSize());
    memcpy(dest, static_cast<const void*>(test_page_order[i]),
           base::GetSysPageSize());
  }
  auto compact_sched_buf = std::make_unique<CompactSchedBuffer>();

  {
    FtraceMetadata metadata{};
    FtraceDataSourceConfig with_filter = EmptyConfig();
    with_filter.event_filter.AddEnabledEvent(
        table->EventToFtraceId(GroupAndName("ftrace", "print")));

    FtraceConfig::PrintFilter conf;
    auto* rule = conf.add_rules();
    rule->set_prefix("");
    rule->set_allow(false);
    with_filter.print_filter = FtracePrintFilterConfig::Create(conf, table);
    ASSERT_TRUE(with_filter.print_filter.has_value());

    TraceWriterForTesting trace_writer;
    base::FlatSet<protos::pbzero::FtraceParseStatus> parse_errors;
    uint64_t last_read_event_ts = 0;
    bool success = CpuReader::ProcessPagesForDataSource(
        &trace_writer, &metadata, /*cpu=*/1, &with_filter, &parse_errors,
        &last_read_event_ts, buf.get(), kTestPages, compact_sched_buf.get(),
        table,
        /*symbolizer=*/nullptr,
        /*ftrace_clock_snapshot=*/nullptr,
        protos::pbzero::FTRACE_CLOCK_UNSPECIFIED);

    EXPECT_TRUE(success);

    // Check that the data source doesn't emit any packet, not even empty
    // packets.
    EXPECT_THAT(trace_writer.GetAllTracePackets(), IsEmpty());
  }

  {
    FtraceMetadata metadata{};
    FtraceDataSourceConfig without_filter = EmptyConfig();
    without_filter.event_filter.AddEnabledEvent(
        table->EventToFtraceId(GroupAndName("ftrace", "print")));

    TraceWriterForTesting trace_writer;
    base::FlatSet<protos::pbzero::FtraceParseStatus> parse_errors;
    uint64_t last_read_event_ts = 0;
    bool success = CpuReader::ProcessPagesForDataSource(
        &trace_writer, &metadata, /*cpu=*/1, &without_filter, &parse_errors,
        &last_read_event_ts, buf.get(), kTestPages, compact_sched_buf.get(),
        table,
        /*symbolizer=*/nullptr,
        /*ftrace_clock_snapshot=*/nullptr,
        protos::pbzero::FTRACE_CLOCK_UNSPECIFIED);

    EXPECT_TRUE(success);

    EXPECT_THAT(trace_writer.GetAllTracePackets(), Not(IsEmpty()));
  }
}

// clang-format off
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
//      ksoftirqd/0-3     [000] d..3 1045157.722134: sched_switch: prev_comm=ksoftirqd/0 prev_pid=3 prev_prio=120 prev_state=S ==> next_comm=sleep next_pid=3733 next_prio=120
//            sleep-3733  [000] d..3 1045157.725035: sched_switch: prev_comm=sleep prev_pid=3733 prev_prio=120 prev_state=R+ ==> next_comm=rcuop/0 next_pid=10 next_prio=120
//      rcu_preempt-7     [000] d..3 1045157.725182: sched_switch: prev_comm=rcu_preempt prev_pid=7 prev_prio=120 prev_state=S ==> next_comm=sleep next_pid=3733 next_prio=120
//            sleep-3733  [000] d..3 1045157.725671: sched_switch: prev_comm=sleep prev_pid=3733 prev_prio=120 prev_state=R+ ==> next_comm=sh next_pid=3513 next_prio=120
//               sh-3513  [000] d..3 1045157.726668: sched_switch: prev_comm=sh prev_pid=3513 prev_prio=120 prev_state=S ==> next_comm=sleep next_pid=3733 next_prio=120
//            sleep-3733  [000] d..3 1045157.726697: sched_switch: prev_comm=sleep prev_pid=3733 prev_prio=120 prev_state=x ==> next_comm=kworker/u16:3 next_pid=3681 next_prio=120
// clang-format on

static ExamplePage g_six_sched_switch{
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

TEST_F(CpuReaderParsePagePayloadTest, ParseSixSchedSwitch) {
  const ExamplePage* test_case = &g_six_sched_switch;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);
  EXPECT_EQ(last_read_event_ts_, 1'045'157'726'697'236ULL);

  auto bundle = GetBundle();
  EXPECT_EQ(0u, bundle.previous_bundle_end_timestamp());
  ASSERT_EQ(bundle.event().size(), 6u);
  {
    const protos::gen::FtraceEvent& event = bundle.event()[1];
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

TEST_F(CpuReaderParsePagePayloadTest, ParseSixSchedSwitchCompactFormat) {
  const ExamplePage* test_case = &g_six_sched_switch;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config{EventFilter{},
                                   EventFilter{},
                                   EnabledCompactSchedConfigForTesting(),
                                   std::nullopt,
                                   {},
                                   {},
                                   {},
                                   false /* symbolize_ksyms*/,
                                   false /*preserve_ftrace_buffer*/,
                                   {}};
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);
  EXPECT_EQ(last_read_event_ts_, 1'045'157'726'697'236ULL);

  // sched switch fields were buffered:
  EXPECT_LT(0u, bundler_->compact_sched_buf()->sched_switch().size());
  EXPECT_LT(0u,
            bundler_->compact_sched_buf()->interner().interned_comms_size());

  // Write the buffer out & check the serialized format:
  auto bundle = GetBundle();

  const auto& compact_sched = bundle.compact_sched();
  EXPECT_EQ(0u, bundle.previous_bundle_end_timestamp());

  EXPECT_EQ(6u, compact_sched.switch_timestamp().size());
  EXPECT_EQ(6u, compact_sched.switch_prev_state().size());
  EXPECT_EQ(6u, compact_sched.switch_next_pid().size());
  EXPECT_EQ(6u, compact_sched.switch_next_prio().size());
  // 4 unique interned next_comm strings:
  EXPECT_EQ(4u, compact_sched.intern_table().size());
  EXPECT_EQ(6u, compact_sched.switch_next_comm_index().size());

  // First event exactly as expected (absolute timestamp):
  EXPECT_TRUE(WithinOneMicrosecond(compact_sched.switch_timestamp()[0], 1045157,
                                   722134));
  EXPECT_EQ(1, compact_sched.switch_prev_state()[0]);
  EXPECT_EQ(3733, compact_sched.switch_next_pid()[0]);
  EXPECT_EQ(120, compact_sched.switch_next_prio()[0]);
  auto comm_intern_idx = compact_sched.switch_next_comm_index()[0];
  std::string next_comm = compact_sched.intern_table()[comm_intern_idx];
  EXPECT_EQ("sleep", next_comm);
}

// clang-format off
// # tracer: nop
// #
// # entries-in-buffer/entries-written: 23/23   #P:8
// #
// #                                _-----=> irqs-off/BH-disabled
// #                               / _----=> need-resched
// #                              | / _---=> hardirq/softirq
// #                              || / _--=> preempt-depth
// #                              ||| / _-=> migrate-disable
// #                              |||| /     delay
// #           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
// #              | |         |   |||||     |         |
//           <idle>-0       [000] d..2. 701500.111507: sched_switch: prev_comm=swapper/0 prev_pid=0 prev_prio=120 prev_state=R ==> next_comm=bash next_pid=219057 next_prio=120
//               ls-219057  [000] d..3. 701500.115222: sched_waking: comm=kworker/u16:17 pid=203967 prio=120 target_cpu=006
//               ls-219057  [000] d..3. 701500.115327: sched_waking: comm=kworker/u16:17 pid=203967 prio=120 target_cpu=006
//               ls-219057  [000] d..3. 701500.115412: sched_waking: comm=kworker/u16:5 pid=205556 prio=120 target_cpu=004
//               ls-219057  [000] d..3. 701500.115416: sched_waking: comm=kworker/u16:17 pid=203967 prio=120 target_cpu=006
//               ls-219057  [000] dN.5. 701500.115801: sched_waking: comm=bash pid=217958 prio=120 target_cpu=006
//               ls-219057  [000] d..2. 701500.115817: sched_switch: prev_comm=ls prev_pid=219057 prev_prio=120 prev_state=Z ==> next_comm=swapper/0 next_pid=0 next_prio=120
// clang-format on

static ExamplePage g_sched_page{
    "synthetic_alt",
    R"(
    00000000: 67ce f4b8 027e 0200 5801 0000 0000 0000  g....~..X.......
    00000010: 1e00 0000 0000 0000 1000 0000 3d01 0102  ............=...
    00000020: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
    00000030: 0000 0000 0000 0000 7800 0000 0000 0000  ........x.......
    00000040: 0000 0000 6261 7368 0000 0000 0000 0000  ....bash........
    00000050: 0000 0000 b157 0300 7800 0000 a9d2 1507  .....W..x.......
    00000060: 4001 0103 b157 0300 6b77 6f72 6b65 722f  @....W..kworker/
    00000070: 7531 363a 3137 0000 bf1c 0300 7800 0000  u16:17......x...
    00000080: 0600 0000 c953 3300 4001 0103 b157 0300  .....S3.@....W..
    00000090: 6b77 6f72 6b65 722f 7531 363a 3137 0000  kworker/u16:17..
    000000a0: bf1c 0300 7800 0000 0600 0000 0981 2900  ....x.........).
    000000b0: 4001 0103 b157 0300 6b77 6f72 6b65 722f  @....W..kworker/
    000000c0: 7531 363a 3500 0000 f422 0300 7800 0000  u16:5...."..x...
    000000d0: 0400 0000 89e0 0100 4001 0103 b157 0300  ........@....W..
    000000e0: 6b77 6f72 6b65 722f 7531 363a 3137 0000  kworker/u16:17..
    000000f0: bf1c 0300 7800 0000 0600 0000 e92c bc00  ....x........,..
    00000100: 4001 2505 b157 0300 6261 7368 0000 0000  @.%..W..bash....
    00000110: 0000 0000 0000 0000 6653 0300 7800 0000  ........fS..x...
    00000120: 0600 0000 10f8 0700 3d01 0102 b157 0300  ........=....W..
    00000130: 6c73 0000 0000 0000 0000 0000 0000 0000  ls..............
    00000140: b157 0300 7800 0000 2000 0000 0000 0000  .W..x... .......
    00000150: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
    00000160: 0000 0000 7800 0000 0000 0000 0000 0000  ....x...........
    )",
};

TEST_F(CpuReaderParsePagePayloadTest, ParseCompactSchedSwitchAndWaking) {
  const ExamplePage* test_case = &g_sched_page;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config{EventFilter{},
                                   EventFilter{},
                                   EnabledCompactSchedConfigForTesting(),
                                   std::nullopt,
                                   {},
                                   {},
                                   {},
                                   false /* symbolize_ksyms*/,
                                   false /*preserve_ftrace_buffer*/,
                                   {}};
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_waking")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  // sched fields were buffered:
  EXPECT_LT(0u, bundler_->compact_sched_buf()->sched_switch().size());
  EXPECT_LT(0u, bundler_->compact_sched_buf()->sched_waking().size());
  EXPECT_LT(0u,
            bundler_->compact_sched_buf()->interner().interned_comms_size());

  // Write the buffer out & check the serialized format:
  auto bundle = GetBundle();
  const auto& compact_sched = bundle.compact_sched();
  // 2 sched_switch events:
  EXPECT_EQ(2u, compact_sched.switch_timestamp().size());
  EXPECT_EQ(2u, compact_sched.switch_prev_state().size());
  EXPECT_EQ(2u, compact_sched.switch_next_pid().size());
  EXPECT_EQ(2u, compact_sched.switch_next_prio().size());
  EXPECT_EQ(2u, compact_sched.switch_next_comm_index().size());
  // 5 sched_waking events:
  EXPECT_EQ(5u, compact_sched.waking_timestamp().size());
  EXPECT_EQ(5u, compact_sched.waking_pid().size());
  EXPECT_EQ(5u, compact_sched.waking_target_cpu().size());
  EXPECT_EQ(5u, compact_sched.waking_prio().size());
  EXPECT_EQ(5u, compact_sched.waking_comm_index().size());
  EXPECT_EQ(5u, compact_sched.waking_common_flags().size());
  // 4 unique interned comm strings:
  EXPECT_EQ(4u, compact_sched.intern_table().size());

  // First sched waking as expected:
  EXPECT_EQ(compact_sched.waking_timestamp()[0], 701500115221756ull);
  EXPECT_EQ(compact_sched.waking_pid()[0], 203967);
  EXPECT_EQ(compact_sched.waking_target_cpu()[0], 6);
  EXPECT_EQ(compact_sched.waking_prio()[0], 120);
  EXPECT_EQ(compact_sched.waking_common_flags()[0], 1u);
  auto comm_intern_idx = compact_sched.waking_comm_index()[0];
  std::string comm = compact_sched.intern_table()[comm_intern_idx];
  EXPECT_EQ("kworker/u16:17", comm);
}

TEST_F(CpuReaderParsePagePayloadTest, ParseKprobeAndKretprobe) {
  char kprobe_fuse_file_write_iter_page[] =
      R"(
    00000000: b31b bfe2 a513 0000 1400 0000 0000 0000  ................
    00000010: 0400 0000 ff05 48ff 8a33 0000 443d 0e91  ......H..3..D=..
    00000020: ffff ffff 0000 0000 0000 0000 0000 0000  ................
    )";

  std::unique_ptr<uint8_t[]> page =
      PageFromXxd(kprobe_fuse_file_write_iter_page);

  base::TmpDirTree ftrace;
  ftrace.AddFile("available_events", "perfetto_kprobes:fuse_file_write_iter\n");
  ftrace.AddDir("events");
  ftrace.AddFile(
      "events/header_page",
      R"(        field: u64 timestamp;   offset:0;       size:8; signed:0;
        field: local_t commit;  offset:8;       size:8; signed:1;
        field: int overwrite;   offset:8;       size:1; signed:1;
        field: char data;       offset:16;      size:4080;      signed:1;
)");
  ftrace.AddDir("events/perfetto_kprobes");
  ftrace.AddDir("events/perfetto_kprobes/fuse_file_write_iter");
  ftrace.AddFile("events/perfetto_kprobes/fuse_file_write_iter/format",
                 R"format(name: fuse_file_write_iter
ID: 1535
format:
        field:unsigned short common_type;       offset:0;       size:2; signed:0;
        field:unsigned char common_flags;       offset:2;       size:1; signed:0;
        field:unsigned char common_preempt_count;       offset:3;       size:1; signed:0;
        field:int common_pid;   offset:4;       size:4; signed:1;

        field:unsigned long __probe_ip; offset:8;       size:8; signed:0;

print fmt: "(%lx)", REC->__probe_ip
)format");
  ftrace.AddFile("trace", "");

  std::unique_ptr<FtraceProcfs> ftrace_procfs =
      FtraceProcfs::Create(ftrace.path() + "/");
  ASSERT_NE(ftrace_procfs.get(), nullptr);
  std::unique_ptr<ProtoTranslationTable> table = ProtoTranslationTable::Create(
      ftrace_procfs.get(), GetStaticEventInfo(), GetStaticCommonFieldsInfo());
  table->GetOrCreateKprobeEvent(
      GroupAndName("perfetto_kprobes", "fuse_file_write_iter"));

  auto ftrace_evt_id = static_cast<uint32_t>(table->EventToFtraceId(
      GroupAndName("perfetto_kprobes", "fuse_file_write_iter")));
  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(ftrace_evt_id);
  ds_config.kprobes[ftrace_evt_id] =
      protos::pbzero::KprobeEvent::KprobeType::KPROBE_TYPE_INSTANT;

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table.get(), &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  // Write the buffer out & check the serialized format:
  auto bundle = GetBundle();
  ASSERT_EQ(bundle.event_size(), 1);
  EXPECT_EQ(bundle.event()[0].kprobe_event().name(), "fuse_file_write_iter");
  EXPECT_EQ(bundle.event()[0].kprobe_event().type(),
            protos::gen::KprobeEvent::KPROBE_TYPE_INSTANT);
}

TEST_F(CpuReaderTableTest, ParseAllFields) {
  using FakeEventProvider =
      ProtoProvider<pbzero::FakeFtraceEvent, gen::FakeFtraceEvent>;

  uint16_t ftrace_event_id = 102;

  std::vector<Field> common_fields;
  {
    common_fields.emplace_back(Field{});
    Field* field = &common_fields.back();
    field->ftrace_offset = 4;
    field->ftrace_size = 4;
    field->ftrace_type = kFtraceCommonPid32;
    field->proto_field_id = 2;
    field->proto_field_type = ProtoSchemaType::kInt32;
    SetTranslationStrategy(field->ftrace_type, field->proto_field_type,
                           &field->strategy);
  }

  std::vector<Event> events;
  events.emplace_back(Event{});
  {
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
      field->proto_field_type = ProtoSchemaType::kUint32;
    }

    {
      // pid32 -> uint32
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 12;
      field->ftrace_size = 4;
      field->ftrace_type = kFtracePid32;
      field->proto_field_id = 2;
      field->proto_field_type = ProtoSchemaType::kInt32;
    }

    {
      // dev32 -> uint64
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 16;
      field->ftrace_size = 4;
      field->ftrace_type = kFtraceDevId32;
      field->proto_field_id = 3;
      field->proto_field_type = ProtoSchemaType::kUint64;
    }

    {
      // ino_t (32bit) -> uint64
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 20;
      field->ftrace_size = 4;
      field->ftrace_type = kFtraceInode32;
      field->proto_field_id = 4;
      field->proto_field_type = ProtoSchemaType::kUint64;
    }

    {
      // dev64 -> uint64
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 24;
      field->ftrace_size = 8;
      field->ftrace_type = kFtraceDevId64;
      field->proto_field_id = 5;
      field->proto_field_type = ProtoSchemaType::kUint64;
    }

    {
      // ino_t (64bit) -> uint64
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 32;
      field->ftrace_size = 8;
      field->ftrace_type = kFtraceInode64;
      field->proto_field_id = 6;
      field->proto_field_type = ProtoSchemaType::kUint64;
    }

    {
      // char[16] -> string
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 40;
      field->ftrace_size = 16;
      field->ftrace_type = kFtraceFixedCString;
      field->proto_field_id = 500;
      field->proto_field_type = ProtoSchemaType::kString;
    }

    {
      // char* -> string
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 56;
      field->ftrace_size = 8;
      field->ftrace_type = kFtraceStringPtr;
      field->proto_field_id = 503;
      field->proto_field_type = ProtoSchemaType::kString;
    }

    {
      // dataloc -> string
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 65;
      field->ftrace_size = 4;
      field->ftrace_type = kFtraceDataLoc;
      field->proto_field_id = 502;
      field->proto_field_type = ProtoSchemaType::kString;
    }

    {
      // char -> string
      event->fields.emplace_back(Field{});
      Field* field = &event->fields.back();
      field->ftrace_offset = 69;
      field->ftrace_size = 0;
      field->ftrace_type = kFtraceCString;
      field->proto_field_id = 501;
      field->proto_field_type = ProtoSchemaType::kString;
    }

    for (Field& field : event->fields) {
      SetTranslationStrategy(field.ftrace_type, field.proto_field_type,
                             &field.strategy);
    }
  }

  PrintkMap printk_formats;
  printk_formats.insert(0xffffff8504f51b23, "my_printk_format_string");
  ProtoTranslationTable table(
      &ftrace_, events, std::move(common_fields),
      ProtoTranslationTable::DefaultPageHeaderSpecForTesting(),
      InvalidCompactSchedEventFormatForTesting(), printk_formats);
  FtraceDataSourceConfig ds_config = EmptyConfig();

  FakeEventProvider provider(base::GetSysPageSize());

  BinaryWriter writer;

  // Must use the bit masks to translate between kernel and userspace device ids
  // to generate the below examples
  const uint32_t kKernelBlockDeviceId = 271581216;

  const BlockDeviceID kUserspaceBlockDeviceId =
      CpuReader::TranslateBlockDeviceIDToUserspace<BlockDeviceID>(
          kKernelBlockDeviceId);
  const uint64_t k64BitKernelBlockDeviceId = 4442450946;
  const BlockDeviceID k64BitUserspaceBlockDeviceId =
      CpuReader::TranslateBlockDeviceIDToUserspace<uint64_t>(
          k64BitKernelBlockDeviceId);

  writer.Write<int32_t>(1001);                       // Common field.
  writer.Write<int32_t>(9999);                       // Common pid
  writer.Write<int32_t>(1003);                       // Uint32 field
  writer.Write<int32_t>(97);                         // Pid
  writer.Write<int32_t>(kKernelBlockDeviceId);       // Dev id
  writer.Write<int32_t>(98);                         // Inode 32
  writer.Write<int64_t>(k64BitKernelBlockDeviceId);  // Dev id 64
  writer.Write<int64_t>(99u);                        // Inode 64
  writer.WriteFixedString(16, "Hello");
  writer.Write<uint64_t>(0xffffff8504f51b23ULL);  // char* (printk formats)
  writer.Write<uint8_t>(0);                       // Deliberately mis-aligning.
  writer.Write<uint32_t>(40 | 6 << 16);
  writer.WriteFixedString(300, "Goodbye");

  auto input = writer.GetCopy();
  auto length = writer.written();
  FtraceMetadata metadata{};

  ASSERT_TRUE(CpuReader::ParseEvent(ftrace_event_id, input.get(),
                                    input.get() + length, &table, &ds_config,
                                    provider.writer(), &metadata));

  auto event = provider.ParseProto();
  ASSERT_TRUE(event);
  EXPECT_EQ(event->common_pid(), 9999ul);
  EXPECT_TRUE(event->has_all_fields());
  EXPECT_EQ(event->all_fields().field_uint32(), 1003u);
  EXPECT_EQ(event->all_fields().field_pid(), 97);
  EXPECT_EQ(event->all_fields().field_dev_32(),
            static_cast<uint32_t>(kUserspaceBlockDeviceId));
  EXPECT_EQ(event->all_fields().field_inode_32(), 98u);
// TODO(primiano): for some reason this fails on mac.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  EXPECT_EQ(event->all_fields().field_dev_64(), k64BitUserspaceBlockDeviceId);
#endif
  EXPECT_EQ(event->all_fields().field_inode_64(), 99u);
  EXPECT_EQ(event->all_fields().field_char_16(), "Hello");
  EXPECT_EQ(event->all_fields().field_char(), "Goodbye");
  EXPECT_EQ(event->all_fields().field_data_loc(), "Hello");
  EXPECT_EQ(event->all_fields().field_char_star(), "my_printk_format_string");
  EXPECT_THAT(metadata.pids, Contains(97));
  EXPECT_EQ(metadata.inode_and_device.size(), 2U);
  EXPECT_THAT(metadata.inode_and_device,
              Contains(Pair(98u, kUserspaceBlockDeviceId)));
  EXPECT_THAT(metadata.inode_and_device,
              Contains(Pair(99u, k64BitUserspaceBlockDeviceId)));
}

TEST(CpuReaderTest, SysEnterEvent) {
  BinaryWriter writer;
  ProtoTranslationTable* table = GetTable("synthetic");
  FtraceDataSourceConfig ds_config = EmptyConfig();

  const auto kSysEnterId = static_cast<uint16_t>(
      table->EventToFtraceId(GroupAndName("raw_syscalls", "sys_enter")));
  ASSERT_GT(kSysEnterId, 0ul);
  constexpr uint32_t kPid = 23;
  constexpr uint32_t kFd = 7;
  constexpr auto kSyscall = SYS_close;

  writer.Write<int32_t>(1001);      // Common field.
  writer.Write<int32_t>(kPid);      // Common pid
  writer.Write<int64_t>(kSyscall);  // id
  for (uint32_t i = 0; i < 6; ++i) {
    writer.Write<uint64_t>(kFd + i);  // args
  }

  auto input = writer.GetCopy();
  auto length = writer.written();

  BundleProvider bundle_provider(base::GetSysPageSize());
  FtraceMetadata metadata{};

  ASSERT_TRUE(CpuReader::ParseEvent(
      kSysEnterId, input.get(), input.get() + length, table, &ds_config,
      bundle_provider.writer()->add_event(), &metadata));

  std::unique_ptr<protos::gen::FtraceEventBundle> a =
      bundle_provider.ParseProto();
  ASSERT_NE(a, nullptr);
  ASSERT_EQ(a->event().size(), 1u);
  const auto& event = a->event()[0].sys_enter();
  EXPECT_EQ(event.id(), kSyscall);
  for (uint32_t i = 0; i < 6; ++i) {
    EXPECT_EQ(event.args()[i], kFd + i);
  }
}

// MacOS fails on this ...but MacOS will never use cpu_reader so it's
// not a big problem.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#define MAYBE_SysExitEvent DISABLED_SysExitEvent
#else
#define MAYBE_SysExitEvent SysExitEvent
#endif
TEST(CpuReaderTest, MAYBE_SysExitEvent) {
  BinaryWriter writer;
  ProtoTranslationTable* table = GetTable("synthetic");
  FtraceDataSourceConfig ds_config = EmptyConfig();
  const auto syscalls = SyscallTable::FromCurrentArch();

  const auto kSysExitId = static_cast<uint16_t>(
      table->EventToFtraceId(GroupAndName("raw_syscalls", "sys_exit")));
  ASSERT_GT(kSysExitId, 0ul);
  constexpr pid_t kPid = 23;
  constexpr int64_t kFd = 2;

  ds_config.syscalls_returning_fd =
      FtraceConfigMuxer::GetSyscallsReturningFds(syscalls);
  ASSERT_FALSE(ds_config.syscalls_returning_fd.empty());
  const auto syscall_id = *ds_config.syscalls_returning_fd.begin();

  writer.Write<int32_t>(1001);        // Common field.
  writer.Write<int32_t>(kPid);        // Common pid
  writer.Write<int64_t>(syscall_id);  // id
  writer.Write<int64_t>(kFd);         // ret

  auto input = writer.GetCopy();
  auto length = writer.written();
  BundleProvider bundle_provider(base::GetSysPageSize());
  FtraceMetadata metadata{};

  ASSERT_TRUE(CpuReader::ParseEvent(
      kSysExitId, input.get(), input.get() + length, table, &ds_config,
      bundle_provider.writer()->add_event(), &metadata));

  std::unique_ptr<protos::gen::FtraceEventBundle> a =
      bundle_provider.ParseProto();
  ASSERT_NE(a, nullptr);
  ASSERT_EQ(a->event().size(), 1u);
  const auto& event = a->event()[0].sys_exit();
  EXPECT_EQ(event.id(), syscall_id);
  EXPECT_EQ(event.ret(), kFd);
  EXPECT_THAT(metadata.fds, Contains(std::make_pair(kPid, kFd)));
}

TEST(CpuReaderTest, TaskRenameEvent) {
  BundleProvider bundle_provider(base::GetSysPageSize());

  BinaryWriter writer;
  ProtoTranslationTable* table = GetTable("android_seed_N2F62_3.10.49");
  FtraceDataSourceConfig ds_config = EmptyConfig();

  constexpr uint32_t kTaskRenameId = 19;

  writer.Write<int32_t>(1001);             // Common field.
  writer.Write<int32_t>(9999);             // Common pid
  writer.Write<int32_t>(9999);             // Pid
  writer.WriteFixedString(16, "Hello");    // Old Comm
  writer.WriteFixedString(16, "Goodbye");  // New Comm
  writer.Write<uint64_t>(10);              // flags
  writer.Write<int16_t>(10);               // oom_score_adj

  auto input = writer.GetCopy();
  auto length = writer.written();
  FtraceMetadata metadata{};

  ASSERT_TRUE(CpuReader::ParseEvent(kTaskRenameId, input.get(),
                                    input.get() + length, table, &ds_config,
                                    bundle_provider.writer(), &metadata));
  EXPECT_THAT(metadata.rename_pids, Contains(9999));
  EXPECT_THAT(metadata.pids, Contains(9999));
}

// Regression test for b/205763418: Kernels without f0a515780393("tracing: Don't
// make assumptions about length of string on task rename") can output non
// zero-terminated strings in some cases. Even though it's a kernel bug, there's
// no point in rejecting that.
TEST(CpuReaderTest, EventNonZeroTerminated) {
  BundleProvider bundle_provider(base::GetSysPageSize());

  BinaryWriter writer;
  ProtoTranslationTable* table = GetTable("android_seed_N2F62_3.10.49");
  FtraceDataSourceConfig ds_config = EmptyConfig();

  constexpr uint32_t kTaskRenameId = 19;

  writer.Write<int32_t>(1001);           // Common field.
  writer.Write<int32_t>(9999);           // Common pid
  writer.Write<int32_t>(9999);           // Pid
  writer.WriteFixedString(16, "Hello");  // Old Comm
  std::array<char, 16> newcomm;
  memcpy(&newcomm, "0123456789abcdef", sizeof newcomm);
  writer.Write(newcomm);       // New Comm - not null terminated
  writer.Write<uint64_t>(10);  // flags
  writer.Write<int16_t>(10);   // oom_score_adj

  auto input = writer.GetCopy();
  auto length = writer.written();
  FtraceMetadata metadata{};

  ASSERT_TRUE(CpuReader::ParseEvent(
      kTaskRenameId, input.get(), input.get() + length, table, &ds_config,
      bundle_provider.writer()->add_event(), &metadata));
  std::unique_ptr<protos::gen::FtraceEventBundle> a =
      bundle_provider.ParseProto();
  ASSERT_NE(a, nullptr);
  ASSERT_EQ(a->event().size(), 1u);
  ASSERT_EQ(a->event()[0].task_rename().newcomm(), "0123456789abcdef");
}

// Page with a single sched_switch, no data loss.
static char g_switch_page[] =
    R"(
    00000000: 2b16 c3be 90b6 0300 4c00 0000 0000 0000  ................
    00000010: 1e00 0000 0000 0000 1000 0000 2f00 0103  ................
    00000020: 0300 0000 6b73 6f66 7469 7271 642f 3000  ................
    00000030: 0000 0000 0300 0000 7800 0000 0100 0000  ................
    00000040: 0000 0000 736c 6565 7000 722f 3000 0000  ................
    00000050: 0000 0000 950e 0000 7800 0000 0000 0000  ................
    )";

// Page with a single sched_switch, header has data loss flag set.
static char g_switch_page_lost_events[] =
    R"(
    00000000: 2b16 c3be 90b6 0300 4c00 0080 ffff ffff  ................
    00000010: 1e00 0000 0000 0000 1000 0000 2f00 0103  ................
    00000020: 0300 0000 6b73 6f66 7469 7271 642f 3000  ................
    00000030: 0000 0000 0300 0000 7800 0000 0100 0000  ................
    00000040: 0000 0000 736c 6565 7000 722f 3000 0000  ................
    00000050: 0000 0000 950e 0000 7800 0000 0000 0000  ................
    )";

// Page with invalid data.
static char g_invalid_page[] =
    R"(
    00000000: 2b16 c3be 90b6 0300 4b00 0000 0000 0000  ................
    00000010: 1e00 0000 0000 0000 1000 0000 2f00 0103  ................
    00000020: 0300 0000 6b73 6f66 7469 7271 642f 3000  ................
    00000030: 0000 0000 0300 0000 7800 0000 0100 0000  ................
    00000040: 0000 0000 736c 6565 7000 722f 3000 0000  ................
    00000050: 0000 0000 950e 0000 7800 0000 0000 0000  ................
    )";

TEST(CpuReaderTest, NewPacketOnLostEvents) {
  auto page_ok = PageFromXxd(g_switch_page);
  auto page_loss = PageFromXxd(g_switch_page_lost_events);

  std::vector<const void*> test_page_order = {
      page_ok.get(),   page_ok.get(), page_ok.get(), page_loss.get(),
      page_loss.get(), page_ok.get(), page_ok.get(), page_ok.get()};

  // Prepare a buffer with 8 contiguous pages, with the above contents.
  static constexpr size_t kTestPages = 8;

  std::unique_ptr<uint8_t[]> buf(
      new uint8_t[base::GetSysPageSize() * kTestPages]());
  for (size_t i = 0; i < kTestPages; i++) {
    void* dest = buf.get() + (i * base::GetSysPageSize());
    memcpy(dest, static_cast<const void*>(test_page_order[i]),
           base::GetSysPageSize());
  }

  ProtoTranslationTable* table = GetTable("synthetic");
  FtraceMetadata metadata{};
  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));

  TraceWriterForTesting trace_writer;
  auto compact_sched_buf = std::make_unique<CompactSchedBuffer>();
  base::FlatSet<protos::pbzero::FtraceParseStatus> parse_errors;
  uint64_t last_read_event_ts = 0;
  bool success = CpuReader::ProcessPagesForDataSource(
      &trace_writer, &metadata, /*cpu=*/1, &ds_config, &parse_errors,
      &last_read_event_ts, buf.get(), kTestPages, compact_sched_buf.get(),
      table, /*symbolizer=*/nullptr,
      /*ftrace_clock_snapshot=*/nullptr,
      protos::pbzero::FTRACE_CLOCK_UNSPECIFIED);

  EXPECT_TRUE(success);

  // Each packet should contain the parsed contents of a contiguous run of pages
  // without data loss.
  // So we should get three packets (each page has 1 event):
  //   [3 events] [1 event] [4 events].
  auto packets = trace_writer.GetAllTracePackets();

  ASSERT_EQ(3u, packets.size());
  EXPECT_FALSE(packets[0].ftrace_events().lost_events());
  EXPECT_EQ(3u, packets[0].ftrace_events().event().size());

  EXPECT_TRUE(packets[1].ftrace_events().lost_events());
  EXPECT_EQ(1u, packets[1].ftrace_events().event().size());

  EXPECT_TRUE(packets[2].ftrace_events().lost_events());
  EXPECT_EQ(4u, packets[2].ftrace_events().event().size());
}

TEST(CpuReaderTest, ProcessPagesForDataSourceError) {
  auto page_ok = PageFromXxd(g_switch_page);
  auto page_err = PageFromXxd(g_invalid_page);

  std::vector<const void*> test_page_order = {
      page_ok.get(), page_ok.get(), page_ok.get(),  page_err.get(),
      page_ok.get(), page_ok.get(), page_err.get(), page_ok.get()};

  // Prepare a buffer with 8 contiguous pages, with the above contents.
  static constexpr size_t kTestPages = 8;

  std::unique_ptr<uint8_t[]> buf(
      new uint8_t[base::GetSysPageSize() * kTestPages]());
  for (size_t i = 0; i < kTestPages; i++) {
    void* dest = buf.get() + (i * base::GetSysPageSize());
    memcpy(dest, static_cast<const void*>(test_page_order[i]),
           base::GetSysPageSize());
  }

  ProtoTranslationTable* table = GetTable("synthetic");
  FtraceMetadata metadata{};
  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));

  TraceWriterForTesting trace_writer;
  auto compact_sched_buf = std::make_unique<CompactSchedBuffer>();
  base::FlatSet<protos::pbzero::FtraceParseStatus> parse_errors;
  uint64_t last_read_event_ts = 0;
  bool success = CpuReader::ProcessPagesForDataSource(
      &trace_writer, &metadata, /*cpu=*/1, &ds_config, &parse_errors,
      &last_read_event_ts, buf.get(), kTestPages, compact_sched_buf.get(),
      table, /*symbolizer=*/nullptr,
      /*ftrace_clock_snapshot=*/nullptr,
      protos::pbzero::FTRACE_CLOCK_UNSPECIFIED);

  EXPECT_FALSE(success);

  EXPECT_EQ(
      parse_errors.count(FtraceParseStatus::FTRACE_STATUS_ABI_END_OVERFLOW),
      1u);

  // 2 invalid pages -> 2 serialised parsing errors
  std::vector<protos::gen::TracePacket> packets =
      trace_writer.GetAllTracePackets();
  ASSERT_EQ(packets.size(), 1u);
  protos::gen::FtraceEventBundle bundle = packets[0].ftrace_events();
  using Bundle = protos::gen::FtraceEventBundle;
  using Error = Bundle::FtraceError;
  using protos::gen::FtraceParseStatus::FTRACE_STATUS_ABI_END_OVERFLOW;
  EXPECT_THAT(
      bundle,
      Property(&Bundle::error,
               ElementsAre(
                   Property(&Error::status, FTRACE_STATUS_ABI_END_OVERFLOW),
                   Property(&Error::status, FTRACE_STATUS_ABI_END_OVERFLOW))));
}

// Page containing an absolute timestamp (RINGBUF_TYPE_TIME_STAMP).
static char g_abs_timestamp[] =
    R"(
00000000: 8949 fbfb 38e4 0400 6407 0000 0000 0000  .I..8...d.......
00000010: 5032 0a2d 3b01 0100 0000 0000 7377 6170  P2.-;.......swap
00000020: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
00000030: 7800 0000 0000 0000 0000 0000 6776 6673  x...........gvfs
00000040: 2d61 6663 2d76 6f6c 756d 6500 6483 0000  -afc-volume.d...
00000050: 7800 0000 f0de 1700 3b01 0100 6483 0000  x.......;...d...
00000060: 6776 6673 2d61 6663 2d76 6f6c 756d 6500  gvfs-afc-volume.
00000070: 6483 0000 7800 0000 0100 0000 0000 0000  d...x...........
00000080: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000090: 0000 0000 7800 0000 aaa1 5c08 0401 1100  ....x.....\.....
000000a0: 0000 0000 88fc 31eb 029f ffff 609e d3c0  ......1.....`...
000000b0: ffff ffff 0076 b4a1 029f ffff 0020 0000  .....v....... ..
000000c0: ffff ffff e477 1700 0301 1100 0000 0000  .....w..........
000000d0: 88fc 31eb 029f ffff aa26 0100 3e01 1100  ..1......&..>...
000000e0: 0000 0000 6b77 6f72 6b65 722f 7538 3a35  ....kworker/u8:5
000000f0: 0000 0000 24c0 0c00 7800 0000 0100 0000  ....$...x.......
00000100: 0300 0000 90e6 e700 3b01 0100 0000 0000  ........;.......
00000110: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
00000120: 0000 0000 7800 0000 0000 0000 0000 0000  ....x...........
00000130: 6b77 6f72 6b65 722f 7538 3a35 0000 0000  kworker/u8:5....
00000140: 24c0 0c00 7800 0000 aa56 0300 3e01 0100  $...x....V..>...
00000150: 24c0 0c00 6b77 6f72 6b65 722f 7538 3a31  $...kworker/u8:1
00000160: 0000 0000 8eb5 0c00 7800 0000 0100 0000  ........x.......
00000170: 0300 0000 06eb 0300 0201 0000 24c0 0c00  ............$...
00000180: 6026 f22a 049f ffff f0e4 4cc0 ffff ffff  `&.*......L.....
00000190: ca45 0f00 3e01 0100 24c0 0c00 646d 6372  .E..>...$...dmcr
000001a0: 7970 745f 7772 6974 652f 3200 2601 0000  ypt_write/2.&...
000001b0: 7800 0000 0100 0000 0100 0000 c617 0200  x...............
000001c0: 0101 0000 24c0 0c00 6026 f22a 049f ffff  ....$...`&.*....
000001d0: f0e4 4cc0 ffff ffff a47c 0000 0301 0100  ..L......|......
000001e0: 24c0 0c00 6015 f22a 049f ffff 0685 0000  $...`..*........
000001f0: 0201 0000 24c0 0c00 a05d f22a 049f ffff  ....$....].*....
00000200: f0e4 4cc0 ffff ffff c6dd 0800 0101 0000  ..L.............
00000210: 24c0 0c00 a05d f22a 049f ffff f0e4 4cc0  $....].*......L.
00000220: ffff ffff 8444 0000 0301 0100 24c0 0c00  .....D......$...
00000230: 6059 f22a 049f ffff e672 0000 0201 0000  `Y.*.....r......
00000240: 24c0 0c00 e050 f22a 049f ffff f0e4 4cc0  $....P.*......L.
00000250: ffff ffff 4673 0a00 0101 0000 24c0 0c00  ....Fs......$...
00000260: e050 f22a 049f ffff f0e4 4cc0 ffff ffff  .P.*......L.....
00000270: 04ca 0000 0301 0100 24c0 0c00 2000 f22a  ........$... ..*
00000280: 049f ffff 86b1 0000 0201 0000 24c0 0c00  ............$...
00000290: 6015 f22a 049f ffff f0e4 4cc0 ffff ffff  `..*......L.....
000002a0: e640 0c00 0101 0000 24c0 0c00 6015 f22a  .@......$...`..*
000002b0: 049f ffff f0e4 4cc0 ffff ffff 64b4 0000  ......L.....d...
000002c0: 0301 0100 24c0 0c00 2011 f22a 049f ffff  ....$... ..*....
000002d0: 66b9 0000 0201 0000 24c0 0c00 a06e f22a  f.......$....n.*
000002e0: 049f ffff f0e4 4cc0 ffff ffff 6ae1 4200  ......L.....j.B.
000002f0: 3e01 1100 24c0 0c00 6a62 6432 2f64 6d2d  >...$...jbd2/dm-
00000300: 312d 3800 0000 0000 6a01 0000 7800 0000  1-8.....j...x...
00000310: 0100 0000 0300 0000 269b 0400 0101 0000  ........&.......
00000320: 24c0 0c00 a06e f22a 049f ffff f0e4 4cc0  $....n.*......L.
00000330: ffff ffff ff9d 6fb6 1f87 9c00 1000 0000  ......o.........
00000340: 3b01 0100 24c0 0c00 6b77 6f72 6b65 722f  ;...$...kworker/
00000350: 7538 3a35 0000 0000 24c0 0c00 7800 0000  u8:5....$...x...
00000360: 8000 0000 0000 0000 7377 6170 7065 722f  ........swapper/
00000370: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000380: 6ad2 3802 0401 1100 0000 0000 c800 384b  j.8...........8K
00000390: 029f ffff 7018 75c0 ffff ffff 00ac edce  ....p.u.........
000003a0: 039f ffff 0020 0000 0000 0000 c4de 0000  ..... ..........
000003b0: 0301 1100 0000 0000 c800 384b 029f ffff  ..........8K....
000003c0: 8a27 0100 3e01 1100 0000 0000 6b77 6f72  .'..>.......kwor
000003d0: 6b65 722f 303a 3200 0000 0000 48b4 0c00  ker/0:2.....H...
000003e0: 7800 0000 0100 0000 0000 0000 706d 0800  x...........pm..
000003f0: 3b01 0100 0000 0000 7377 6170 7065 722f  ;.......swapper/
00000400: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000410: 0000 0000 0000 0000 6b77 6f72 6b65 722f  ........kworker/
00000420: 303a 3200 0000 0000 48b4 0c00 7800 0000  0:2.....H...x...
00000430: 4636 0200 0201 0000 48b4 0c00 c800 384b  F6......H.....8K
00000440: 029f ffff 7018 75c0 ffff ffff ca56 0500  ....p.u......V..
00000450: 0401 0100 48b4 0c00 606a ad55 029f ffff  ....H...`j.U....
00000460: f0e4 4cc0 ffff ffff 002c 04d0 039f ffff  ..L......,......
00000470: 0020 0000 ffff ffff e435 0000 0301 0100  . .......5......
00000480: 48b4 0c00 606a ad55 029f ffff ca67 0000  H...`j.U.....g..
00000490: 3e01 0100 48b4 0c00 6b77 6f72 6b65 722f  >...H...kworker/
000004a0: 7538 3a35 0000 0000 24c0 0c00 7800 0000  u8:5....$...x...
000004b0: 0100 0000 0000 0000 e6fc 0200 0101 0000  ................
000004c0: 48b4 0c00 c800 384b 029f ffff 7018 75c0  H.....8K....p.u.
000004d0: ffff ffff 708f 0200 3b01 0100 48b4 0c00  ....p...;...H...
000004e0: 6b77 6f72 6b65 722f 303a 3200 0000 0000  kworker/0:2.....
000004f0: 48b4 0c00 7800 0000 8000 0000 0000 0000  H...x...........
00000500: 6b77 6f72 6b65 722f 7538 3a35 0000 0000  kworker/u8:5....
00000510: 24c0 0c00 7800 0000 0614 0100 0201 0000  $...x...........
00000520: 24c0 0c00 606a ad55 029f ffff f0e4 4cc0  $...`j.U......L.
00000530: ffff ffff ea7e 0c00 3e01 0100 24c0 0c00  .....~..>...$...
00000540: 646d 6372 7970 745f 7772 6974 652f 3200  dmcrypt_write/2.
00000550: 2601 0000 7800 0000 0100 0000 0100 0000  &...x...........
00000560: 4645 0200 0101 0000 24c0 0c00 606a ad55  FE......$...`j.U
00000570: 029f ffff f0e4 4cc0 ffff ffff b043 0900  ......L......C..
00000580: 3b01 0100 24c0 0c00 6b77 6f72 6b65 722f  ;...$...kworker/
00000590: 7538 3a35 0000 0000 24c0 0c00 7800 0000  u8:5....$...x...
000005a0: 8000 0000 0000 0000 7377 6170 7065 722f  ........swapper/
000005b0: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
000005c0: ca7a 3900 0401 1100 0000 0000 48bc d5a1  .z9.........H...
000005d0: 029f ffff 10e2 62bb ffff ffff 00e0 40d0  ......b.......@.
000005e0: 039f ffff 0020 0000 0000 0000 c4bb 0000  ..... ..........
000005f0: 0301 1100 0000 0000 48bc d5a1 029f ffff  ........H.......
00000600: 2aea 0000 3e01 1100 0000 0000 6b77 6f72  *...>.......kwor
00000610: 6b65 722f 303a 3148 0000 0000 cfc1 0c00  ker/0:1H........
00000620: 6400 0000 0100 0000 0000 0000 90bb 0600  d...............
00000630: 3b01 0100 0000 0000 7377 6170 7065 722f  ;.......swapper/
00000640: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
00000650: 0000 0000 0000 0000 6b77 6f72 6b65 722f  ........kworker/
00000660: 303a 3148 0000 0000 cfc1 0c00 6400 0000  0:1H........d...
00000670: 8617 0200 0201 0000 cfc1 0c00 48bc d5a1  ............H...
00000680: 029f ffff 10e2 62bb ffff ffff c68f 0400  ......b.........
00000690: 0101 0000 cfc1 0c00 48bc d5a1 029f ffff  ........H.......
000006a0: 10e2 62bb ffff ffff b063 0300 3b01 0100  ..b......c..;...
000006b0: cfc1 0c00 6b77 6f72 6b65 722f 303a 3148  ....kworker/0:1H
000006c0: 0000 0000 cfc1 0c00 6400 0000 8000 0000  ........d.......
000006d0: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
000006e0: 0000 0000 0000 0000 7800 0000 4a10 ad01  ........x...J...
000006f0: 3e01 1100 0000 0000 6a62 6432 2f64 6d2d  >.......jbd2/dm-
00000700: 312d 3800 0000 0000 6a01 0000 7800 0000  1-8.....j...x...
00000710: 0100 0000 0300 0000 ea27 b900 3e01 1100  .........'..>...
00000720: 0000 0000 7263 755f 7363 6865 6400 0000  ....rcu_sched...
00000730: 0000 0000 0d00 0000 7800 0000 0100 0000  ........x.......
00000740: 0200 0000 3d00 0000 2c00 0000 0000 0000  ....=...,.......
00000750: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000760: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000770: 0000 0000 0000 0000 0000 0000 0000 0000  ................
  )";

TEST_F(CpuReaderParsePagePayloadTest, ParseAbsoluteTimestamp) {
  auto page = PageFromXxd(g_abs_timestamp);

  // Hand-build a translation table that handles sched_switch for this test
  // page. We cannot reuse the test data format file, since the ftrace id for
  // sched_switch in this page is different.
  std::vector<Field> common_fields;
  {  // common_pid
    common_fields.emplace_back(Field{});
    Field* field = &common_fields.back();
    field->ftrace_offset = 4;
    field->ftrace_size = 4;
    field->ftrace_type = kFtraceCommonPid32;
    field->proto_field_id = 2;
    field->proto_field_type = ProtoSchemaType::kInt32;
    SetTranslationStrategy(field->ftrace_type, field->proto_field_type,
                           &field->strategy);
  }
  using Switch = protos::gen::SchedSwitchFtraceEvent;
  Event sched_switch_event{
      "sched_switch",
      "sched",
      {
          {8, 16, FtraceFieldType::kFtraceFixedCString, "prev_comm",
           Switch::kPrevCommFieldNumber, ProtoSchemaType::kString,
           TranslationStrategy::kInvalidTranslationStrategy},
          {24, 4, FtraceFieldType::kFtracePid32, "prev_pid",
           Switch::kPrevPidFieldNumber, ProtoSchemaType::kInt32,
           TranslationStrategy::kInvalidTranslationStrategy},
          {28, 4, FtraceFieldType::kFtraceInt32, "prev_prio",
           Switch::kPrevPrioFieldNumber, ProtoSchemaType::kInt32,
           TranslationStrategy::kInvalidTranslationStrategy},
          {32, 8, FtraceFieldType::kFtraceInt64, "prev_state",
           Switch::kPrevStateFieldNumber, ProtoSchemaType::kInt64,
           TranslationStrategy::kInvalidTranslationStrategy},
          {40, 16, FtraceFieldType::kFtraceFixedCString, "next_comm",
           Switch::kNextCommFieldNumber, ProtoSchemaType::kString,
           TranslationStrategy::kInvalidTranslationStrategy},
          {56, 4, FtraceFieldType::kFtracePid32, "next_pid",
           Switch::kNextPidFieldNumber, ProtoSchemaType::kInt32,
           TranslationStrategy::kInvalidTranslationStrategy},
          {60, 4, FtraceFieldType::kFtraceInt32, "next_prio",
           Switch::kNextPrioFieldNumber, ProtoSchemaType::kInt32,
           TranslationStrategy::kInvalidTranslationStrategy},
      },
      /*ftrace_event_id=*/315,
      /*proto_field_id=*/4,
      /*size=*/64};
  for (Field& field : sched_switch_event.fields) {
    SetTranslationStrategy(field.ftrace_type, field.proto_field_type,
                           &field.strategy);
  }
  std::vector<Event> events;
  events.emplace_back(std::move(sched_switch_event));

  NiceMock<MockFtraceProcfs> mock_ftrace;
  PrintkMap printk_formats;
  ProtoTranslationTable translation_table(
      &mock_ftrace, events, std::move(common_fields),
      ProtoTranslationTable::DefaultPageHeaderSpecForTesting(),
      InvalidCompactSchedEventFormatForTesting(), printk_formats);
  ProtoTranslationTable* table = &translation_table;

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();

  // There should be 9 sched_switch events within the above page.
  // We assert that all of their timestamps are exactly as expected.
  //
  // The key record that we're testing is an absolute timestamp
  // (RINGBUF_TYPE_TIME_STAMP) between the 3rd and 4th sched_switch events.
  //
  // This timestamp record starts at 0x334 bytes into the page.
  // The event header (first 4 bytes): 0xb66f9dff
  // -> type (bottom 5 bits): 31 (RINGBUF_TYPE_TIME_STAMP)
  // -> bottom 27 bits of ts: 0x5b37cef
  // Next 4 bytes have the top bits (28..59) of ts.
  // -> post-shift: 0x4e438f8000000
  // Adding the two parts of the timestamp, we get: 1376833332542703.
  //
  // The next event (sched_switch at 0x33c) after this timestamp has a
  // delta-timestamp of 0 in its event header, so we expect the 4th
  // sched_switch to have a timestamp of exactly 1376833332542703.
  EXPECT_EQ(bundle.event().size(), 9u);

  std::vector<uint64_t> switch_timestamps;
  for (const auto& e : bundle.event())
    switch_timestamps.push_back(e.timestamp());

  uint64_t expected_timestamps[] = {
      1376833327307547ull, 1376833327356434ull, 1376833332265799ull,
      1376833332542703ull, 1376833333729055ull, 1376833333757142ull,
      1376833333808564ull, 1376833333943445ull, 1376833333964012ull};

  ASSERT_THAT(switch_timestamps,
              testing::ElementsAreArray(expected_timestamps));
}

TEST(CpuReaderTest, TranslateBlockDeviceIDToUserspace) {
  const uint32_t kKernelBlockDeviceId = 271581216;
  const BlockDeviceID kUserspaceBlockDeviceId = 66336;
  const uint64_t k64BitKernelBlockDeviceId = 4442450946;
  const BlockDeviceID k64BitUserspaceBlockDeviceId =
      static_cast<BlockDeviceID>(17594983681026ULL);

  EXPECT_EQ(CpuReader::TranslateBlockDeviceIDToUserspace<uint32_t>(
                kKernelBlockDeviceId),
            kUserspaceBlockDeviceId);
  EXPECT_EQ(CpuReader::TranslateBlockDeviceIDToUserspace<uint64_t>(
                k64BitKernelBlockDeviceId),
            k64BitUserspaceBlockDeviceId);
}

// clang-format off
// # tracer: nop
// #
// # entries-in-buffer/entries-written: 1041/238740   #P:8
// #
// #                              _-----=> irqs-off
// #                             / _----=> need-resched
// #                            | / _---=> hardirq/softirq
// #                            || / _--=> preempt-depth
// #                            ||| /     delay
// #           TASK-PID   CPU#  ||||    TIMESTAMP  FUNCTION
// #              | |       |   ||||       |         |
//       android.bg-1668  [000] ...1 174991.234105: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//       android.bg-1668  [000] ...1 174991.234108: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_dirty_inode+0x48/0x68
//       android.bg-1668  [000] ...1 174991.234118: ext4_da_write_begin: dev 259,32 ino 2883605 pos 20480 len 4096 flags 0
//       android.bg-1668  [000] ...1 174991.234126: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//       android.bg-1668  [000] ...1 174991.234133: ext4_es_lookup_extent_enter: dev 259,32 ino 2883605 lblk 5
//       android.bg-1668  [000] ...1 174991.234135: ext4_es_lookup_extent_exit: dev 259,32 ino 2883605 found 1 [5/4294967290) 576460752303423487 H0x10
//       android.bg-1668  [000] ...2 174991.234140: ext4_da_reserve_space: dev 259,32 ino 2883605 mode 0100600 i_blocks 8 reserved_data_blocks 6 reserved_meta_blocks 0
//       android.bg-1668  [000] ...1 174991.234142: ext4_es_insert_extent: dev 259,32 ino 2883605 es [5/1) mapped 576460752303423487 status D
//       android.bg-1668  [000] ...1 174991.234153: ext4_da_write_end: dev 259,32 ino 2883605 pos 20480 len 4096 copied 4096
//       android.bg-1668  [000] ...1 174991.234158: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//       android.bg-1668  [000] ...1 174991.234160: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_dirty_inode+0x48/0x68
//       android.bg-1668  [000] ...1 174991.234170: ext4_da_write_begin: dev 259,32 ino 2883605 pos 24576 len 2968 flags 0
//       android.bg-1668  [000] ...1 174991.234178: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//       android.bg-1668  [000] ...1 174991.234184: ext4_es_lookup_extent_enter: dev 259,32 ino 2883605 lblk 6
//       android.bg-1668  [000] ...1 174991.234187: ext4_es_lookup_extent_exit: dev 259,32 ino 2883605 found 1 [6/4294967289) 576460752303423487 H0x10
//       android.bg-1668  [000] ...2 174991.234191: ext4_da_reserve_space: dev 259,32 ino 2883605 mode 0100600 i_blocks 8 reserved_data_blocks 7 reserved_meta_blocks 0
//       android.bg-1668  [000] ...1 174991.234193: ext4_es_insert_extent: dev 259,32 ino 2883605 es [6/1) mapped 576460752303423487 status D
//       android.bg-1668  [000] ...1 174991.234203: ext4_da_write_end: dev 259,32 ino 2883605 pos 24576 len 2968 copied 2968
//       android.bg-1668  [000] ...1 174991.234209: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//       android.bg-1668  [000] ...1 174991.234211: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_dirty_inode+0x48/0x68
//       android.bg-1668  [000] ...1 174991.234262: ext4_sync_file_enter: dev 259,32 ino 2883605 parent 2883592 datasync 0
//       android.bg-1668  [000] ...1 174991.234270: ext4_writepages: dev 259,32 ino 2883605 nr_to_write 9223372036854775807 pages_skipped 0 range_start 0 range_end 9223372036854775807 sync_mode 1 for_kupdate 0 range_cyclic 0 writeback_index 0
//       android.bg-1668  [000] ...1 174991.234287: ext4_journal_start: dev 259,32 blocks, 10 rsv_blocks, 0 caller ext4_writepages+0x6a4/0x119c
//       android.bg-1668  [000] ...1 174991.234294: ext4_da_write_pages: dev 259,32 ino 2883605 first_page 0 nr_to_write 9223372036854775807 sync_mode 1
//       android.bg-1668  [000] ...1 174991.234319: ext4_da_write_pages_extent: dev 259,32 ino 2883605 lblk 0 len 7 flags 0x200
//       android.bg-1668  [000] ...1 174991.234322: ext4_es_lookup_extent_enter: dev 259,32 ino 2883605 lblk 0
//       android.bg-1668  [000] ...1 174991.234324: ext4_es_lookup_extent_exit: dev 259,32 ino 2883605 found 1 [0/7) 576460752303423487 D0x10
//       android.bg-1668  [000] ...1 174991.234328: ext4_ext_map_blocks_enter: dev 259,32 ino 2883605 lblk 0 len 7 flags CREATE|DELALLOC|METADATA_NOFAIL
//       android.bg-1668  [000] ...1 174991.234341: ext4_request_blocks: dev 259,32 ino 2883605 flags HINT_DATA|DELALLOC_RESV|USE_RESV len 7 lblk 0 goal 11567104 lleft 0 lright 0 pleft 0 pright 0
//       android.bg-1668  [000] ...1 174991.234394: ext4_mballoc_prealloc: dev 259,32 inode 2883605 orig 353/0/7@0 result 65/25551/7@0
//       android.bg-1668  [000] ...1 174991.234400: ext4_allocate_blocks: dev 259,32 ino 2883605 flags HINT_DATA|DELALLOC_RESV|USE_RESV len 7 block 2155471 lblk 0 goal 11567104 lleft 0 lright 0 pleft 0 pright 0
//       android.bg-1668  [000] ...1 174991.234409: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller __ext4_ext_dirty+0x104/0x170
//       android.bg-1668  [000] ...1 174991.234420: ext4_get_reserved_cluster_alloc: dev 259,32 ino 2883605 lblk 0 len 7
//       android.bg-1668  [000] ...2 174991.234426: ext4_da_update_reserve_space: dev 259,32 ino 2883605 mode 0100600 i_blocks 8 used_blocks 7 reserved_data_blocks 7 reserved_meta_blocks 0 allocated_meta_blocks 0 quota_claim 1
//       android.bg-1668  [000] ...1 174991.234434: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_mark_dquot_dirty+0x80/0xd4
//       android.bg-1668  [000] ...1 174991.234441: ext4_es_lookup_extent_enter: dev 259,32 ino 3 lblk 1
//       android.bg-1668  [000] ...1 174991.234445: ext4_es_lookup_extent_exit: dev 259,32 ino 3 found 1 [0/2) 9255 W0x10
//       android.bg-1668  [000] ...1 174991.234456: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_mark_dquot_dirty+0x80/0xd4
//       android.bg-1668  [000] ...1 174991.234460: ext4_es_lookup_extent_enter: dev 259,32 ino 4 lblk 1
//       android.bg-1668  [000] ...1 174991.234463: ext4_es_lookup_extent_exit: dev 259,32 ino 4 found 1 [0/2) 9257 W0x10
//       android.bg-1668  [000] ...1 174991.234471: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//       android.bg-1668  [000] ...1 174991.234474: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_dirty_inode+0x48/0x68
//       android.bg-1668  [000] ...1 174991.234481: ext4_ext_map_blocks_exit: dev 259,32 ino 2883605 flags CREATE|DELALLOC|METADATA_NOFAIL lblk 0 pblk 2155471 len 7 mflags NM ret 7
//       android.bg-1668  [000] ...1 174991.234484: ext4_es_insert_extent: dev 259,32 ino 2883605 es [0/7) mapped 2155471 status W
//       android.bg-1668  [000] ...1 174991.234547: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_writepages+0xdc0/0x119c
//       android.bg-1668  [000] ...1 174991.234604: ext4_journal_start: dev 259,32 blocks, 10 rsv_blocks, 0 caller ext4_writepages+0x6a4/0x119c
//       android.bg-1668  [000] ...1 174991.234609: ext4_da_write_pages: dev 259,32 ino 2883605 first_page 7 nr_to_write 9223372036854775800 sync_mode 1
//       android.bg-1668  [000] ...1 174991.234876: ext4_writepages_result: dev 259,32 ino 2883605 ret 0 pages_written 7 pages_skipped 0 sync_mode 1 writeback_index 7
//    Profile Saver-5504  [000] ...1 175002.711928: ext4_discard_preallocations: dev 259,32 ino 1311176
//    Profile Saver-5504  [000] ...1 175002.714165: ext4_begin_ordered_truncate: dev 259,32 ino 1311176 new_size 0
//    Profile Saver-5504  [000] ...1 175002.714172: ext4_journal_start: dev 259,32 blocks, 3 rsv_blocks, 0 caller ext4_setattr+0x5b4/0x788
//    Profile Saver-5504  [000] ...1 175002.714218: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_setattr+0x65c/0x788
//    Profile Saver-5504  [000] ...1 175002.714277: ext4_invalidatepage: dev 259,32 ino 1311176 page_index 0 offset 0 length 4096
//    Profile Saver-5504  [000] ...1 175002.714281: ext4_releasepage: dev 259,32 ino 1311176 page_index 0
//    Profile Saver-5504  [000] ...1 175002.714295: ext4_invalidatepage: dev 259,32 ino 1311176 page_index 1 offset 0 length 4096
//    Profile Saver-5504  [000] ...1 175002.714296: ext4_releasepage: dev 259,32 ino 1311176 page_index 1
//    Profile Saver-5504  [000] ...1 175002.714315: ext4_truncate_enter: dev 259,32 ino 1311176 blocks 24
//    Profile Saver-5504  [000] ...1 175002.714318: ext4_journal_start: dev 259,32 blocks, 10 rsv_blocks, 0 caller ext4_truncate+0x258/0x4b8
//    Profile Saver-5504  [000] ...1 175002.714322: ext4_discard_preallocations: dev 259,32 ino 1311176
//    Profile Saver-5504  [000] ...1 175002.714324: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_ext_truncate+0x24/0xc8
//    Profile Saver-5504  [000] ...1 175002.714328: ext4_es_remove_extent: dev 259,32 ino 1311176 es [0/4294967295)
//    Profile Saver-5504  [000] ...1 175002.714335: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_ext_remove_space+0x60/0x1180
//    Profile Saver-5504  [000] ...1 175002.714338: ext4_ext_remove_space: dev 259,32 ino 1311176 since 0 end 4294967294 depth 0
//    Profile Saver-5504  [000] ...1 175002.714347: ext4_ext_rm_leaf: dev 259,32 ino 1311176 start_lblk 0 last_extent [0(5276994), 2]partial_cluster 0
//    Profile Saver-5504  [000] ...1 175002.714351: ext4_remove_blocks: dev 259,32 ino 1311176 extent [0(5276994), 2]from 0 to 1 partial_cluster 0
//    Profile Saver-5504  [000] ...1 175002.714354: ext4_free_blocks: dev 259,32 ino 1311176 mode 0100600 block 5276994 count 2 flags 1ST_CLUSTER
//    Profile Saver-5504  [000] ...1 175002.714365: ext4_mballoc_free: dev 259,32 inode 1311176 extent 161/1346/2
//    Profile Saver-5504  [000] ...1 175002.714382: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_mark_dquot_dirty+0x80/0xd4
//    Profile Saver-5504  [000] ...1 175002.714391: ext4_es_lookup_extent_enter: dev 259,32 ino 3 lblk 4
//    Profile Saver-5504  [000] ...1 175002.714394: ext4_es_lookup_extent_exit: dev 259,32 ino 3 found 1 [4/1) 557094 W0x10
//    Profile Saver-5504  [000] ...1 175002.714402: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_mark_dquot_dirty+0x80/0xd4
//    Profile Saver-5504  [000] ...1 175002.714404: ext4_es_lookup_extent_enter: dev 259,32 ino 4 lblk 8
//    Profile Saver-5504  [000] ...1 175002.714406: ext4_es_lookup_extent_exit: dev 259,32 ino 4 found 1 [8/3) 7376914 W0x10
//    Profile Saver-5504  [000] ...1 175002.714413: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.714414: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.714420: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller __ext4_ext_dirty+0x104/0x170
//    Profile Saver-5504  [000] ...1 175002.714423: ext4_ext_remove_space_done: dev 259,32 ino 1311176 since 0 end 4294967294 depth 0 partial 0 remaining_entries 0
//    Profile Saver-5504  [000] ...1 175002.714425: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller __ext4_ext_dirty+0x104/0x170
//    Profile Saver-5504  [000] ...1 175002.714433: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_truncate+0x3c4/0x4b8
//    Profile Saver-5504  [000] ...1 175002.714436: ext4_truncate_exit: dev 259,32 ino 1311176 blocks 8
//    Profile Saver-5504  [000] ...1 175002.714437: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.714438: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.714462: ext4_da_write_begin: dev 259,32 ino 1311176 pos 0 len 4 flags 0
//    Profile Saver-5504  [000] ...1 175002.714472: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.714477: ext4_es_lookup_extent_enter: dev 259,32 ino 1311176 lblk 0
//    Profile Saver-5504  [000] ...1 175002.714477: ext4_es_lookup_extent_exit: dev 259,32 ino 1311176 found 0 [0/0) 0
//    Profile Saver-5504  [000] ...1 175002.714480: ext4_ext_map_blocks_enter: dev 259,32 ino 1311176 lblk 0 len 1 flags
//    Profile Saver-5504  [000] ...1 175002.714485: ext4_es_find_delayed_extent_range_enter: dev 259,32 ino 1311176 lblk 0
//    Profile Saver-5504  [000] ...1 175002.714488: ext4_es_find_delayed_extent_range_exit: dev 259,32 ino 1311176 es [0/0) mapped 0 status
//    Profile Saver-5504  [000] ...1 175002.714490: ext4_es_insert_extent: dev 259,32 ino 1311176 es [0/4294967295) mapped 576460752303423487 status H
//    Profile Saver-5504  [000] ...1 175002.714495: ext4_ext_map_blocks_exit: dev 259,32 ino 1311176 flags  lblk 0 pblk 4294967296 len 1 mflags  ret 0
//    Profile Saver-5504  [000] ...2 175002.714501: ext4_da_reserve_space: dev 259,32 ino 1311176 mode 0100600 i_blocks 8 reserved_data_blocks 1 reserved_meta_blocks 0
//    Profile Saver-5504  [000] ...1 175002.714505: ext4_es_insert_extent: dev 259,32 ino 1311176 es [0/1) mapped 576460752303423487 status D
//    Profile Saver-5504  [000] ...1 175002.714513: ext4_da_write_end: dev 259,32 ino 1311176 pos 0 len 4 copied 4
//    Profile Saver-5504  [000] ...1 175002.714519: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.714520: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.714527: ext4_da_write_begin: dev 259,32 ino 1311176 pos 4 len 4 flags 0
//    Profile Saver-5504  [000] ...1 175002.714529: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.714531: ext4_da_write_end: dev 259,32 ino 1311176 pos 4 len 4 copied 4
//    Profile Saver-5504  [000] ...1 175002.714532: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.714532: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.715313: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.715322: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.723849: ext4_da_write_begin: dev 259,32 ino 1311176 pos 8 len 5 flags 0
//    Profile Saver-5504  [000] ...1 175002.723862: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.723873: ext4_da_write_end: dev 259,32 ino 1311176 pos 8 len 5 copied 5
//    Profile Saver-5504  [000] ...1 175002.723877: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.723879: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.726857: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.726867: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.726881: ext4_da_write_begin: dev 259,32 ino 1311176 pos 13 len 4 flags 0
//    Profile Saver-5504  [000] ...1 175002.726883: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.726890: ext4_da_write_end: dev 259,32 ino 1311176 pos 13 len 4 copied 4
//    Profile Saver-5504  [000] ...1 175002.726892: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.726892: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.726900: ext4_da_write_begin: dev 259,32 ino 1311176 pos 17 len 4079 flags 0
//    Profile Saver-5504  [000] ...1 175002.726901: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.726904: ext4_da_write_end: dev 259,32 ino 1311176 pos 17 len 4079 copied 4079
//    Profile Saver-5504  [000] ...1 175002.726905: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.726906: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.726908: ext4_da_write_begin: dev 259,32 ino 1311176 pos 4096 len 2780 flags 0
//    Profile Saver-5504  [000] ...1 175002.726916: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.726921: ext4_es_lookup_extent_enter: dev 259,32 ino 1311176 lblk 1
//    Profile Saver-5504  [000] ...1 175002.726924: ext4_es_lookup_extent_exit: dev 259,32 ino 1311176 found 1 [1/4294967294) 576460752303423487 H0x10
//    Profile Saver-5504  [000] ...2 175002.726931: ext4_da_reserve_space: dev 259,32 ino 1311176 mode 0100600 i_blocks 8 reserved_data_blocks 2 reserved_meta_blocks 0
//    Profile Saver-5504  [000] ...1 175002.726933: ext4_es_insert_extent: dev 259,32 ino 1311176 es [1/1) mapped 576460752303423487 status D
//    Profile Saver-5504  [000] ...1 175002.726940: ext4_da_write_end: dev 259,32 ino 1311176 pos 4096 len 2780 copied 2780
//    Profile Saver-5504  [000] ...1 175002.726941: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.726942: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//   d.process.acor-27885 [000] ...1 175018.227675: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//   d.process.acor-27885 [000] ...1 175018.227699: ext4_mark_inode_dirty: dev 259,32 ino 3278189 caller ext4_dirty_inode+0x48/0x68
//   d.process.acor-27885 [000] ...1 175018.227839: ext4_sync_file_enter: dev 259,32 ino 3278183 parent 3277001 datasync 1
//   d.process.acor-27885 [000] ...1 175018.227847: ext4_writepages: dev 259,32 ino 3278183 nr_to_write 9223372036854775807 pages_skipped 0 range_start 0 range_end 9223372036854775807 sync_mode 1 for_kupdate 0 range_cyclic 0 writeback_index 2
//   d.process.acor-27885 [000] ...1 175018.227852: ext4_writepages_result: dev 259,32 ino 3278183 ret 0 pages_written 0 pages_skipped 0 sync_mode 1 writeback_index 2
// clang-format on

static ExamplePage g_full_page_sched_switch{
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

TEST_F(CpuReaderParsePagePayloadTest, ParseFullPageSchedSwitch) {
  const ExamplePage* test_case = &g_full_page_sched_switch;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();
  EXPECT_EQ(bundle.event().size(), 59u);
}

// clang-format off
// # tracer: nop
// #
// # entries-in-buffer/entries-written: 18/18   #P:8
// #
// #                              _-----=> irqs-off
// #                             / _----=> need-resched
// #                            | / _---=> hardirq/softirq
// #                            || / _--=> preempt-depth
// #                            ||| /     delay
// #           TASK-PID   CPU#  ||||    TIMESTAMP  FUNCTION
// #              | |       |   ||||       |         |
//            <...>-9290  [000] ....  1352.654573: suspend_resume: sync_filesystems[0] end
//            <...>-9290  [000] ....  1352.665366: suspend_resume: freeze_processes[0] begin
//            <...>-9290  [000] ....  1352.699711: suspend_resume: freeze_processes[0] end
//            <...>-9290  [000] ....  1352.699718: suspend_resume: suspend_enter[1] end
//            <...>-9290  [000] ....  1352.699723: suspend_resume: dpm_prepare[2] begin
//            <...>-9290  [000] ....  1352.703470: suspend_resume: dpm_prepare[2] end
//            <...>-9290  [000] ....  1352.703477: suspend_resume: dpm_suspend[2] begin
//            <...>-9290  [000] ....  1352.720107: suspend_resume: dpm_resume[16] end
//            <...>-9290  [000] ....  1352.720113: suspend_resume: dpm_complete[16] begin
//            <...>-9290  [000] .n..  1352.724540: suspend_resume: dpm_complete[16] end
//            <...>-9290  [000] ....  1352.724567: suspend_resume: resume_console[1] begin
//            <...>-9290  [000] ....  1352.724570: suspend_resume: resume_console[1] end
//            <...>-9290  [000] ....  1352.724574: suspend_resume: thaw_processes[0] begin
// clang-format on

static ExamplePage g_suspend_resume{
    "synthetic",
    R"(00000000: edba 155a 3201 0000 7401 0000 0000 0000  ...Z2...t.......
00000010: 7e58 22cd 1201 0000 0600 0000 ac00 0000  ~X".............
00000020: 4a24 0000 5a7a f504 85ff ffff 0000 0000  J$..Zz..........
00000030: 0017 0000 c621 9614 ac00 0000 4a24 0000  .....!......J$..
00000040: 1c7a f504 85ff ffff 0000 0000 0100 0000  .z..............
00000050: e6f1 8141 ac00 0000 4a24 0000 1c7a f504  ...A....J$...z..
00000060: 85ff ffff 0000 0000 0000 0000 8682 0300  ................
00000070: ac00 0000 4a24 0000 4c7a f504 85ff ffff  ....J$..Lz......
00000080: 0100 0000 0063 755f 0657 0200 ac00 0000  .....cu_.W......
00000090: 4a24 0000 8ad5 0105 85ff ffff 0200 0000  J$..............
000000a0: 0100 0000 06b5 2507 ac00 0000 4a24 0000  ......%.....J$..
000000b0: 8ad5 0105 85ff ffff 0200 0000 0000 0000  ................
000000c0: 460d 0300 ac00 0000 4a24 0000 51d5 0105  F.......J$..Q...
000000d0: 85ff ffff 0200 0000 0117 0000 c63e b81f  .............>..
000000e0: ac00 0000 4a24 0000 7fd5 0105 85ff ffff  ....J$..........
000000f0: 1000 0000 0010 0b00 a6f9 0200 ac00 0000  ................
00000100: 4a24 0000 96d5 0105 85ff ffff 1000 0000  J$..............
00000110: 01c0 1f00 a6dd 7108 ac00 0400 4a24 0000  ......q.....J$..
00000120: 96d5 0105 85ff ffff 1000 0000 0000 0000  ................
00000130: c6f1 0c00 ac00 0000 4a24 0000 3d7a f504  ........J$..=z..
00000140: 85ff ffff 0100 0000 01ea 24d5 a66c 0100  ..........$..l..
00000150: ac00 0000 4a24 0000 3d7a f504 85ff ffff  ....J$..=z......
00000160: 0100 0000 0000 0001 6636 0200 ac00 0000  ........f6......
00000170: 4a24 0000 d178 f504 85ff ffff 0000 0000  J$...x..........
00000180: 0100 0000 0000 0000 0000 0000 0000 0000  ................
00000190: 0000 0000 0000 0000 0000 0000 0000 0000  ................
)"};

TEST_F(CpuReaderParsePagePayloadTest, ParseSuspendResume) {
  const ExamplePage* test_case = &g_suspend_resume;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("power", "suspend_resume")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());
  ASSERT_TRUE(page_header.has_value());

  CpuReader::ParsePagePayload(parse_pos, &page_header.value(), table,
                              &ds_config, CreateBundler(ds_config), &metadata_,
                              &last_read_event_ts_);
  auto bundle = GetBundle();
  ASSERT_EQ(bundle.event().size(), 13u);
  EXPECT_EQ(bundle.event()[0].suspend_resume().action(), "sync_filesystems");
  EXPECT_EQ(bundle.event()[1].suspend_resume().action(), "freeze_processes");
  EXPECT_EQ(bundle.event()[2].suspend_resume().action(), "freeze_processes");
  EXPECT_EQ(bundle.event()[3].suspend_resume().action(), "suspend_enter");
  // dpm_prepare deliberately missing from:
  // src/traced/probes/ftrace/test/data/synthetic/printk_formats to ensure we
  // handle that case correctly.
  EXPECT_EQ(bundle.event()[4].suspend_resume().action(), "");
}

// clang-format off
// # tracer: nop
// #
// # entries-in-buffer/entries-written: 1041/238740   #P:8
// #
// #                              _-----=> irqs-off
// #                             / _----=> need-resched
// #                            | / _---=> hardirq/softirq
// #                            || / _--=> preempt-depth
// #                            ||| /     delay
// #           TASK-PID   CPU#  ||||    TIMESTAMP  FUNCTION
// #              | |       |   ||||       |         |
//       android.bg-1668  [000] ...1 174991.234105: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//       android.bg-1668  [000] ...1 174991.234108: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_dirty_inode+0x48/0x68
//       android.bg-1668  [000] ...1 174991.234118: ext4_da_write_begin: dev 259,32 ino 2883605 pos 20480 len 4096 flags 0
//       android.bg-1668  [000] ...1 174991.234126: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//       android.bg-1668  [000] ...1 174991.234133: ext4_es_lookup_extent_enter: dev 259,32 ino 2883605 lblk 5
//       android.bg-1668  [000] ...1 174991.234135: ext4_es_lookup_extent_exit: dev 259,32 ino 2883605 found 1 [5/4294967290) 576460752303423487 H0x10
//       android.bg-1668  [000] ...2 174991.234140: ext4_da_reserve_space: dev 259,32 ino 2883605 mode 0100600 i_blocks 8 reserved_data_blocks 6 reserved_meta_blocks 0
//       android.bg-1668  [000] ...1 174991.234142: ext4_es_insert_extent: dev 259,32 ino 2883605 es [5/1) mapped 576460752303423487 status D
//       android.bg-1668  [000] ...1 174991.234153: ext4_da_write_end: dev 259,32 ino 2883605 pos 20480 len 4096 copied 4096
//       android.bg-1668  [000] ...1 174991.234158: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//       android.bg-1668  [000] ...1 174991.234160: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_dirty_inode+0x48/0x68
//       android.bg-1668  [000] ...1 174991.234170: ext4_da_write_begin: dev 259,32 ino 2883605 pos 24576 len 2968 flags 0
//       android.bg-1668  [000] ...1 174991.234178: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//       android.bg-1668  [000] ...1 174991.234184: ext4_es_lookup_extent_enter: dev 259,32 ino 2883605 lblk 6
//       android.bg-1668  [000] ...1 174991.234187: ext4_es_lookup_extent_exit: dev 259,32 ino 2883605 found 1 [6/4294967289) 576460752303423487 H0x10
//       android.bg-1668  [000] ...2 174991.234191: ext4_da_reserve_space: dev 259,32 ino 2883605 mode 0100600 i_blocks 8 reserved_data_blocks 7 reserved_meta_blocks 0
//       android.bg-1668  [000] ...1 174991.234193: ext4_es_insert_extent: dev 259,32 ino 2883605 es [6/1) mapped 576460752303423487 status D
//       android.bg-1668  [000] ...1 174991.234203: ext4_da_write_end: dev 259,32 ino 2883605 pos 24576 len 2968 copied 2968
//       android.bg-1668  [000] ...1 174991.234209: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//       android.bg-1668  [000] ...1 174991.234211: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_dirty_inode+0x48/0x68
//       android.bg-1668  [000] ...1 174991.234262: ext4_sync_file_enter: dev 259,32 ino 2883605 parent 2883592 datasync 0
//       android.bg-1668  [000] ...1 174991.234270: ext4_writepages: dev 259,32 ino 2883605 nr_to_write 9223372036854775807 pages_skipped 0 range_start 0 range_end 9223372036854775807 sync_mode 1 for_kupdate 0 range_cyclic 0 writeback_index 0
//       android.bg-1668  [000] ...1 174991.234287: ext4_journal_start: dev 259,32 blocks, 10 rsv_blocks, 0 caller ext4_writepages+0x6a4/0x119c
//       android.bg-1668  [000] ...1 174991.234294: ext4_da_write_pages: dev 259,32 ino 2883605 first_page 0 nr_to_write 9223372036854775807 sync_mode 1
//       android.bg-1668  [000] ...1 174991.234319: ext4_da_write_pages_extent: dev 259,32 ino 2883605 lblk 0 len 7 flags 0x200
//       android.bg-1668  [000] ...1 174991.234322: ext4_es_lookup_extent_enter: dev 259,32 ino 2883605 lblk 0
//       android.bg-1668  [000] ...1 174991.234324: ext4_es_lookup_extent_exit: dev 259,32 ino 2883605 found 1 [0/7) 576460752303423487 D0x10
//       android.bg-1668  [000] ...1 174991.234328: ext4_ext_map_blocks_enter: dev 259,32 ino 2883605 lblk 0 len 7 flags CREATE|DELALLOC|METADATA_NOFAIL
//       android.bg-1668  [000] ...1 174991.234341: ext4_request_blocks: dev 259,32 ino 2883605 flags HINT_DATA|DELALLOC_RESV|USE_RESV len 7 lblk 0 goal 11567104 lleft 0 lright 0 pleft 0 pright 0
//       android.bg-1668  [000] ...1 174991.234394: ext4_mballoc_prealloc: dev 259,32 inode 2883605 orig 353/0/7@0 result 65/25551/7@0
//       android.bg-1668  [000] ...1 174991.234400: ext4_allocate_blocks: dev 259,32 ino 2883605 flags HINT_DATA|DELALLOC_RESV|USE_RESV len 7 block 2155471 lblk 0 goal 11567104 lleft 0 lright 0 pleft 0 pright 0
//       android.bg-1668  [000] ...1 174991.234409: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller __ext4_ext_dirty+0x104/0x170
//       android.bg-1668  [000] ...1 174991.234420: ext4_get_reserved_cluster_alloc: dev 259,32 ino 2883605 lblk 0 len 7
//       android.bg-1668  [000] ...2 174991.234426: ext4_da_update_reserve_space: dev 259,32 ino 2883605 mode 0100600 i_blocks 8 used_blocks 7 reserved_data_blocks 7 reserved_meta_blocks 0 allocated_meta_blocks 0 quota_claim 1
//       android.bg-1668  [000] ...1 174991.234434: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_mark_dquot_dirty+0x80/0xd4
//       android.bg-1668  [000] ...1 174991.234441: ext4_es_lookup_extent_enter: dev 259,32 ino 3 lblk 1
//       android.bg-1668  [000] ...1 174991.234445: ext4_es_lookup_extent_exit: dev 259,32 ino 3 found 1 [0/2) 9255 W0x10
//       android.bg-1668  [000] ...1 174991.234456: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_mark_dquot_dirty+0x80/0xd4
//       android.bg-1668  [000] ...1 174991.234460: ext4_es_lookup_extent_enter: dev 259,32 ino 4 lblk 1
//       android.bg-1668  [000] ...1 174991.234463: ext4_es_lookup_extent_exit: dev 259,32 ino 4 found 1 [0/2) 9257 W0x10
//       android.bg-1668  [000] ...1 174991.234471: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//       android.bg-1668  [000] ...1 174991.234474: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_dirty_inode+0x48/0x68
//       android.bg-1668  [000] ...1 174991.234481: ext4_ext_map_blocks_exit: dev 259,32 ino 2883605 flags CREATE|DELALLOC|METADATA_NOFAIL lblk 0 pblk 2155471 len 7 mflags NM ret 7
//       android.bg-1668  [000] ...1 174991.234484: ext4_es_insert_extent: dev 259,32 ino 2883605 es [0/7) mapped 2155471 status W
//       android.bg-1668  [000] ...1 174991.234547: ext4_mark_inode_dirty: dev 259,32 ino 2883605 caller ext4_writepages+0xdc0/0x119c
//       android.bg-1668  [000] ...1 174991.234604: ext4_journal_start: dev 259,32 blocks, 10 rsv_blocks, 0 caller ext4_writepages+0x6a4/0x119c
//       android.bg-1668  [000] ...1 174991.234609: ext4_da_write_pages: dev 259,32 ino 2883605 first_page 7 nr_to_write 9223372036854775800 sync_mode 1
//       android.bg-1668  [000] ...1 174991.234876: ext4_writepages_result: dev 259,32 ino 2883605 ret 0 pages_written 7 pages_skipped 0 sync_mode 1 writeback_index 7
//    Profile Saver-5504  [000] ...1 175002.711928: ext4_discard_preallocations: dev 259,32 ino 1311176
//    Profile Saver-5504  [000] ...1 175002.714165: ext4_begin_ordered_truncate: dev 259,32 ino 1311176 new_size 0
//    Profile Saver-5504  [000] ...1 175002.714172: ext4_journal_start: dev 259,32 blocks, 3 rsv_blocks, 0 caller ext4_setattr+0x5b4/0x788
//    Profile Saver-5504  [000] ...1 175002.714218: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_setattr+0x65c/0x788
//    Profile Saver-5504  [000] ...1 175002.714277: ext4_invalidatepage: dev 259,32 ino 1311176 page_index 0 offset 0 length 4096
//    Profile Saver-5504  [000] ...1 175002.714281: ext4_releasepage: dev 259,32 ino 1311176 page_index 0
//    Profile Saver-5504  [000] ...1 175002.714295: ext4_invalidatepage: dev 259,32 ino 1311176 page_index 1 offset 0 length 4096
//    Profile Saver-5504  [000] ...1 175002.714296: ext4_releasepage: dev 259,32 ino 1311176 page_index 1
//    Profile Saver-5504  [000] ...1 175002.714315: ext4_truncate_enter: dev 259,32 ino 1311176 blocks 24
//    Profile Saver-5504  [000] ...1 175002.714318: ext4_journal_start: dev 259,32 blocks, 10 rsv_blocks, 0 caller ext4_truncate+0x258/0x4b8
//    Profile Saver-5504  [000] ...1 175002.714322: ext4_discard_preallocations: dev 259,32 ino 1311176
//    Profile Saver-5504  [000] ...1 175002.714324: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_ext_truncate+0x24/0xc8
//    Profile Saver-5504  [000] ...1 175002.714328: ext4_es_remove_extent: dev 259,32 ino 1311176 es [0/4294967295)
//    Profile Saver-5504  [000] ...1 175002.714335: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_ext_remove_space+0x60/0x1180
//    Profile Saver-5504  [000] ...1 175002.714338: ext4_ext_remove_space: dev 259,32 ino 1311176 since 0 end 4294967294 depth 0
//    Profile Saver-5504  [000] ...1 175002.714347: ext4_ext_rm_leaf: dev 259,32 ino 1311176 start_lblk 0 last_extent [0(5276994), 2]partial_cluster 0
//    Profile Saver-5504  [000] ...1 175002.714351: ext4_remove_blocks: dev 259,32 ino 1311176 extent [0(5276994), 2]from 0 to 1 partial_cluster 0
//    Profile Saver-5504  [000] ...1 175002.714354: ext4_free_blocks: dev 259,32 ino 1311176 mode 0100600 block 5276994 count 2 flags 1ST_CLUSTER
//    Profile Saver-5504  [000] ...1 175002.714365: ext4_mballoc_free: dev 259,32 inode 1311176 extent 161/1346/2
//    Profile Saver-5504  [000] ...1 175002.714382: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_mark_dquot_dirty+0x80/0xd4
//    Profile Saver-5504  [000] ...1 175002.714391: ext4_es_lookup_extent_enter: dev 259,32 ino 3 lblk 4
//    Profile Saver-5504  [000] ...1 175002.714394: ext4_es_lookup_extent_exit: dev 259,32 ino 3 found 1 [4/1) 557094 W0x10
//    Profile Saver-5504  [000] ...1 175002.714402: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_mark_dquot_dirty+0x80/0xd4
//    Profile Saver-5504  [000] ...1 175002.714404: ext4_es_lookup_extent_enter: dev 259,32 ino 4 lblk 8
//    Profile Saver-5504  [000] ...1 175002.714406: ext4_es_lookup_extent_exit: dev 259,32 ino 4 found 1 [8/3) 7376914 W0x10
//    Profile Saver-5504  [000] ...1 175002.714413: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.714414: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.714420: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller __ext4_ext_dirty+0x104/0x170
//    Profile Saver-5504  [000] ...1 175002.714423: ext4_ext_remove_space_done: dev 259,32 ino 1311176 since 0 end 4294967294 depth 0 partial 0 remaining_entries 0
//    Profile Saver-5504  [000] ...1 175002.714425: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller __ext4_ext_dirty+0x104/0x170
//    Profile Saver-5504  [000] ...1 175002.714433: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_truncate+0x3c4/0x4b8
//    Profile Saver-5504  [000] ...1 175002.714436: ext4_truncate_exit: dev 259,32 ino 1311176 blocks 8
//    Profile Saver-5504  [000] ...1 175002.714437: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.714438: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.714462: ext4_da_write_begin: dev 259,32 ino 1311176 pos 0 len 4 flags 0
//    Profile Saver-5504  [000] ...1 175002.714472: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.714477: ext4_es_lookup_extent_enter: dev 259,32 ino 1311176 lblk 0
//    Profile Saver-5504  [000] ...1 175002.714477: ext4_es_lookup_extent_exit: dev 259,32 ino 1311176 found 0 [0/0) 0
//    Profile Saver-5504  [000] ...1 175002.714480: ext4_ext_map_blocks_enter: dev 259,32 ino 1311176 lblk 0 len 1 flags
//    Profile Saver-5504  [000] ...1 175002.714485: ext4_es_find_delayed_extent_range_enter: dev 259,32 ino 1311176 lblk 0
//    Profile Saver-5504  [000] ...1 175002.714488: ext4_es_find_delayed_extent_range_exit: dev 259,32 ino 1311176 es [0/0) mapped 0 status
//    Profile Saver-5504  [000] ...1 175002.714490: ext4_es_insert_extent: dev 259,32 ino 1311176 es [0/4294967295) mapped 576460752303423487 status H
//    Profile Saver-5504  [000] ...1 175002.714495: ext4_ext_map_blocks_exit: dev 259,32 ino 1311176 flags  lblk 0 pblk 4294967296 len 1 mflags  ret 0
//    Profile Saver-5504  [000] ...2 175002.714501: ext4_da_reserve_space: dev 259,32 ino 1311176 mode 0100600 i_blocks 8 reserved_data_blocks 1 reserved_meta_blocks 0
//    Profile Saver-5504  [000] ...1 175002.714505: ext4_es_insert_extent: dev 259,32 ino 1311176 es [0/1) mapped 576460752303423487 status D
//    Profile Saver-5504  [000] ...1 175002.714513: ext4_da_write_end: dev 259,32 ino 1311176 pos 0 len 4 copied 4
//    Profile Saver-5504  [000] ...1 175002.714519: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.714520: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.714527: ext4_da_write_begin: dev 259,32 ino 1311176 pos 4 len 4 flags 0
//    Profile Saver-5504  [000] ...1 175002.714529: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.714531: ext4_da_write_end: dev 259,32 ino 1311176 pos 4 len 4 copied 4
//    Profile Saver-5504  [000] ...1 175002.714532: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.714532: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.715313: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.715322: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.723849: ext4_da_write_begin: dev 259,32 ino 1311176 pos 8 len 5 flags 0
//    Profile Saver-5504  [000] ...1 175002.723862: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.723873: ext4_da_write_end: dev 259,32 ino 1311176 pos 8 len 5 copied 5
//    Profile Saver-5504  [000] ...1 175002.723877: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.723879: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.726857: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.726867: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.726881: ext4_da_write_begin: dev 259,32 ino 1311176 pos 13 len 4 flags 0
//    Profile Saver-5504  [000] ...1 175002.726883: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.726890: ext4_da_write_end: dev 259,32 ino 1311176 pos 13 len 4 copied 4
//    Profile Saver-5504  [000] ...1 175002.726892: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.726892: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.726900: ext4_da_write_begin: dev 259,32 ino 1311176 pos 17 len 4079 flags 0
//    Profile Saver-5504  [000] ...1 175002.726901: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.726904: ext4_da_write_end: dev 259,32 ino 1311176 pos 17 len 4079 copied 4079
//    Profile Saver-5504  [000] ...1 175002.726905: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.726906: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//    Profile Saver-5504  [000] ...1 175002.726908: ext4_da_write_begin: dev 259,32 ino 1311176 pos 4096 len 2780 flags 0
//    Profile Saver-5504  [000] ...1 175002.726916: ext4_journal_start: dev 259,32 blocks, 1 rsv_blocks, 0 caller ext4_da_write_begin+0x3d4/0x518
//    Profile Saver-5504  [000] ...1 175002.726921: ext4_es_lookup_extent_enter: dev 259,32 ino 1311176 lblk 1
//    Profile Saver-5504  [000] ...1 175002.726924: ext4_es_lookup_extent_exit: dev 259,32 ino 1311176 found 1 [1/4294967294) 576460752303423487 H0x10
//    Profile Saver-5504  [000] ...2 175002.726931: ext4_da_reserve_space: dev 259,32 ino 1311176 mode 0100600 i_blocks 8 reserved_data_blocks 2 reserved_meta_blocks 0
//    Profile Saver-5504  [000] ...1 175002.726933: ext4_es_insert_extent: dev 259,32 ino 1311176 es [1/1) mapped 576460752303423487 status D
//    Profile Saver-5504  [000] ...1 175002.726940: ext4_da_write_end: dev 259,32 ino 1311176 pos 4096 len 2780 copied 2780
//    Profile Saver-5504  [000] ...1 175002.726941: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//    Profile Saver-5504  [000] ...1 175002.726942: ext4_mark_inode_dirty: dev 259,32 ino 1311176 caller ext4_dirty_inode+0x48/0x68
//   d.process.acor-27885 [000] ...1 175018.227675: ext4_journal_start: dev 259,32 blocks, 2 rsv_blocks, 0 caller ext4_dirty_inode+0x30/0x68
//   d.process.acor-27885 [000] ...1 175018.227699: ext4_mark_inode_dirty: dev 259,32 ino 3278189 caller ext4_dirty_inode+0x48/0x68
//   d.process.acor-27885 [000] ...1 175018.227839: ext4_sync_file_enter: dev 259,32 ino 3278183 parent 3277001 datasync 1
//   d.process.acor-27885 [000] ...1 175018.227847: ext4_writepages: dev 259,32 ino 3278183 nr_to_write 9223372036854775807 pages_skipped 0 range_start 0 range_end 9223372036854775807 sync_mode 1 for_kupdate 0 range_cyclic 0 writeback_index 2
//   d.process.acor-27885 [000] ...1 175018.227852: ext4_writepages_result: dev 259,32 ino 3278183 ret 0 pages_written 0 pages_skipped 0 sync_mode 1 writeback_index 2
// clang-format on

static ExamplePage g_full_page_ext4{
    "synthetic",
    R"(
00000000: 50fe 5852 279f 0000 c80f 00c0 ffff ffff  P.XR'...........
00000010: 0800 0000 5701 0001 8406 0000 2000 3010  ....W....... .0.
00000020: 566b 0000 8829 e86a 91ff ffff 0200 0000  Vk...).j........
00000030: 0000 0000 2873 0100 1b01 0001 8406 0000  ....(s..........
00000040: 2000 3010 9200 0000 1500 2c00 0000 0000   .0.......,.....
00000050: a029 e86a 91ff ffff 0ac8 0400 1e01 0001  .).j............
00000060: 8406 0000 2000 3010 2866 0100 1500 2c00  .... .0.(f....,.
00000070: 0000 0000 0050 0000 0000 0000 0010 0000  .....P..........
00000080: 0000 0000 a804 0400 5701 0001 8406 0000  ........W.......
00000090: 2000 3010 91ff ffff 586f e86a 91ff ffff   .0.....Xo.j....
000000a0: 0100 0000 0000 0000 c83a 0300 6c01 0001  .........:..l...
000000b0: 8406 0000 2000 3010 0000 0000 1500 2c00  .... .0.......,.
000000c0: 0000 0000 0500 0000 5701 0001 ac6c 0100  ........W....l..
000000d0: 6d01 0001 8406 0000 2000 3010 91ff ffff  m....... .0.....
000000e0: 1500 2c00 0000 0000 0500 0000 faff ffff  ..,.............
000000f0: ffff ffff ffff ff07 184e 0000 0100 0000  .........N......
00000100: ec08 0200 3f01 0002 8406 0000 2000 3010  ....?....... .0.
00000110: 0000 0000 1500 2c00 0000 0000 0800 0000  ......,.........
00000120: 0000 0000 0600 0000 0000 0000 8081 0000  ................
00000130: 0000 0000 ec24 0100 6701 0001 8406 0000  .....$..g.......
00000140: 2000 3010 0000 0000 1500 2c00 0000 0000   .0.......,.....
00000150: 0500 0000 0100 0000 ffff ffff ffff ff07  ................
00000160: 0400 0000 7b04 3200 2a30 0500 2101 0001  ....{.2.*0..!...
00000170: 8406 0000 2000 3010 0000 0000 1500 2c00  .... .0.......,.
00000180: 0000 0000 0050 0000 0000 0000 0010 0000  .....P..........
00000190: 0010 0000 288b 0200 5701 0001 8406 0000  ....(...W.......
000001a0: 2000 3010 0000 0000 8829 e86a 91ff ffff   .0......).j....
000001b0: 0200 0000 0000 0000 0832 0100 1b01 0001  .........2......
000001c0: 8406 0000 2000 3010 566b 0000 1500 2c00  .... .0.Vk....,.
000001d0: 0000 0000 a029 e86a 91ff ffff eaa0 0400  .....).j........
000001e0: 1e01 0001 8406 0000 2000 3010 280b 0400  ........ .0.(...
000001f0: 1500 2c00 0000 0000 0060 0000 0000 0000  ..,......`......
00000200: 980b 0000 0000 0000 88d0 0300 5701 0001  ............W...
00000210: 8406 0000 2000 3010 566b 0000 586f e86a  .... .0.Vk..Xo.j
00000220: 91ff ffff 0100 0000 0000 0000 c813 0300  ................
00000230: 6c01 0001 8406 0000 2000 3010 566b 0000  l....... .0.Vk..
00000240: 1500 2c00 0000 0000 0600 0000 0000 0000  ..,.............
00000250: ac5f 0100 6d01 0001 8406 0000 2000 3010  ._..m....... .0.
00000260: 1100 3010 1500 2c00 0000 0000 0600 0000  ..0...,.........
00000270: f9ff ffff ffff ffff ffff ff07 185a ea6a  .............Z.j
00000280: 0100 0000 4c02 0200 3f01 0002 8406 0000  ....L...?.......
00000290: 2000 3010 566b 0000 1500 2c00 0000 0000   .0.Vk....,.....
000002a0: 0800 0000 0000 0000 0700 0000 0000 0000  ................
000002b0: 8081 0000 6d01 0001 0c0b 0100 6701 0001  ....m.......g...
000002c0: 8406 0000 2000 3010 0000 0000 1500 2c00  .... .0.......,.
000002d0: 0000 0000 0600 0000 0100 0000 ffff ffff  ................
000002e0: ffff ff07 049a 0100 5701 0001 aa1c 0500  ........W.......
000002f0: 2101 0001 8406 0000 2000 3010 91ff ffff  !....... .0.....
00000300: 1500 2c00 0000 0000 0060 0000 0000 0000  ..,......`......
00000310: 980b 0000 980b 0000 889e 0200 5701 0001  ............W...
00000320: 8406 0000 2000 3010 91ff ffff 8829 e86a  .... .0......).j
00000330: 91ff ffff 0200 0000 0000 0000 8838 0100  .............8..
00000340: 1b01 0001 8406 0000 2000 3010 91ff ffff  ........ .0.....
00000350: 1500 2c00 0000 0000 a029 e86a 91ff ffff  ..,......).j....
00000360: 2ab8 1800 3501 0001 8406 0000 2000 3010  *...5....... .0.
00000370: feff ffff 1500 2c00 0000 0000 0800 2c00  ......,.......,.
00000380: 0000 0000 0000 0000 2000 3010 32fe 0300  ........ .0.2...
00000390: 2201 0001 8406 0000 2000 3010 0000 0000  "....... .0.....
000003a0: 1500 2c00 0000 0000 ffff ffff ffff ff7f  ..,.............
000003b0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
000003c0: ffff ffff ffff ff7f 0000 0000 0000 0000  ................
000003d0: 0100 0000 0000 0000 887e 0800 5701 0001  .........~..W...
000003e0: 8406 0000 2000 3010 7b04 3200 7c3f e86a  .... .0.{.2.|?.j
000003f0: 91ff ffff 0a00 0000 0000 0000 ec2d 0300  .............-..
00000400: 2301 0001 8406 0000 2000 3010 7b04 3200  #....... .0.{.2.
00000410: 1500 2c00 0000 0000 0000 0000 0000 0000  ..,.............
00000420: ffff ffff ffff ff7f 0100 0000 3c01 0001  ............<...
00000430: 0a42 0c00 2401 0001 8406 0000 2000 3010  .B..$....... .0.
00000440: 0800 0000 1500 2c00 0000 0000 0000 0000  ......,.........
00000450: 0000 0000 0700 0000 0002 0000 885f 0100  ............._..
00000460: 6c01 0001 8406 0000 2000 3010 0100 0000  l....... .0.....
00000470: 1500 2c00 0000 0000 0000 0000 566b 0000  ..,.........Vk..
00000480: 0c25 0100 6d01 0001 8406 0000 2000 3010  .%..m....... .0.
00000490: 0400 0000 1500 2c00 0000 0000 0000 0000  ......,.........
000004a0: 0700 0000 ffff ffff ffff ff07 1400 0000  ................
000004b0: 0100 0000 caee 0100 5101 0001 8406 0000  ........Q.......
000004c0: 2000 3010 1100 0000 1500 2c00 0000 0000   .0.......,.....
000004d0: 0000 0000 0700 0000 2500 0000 2000 3010  ........%... .0.
000004e0: 323b 0600 3201 0001 8406 0000 2000 3010  2;..2....... .0.
000004f0: c86e 0000 1500 2c00 0000 0000 0700 0000  .n....,.........
00000500: 0000 0000 0000 0000 0000 0000 0080 b000  ................
00000510: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000520: 0000 0000 2024 0000 0400 0000 ae0a 1a00  .... $..........
00000530: 3a01 0001 8406 0000 2000 3010 0000 0000  :....... .0.....
00000540: 1500 2c00 0000 0000 0000 0000 0000 0000  ..,.............
00000550: 6101 0000 0700 0000 0000 0000 cf63 0000  a............c..
00000560: 4100 0000 0700 0000 b4c5 0200 3301 0001  A...........3...
00000570: 8406 0000 2000 3010 2000 3010 1500 2c00  .... .0. .0...,.
00000580: 0000 0000 cfe3 2000 0000 0000 0700 0000  ...... .........
00000590: 0000 0000 0000 0000 0000 0000 0080 b000  ................
000005a0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
000005b0: 0000 0000 2024 0000 6c01 0001 4859 0400  .... $..l...HY..
000005c0: 1b01 0001 8406 0000 2000 3010 0000 0000  ........ .0.....
000005d0: 1500 2c00 0000 0000 9c99 ea6a 91ff ffff  ..,........j....
000005e0: c850 0500 6001 0001 8406 0000 2000 3010  .P..`....... .0.
000005f0: 0000 0000 1500 2c00 0000 0000 0000 0000  ......,.........
00000600: 0700 0000 2ee6 0200 3e01 0002 8406 0000  ........>.......
00000610: 2000 3010 566b 0000 1500 2c00 0000 0000   .0.Vk....,.....
00000620: 0800 0000 0000 0000 0700 0000 0700 0000  ................
00000630: 0000 0000 0000 0000 0100 0000 8081 3010  ..............0.
00000640: a804 0400 5701 0001 8406 0000 2000 3010  ....W....... .0.
00000650: cb07 3200 885a ea6a 91ff ffff 0100 0000  ..2..Z.j........
00000660: 0000 0000 8875 0300 6c01 0001 8406 0000  .....u..l.......
00000670: 2000 3010 0300 0000 0300 0000 0000 0000   .0.............
00000680: 0100 0000 0100 0000 ccd4 0100 6d01 0001  ............m...
00000690: 8406 0000 2000 3010 cb07 3200 0300 0000  .... .0...2.....
000006a0: 0000 0000 0000 0000 0200 0000 2724 0000  ............'$..
000006b0: 0000 0000 1100 3010 0100 0000 a850 0500  ......0......P..
000006c0: 5701 0001 8406 0000 2000 3010 0000 0000  W....... .0.....
000006d0: 885a ea6a 91ff ffff 0100 0000 0000 0000  .Z.j............
000006e0: 680f 0200 6c01 0001 8406 0000 2000 3010  h...l....... .0.
000006f0: 0000 0000 0400 0000 0000 0000 0100 0000  ................
00000700: 6d01 0001 ac79 0100 6d01 0001 8406 0000  m....y..m.......
00000710: 2000 3010 0000 0000 0400 0000 0000 0000   .0.............
00000720: 0000 0000 0200 0000 2924 0000 0000 0000  ........)$......
00000730: 1143 0200 0100 0000 2818 0400 5701 0001  .C......(...W...
00000740: 8406 0000 2000 3010 0000 0000 8829 e86a  .... .0......).j
00000750: 91ff ffff 0200 0000 0000 0000 8838 0100  .............8..
00000760: 1b01 0001 8406 0000 2000 3010 0400 0000  ........ .0.....
00000770: 1500 2c00 0000 0000 a029 e86a 91ff ffff  ..,......).j....
00000780: 0e89 0300 5301 0001 8406 0000 2000 3010  ....S....... .0.
00000790: e128 0000 1500 2c00 0000 0000 2500 0000  .(....,.....%...
000007a0: 0000 0000 cfe3 2000 0000 0000 0000 0000  ...... .........
000007b0: 0700 0000 6000 0000 0700 0000 aca0 0100  ....`...........
000007c0: 6701 0001 8406 0000 2000 3010 e128 0000  g....... .0..(..
000007d0: 1500 2c00 0000 0000 0000 0000 0700 0000  ..,.............
000007e0: cfe3 2000 0000 0000 01a2 0800 0000 0000  .. .............
000007f0: 28b2 1e00 1b01 0001 8406 0000 2000 3010  (........... .0.
00000800: e128 0000 1500 2c00 0000 0000 9846 e86a  .(....,......F.j
00000810: 91ff ffff 68d2 1b00 5701 0001 8406 0000  ....h...W.......
00000820: 2000 3010 e128 0000 7c3f e86a 91ff ffff   .0..(..|?.j....
00000830: 0a00 0000 0000 0000 0c57 0200 2301 0001  .........W..#...
00000840: 8406 0000 2000 3010 006c 0000 1500 2c00  .... .0..l....,.
00000850: 0000 0000 0700 0000 0000 0000 f8ff ffff  ................
00000860: ffff ff7f 0100 0000 0000 0000 6e69 8200  ............ni..
00000870: 2501 0001 8406 0000 2000 3010 ca6e 0000  %....... .0..n..
00000880: 1500 2c00 0000 0000 0000 0000 0700 0000  ..,.............
00000890: 0000 0000 0000 0000 0700 0000 0000 0000  ................
000008a0: 0100 0000 0200 3010 3e13 bd82 5500 0000  ......0.>...U...
000008b0: 0600 0000 3001 0001 8015 0000 2000 3010  ....0....... .0.
000008c0: 0000 0000 c801 1400 0000 0000 8860 4404  .............`D.
000008d0: 1c01 0001 8015 0000 2000 3010 2000 0000  ........ .0. ...
000008e0: c801 1400 0000 0000 0000 0000 0000 0000  ................
000008f0: 88a9 0300 5701 0001 8015 0000 2000 3010  ....W....... .0.
00000900: 0400 0000 1c1e e86a 91ff ffff 0300 0000  .......j........
00000910: 0000 0000 a85a 1600 1b01 0001 8015 0000  .....Z..........
00000920: 2000 3010 2000 3010 c801 1400 0000 0000   .0. .0.........
00000930: c41e e86a 91ff ffff ca95 1c00 2901 0001  ...j........)...
00000940: 8015 0000 2000 3010 2000 3010 c801 1400  .... .0. .0.....
00000950: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000960: 0010 0000 c8fb 0100 2801 0001 8015 0000  ........(.......
00000970: 2000 3010 5101 0001 c801 1400 0000 0000   .0.Q...........
00000980: 0000 0000 0000 0000 6af1 0600 2901 0001  ........j...)...
00000990: 8015 0000 2000 3010 0000 0000 c801 1400  .... .0.........
000009a0: 0000 0000 0100 0000 0000 0000 0000 0000  ................
000009b0: 0010 0000 488f 0000 2801 0001 8015 0000  ....H...(.......
000009c0: 2000 3010 0200 ffff c801 1400 0000 0000   .0.............
000009d0: 0100 0000 0000 0000 483b 0900 4d01 0001  ........H;..M...
000009e0: 8015 0000 2000 3010 0000 0000 c801 1400  .... .0.........
000009f0: 0000 0000 1800 0000 0000 0000 8852 0100  .............R..
00000a00: 5701 0001 8015 0000 2000 3010 e128 0000  W....... .0..(..
00000a10: 9ce9 e76a 91ff ffff 0a00 0000 0000 0000  ...j............
00000a20: e615 0200 3001 0001 8015 0000 2000 3010  ....0....... .0.
00000a30: 0155 0000 c801 1400 0000 0000 68d0 0000  .U..........h...
00000a40: 1b01 0001 8015 0000 2000 3010 6606 3200  ........ .0.f.2.
00000a50: c801 1400 0000 0000 acfa ea6a 91ff ffff  ...........j....
00000a60: 6a0f 0200 6901 0001 8015 0000 2000 3010  j...i....... .0.
00000a70: 7106 3200 c801 1400 0000 0000 0000 0000  q.2.............
00000a80: 0000 0000 ffff ffff 0000 0000 e895 0300  ................
00000a90: 5701 0001 8015 0000 2000 3010 0300 0000  W....... .0.....
00000aa0: acbe ea6a 91ff ffff 0100 0000 0000 0000  ...j............
00000ab0: 8a38 0100 6501 0001 8015 0000 2000 3010  .8..e....... .0.
00000ac0: c41e e86a c801 1400 0000 0000 0000 0000  ...j............
00000ad0: feff ffff 0000 0000 0000 0000 ee86 0400  ................
00000ae0: 6301 0001 8015 0000 2000 3010 0000 0000  c....... .0.....
00000af0: c801 1400 0000 0000 0000 0000 0000 0000  ................
00000b00: 0000 0000 0000 0000 4285 5000 0000 0000  ........B.P.....
00000b10: 0200 0000 0000 0000 8e36 0200 6201 0001  .........6..b...
00000b20: 8015 0000 2000 3010 7d55 0000 c801 1400  .... .0.}U......
00000b30: 0000 0000 0000 0000 0100 0000 0000 0000  ................
00000b40: 0000 0000 4285 5000 0000 0000 0000 0000  ....B.P.........
00000b50: 0200 3010 8c5f 0100 3401 0001 8015 0000  ..0.._..4.......
00000b60: 2000 3010 0000 0000 c801 1400 0000 0000   .0.............
00000b70: 4285 5000 0000 0000 0200 0000 0000 0000  B.P.............
00000b80: 1000 0000 8081 0000 aa43 0500 3c01 0001  .........C..<...
00000b90: 8015 0000 2000 3010 2801 0001 c801 1400  .... .0.(.......
00000ba0: 0000 0000 4205 0000 a100 0000 0200 0000  ....B...........
00000bb0: 0200 0000 8871 0800 5701 0001 8015 0000  .....q..W.......
00000bc0: 2000 3010 2000 3010 885a ea6a 91ff ffff   .0. .0..Z.j....
00000bd0: 0100 0000 0000 0000 4825 0400 6c01 0001  ........H%..l...
00000be0: 8015 0000 2000 3010 2801 0001 0300 0000  .... .0.(.......
00000bf0: 0000 0000 0400 0000 7106 3200 0c73 0100  ........q.2..s..
00000c00: 6d01 0001 8015 0000 2000 3010 2901 0001  m....... .0.)...
00000c10: 0300 0000 0000 0000 0400 0000 0100 0000  ................
00000c20: 2680 0800 0000 0000 1100 0000 0100 0000  &...............
00000c30: c845 0400 5701 0001 8015 0000 2000 3010  .E..W....... .0.
00000c40: 2000 3010 885a ea6a 91ff ffff 0100 0000   .0..Z.j........
00000c50: 0000 0000 e8c9 0000 6c01 0001 8015 0000  ........l.......
00000c60: 2000 3010 2000 3010 0400 0000 0000 0000   .0. .0.........
00000c70: 0800 0000 0500 0000 6cdd 0000 6d01 0001  ........l...m...
00000c80: 8015 0000 2000 3010 2801 0001 0400 0000  .... .0.(.......
00000c90: 0000 0000 0800 0000 0300 0000 1290 7000  ..............p.
00000ca0: 0000 0000 1100 0000 0100 0000 6875 0300  ............hu..
00000cb0: 5701 0001 8015 0000 2000 3010 7106 3200  W....... .0.q.2.
00000cc0: 8829 e86a 91ff ffff 0200 0000 0000 0000  .).j............
00000cd0: a847 0000 1b01 0001 8015 0000 2000 3010  .G.......... .0.
00000ce0: 9ce9 e76a c801 1400 0000 0000 a029 e86a  ...j.........).j
00000cf0: 91ff ffff e83a 0300 1b01 0001 8015 0000  .....:..........
00000d00: 2000 3010 7106 3200 c801 1400 0000 0000   .0.q.2.........
00000d10: 9c99 ea6a 91ff ffff ae93 0100 6601 0001  ...j........f...
00000d20: 8015 0000 2000 3010 acfa ea6a c801 1400  .... .0....j....
00000d30: 0000 0000 0000 0000 feff ffff 0000 0000  ................
00000d40: 2000 3010 0000 0000 0000 0000 0000 0000   .0.............
00000d50: 0000 0000 48b6 0000 1b01 0001 8015 0000  ....H...........
00000d60: 2000 3010 e128 0000 c801 1400 0000 0000   .0..(..........
00000d70: 9c99 ea6a 91ff ffff a8ea 0300 1b01 0001  ...j............
00000d80: 8015 0000 2000 3010 e128 0000 c801 1400  .... .0..(......
00000d90: 0000 0000 08eb e76a 91ff ffff 885f 0100  .......j....._..
00000da0: 4e01 0001 8015 0000 2000 3010 2efe 0300  N....... .0.....
00000db0: c801 1400 0000 0000 0800 0000 0000 0000  ................
00000dc0: e8bc 0000 5701 0001 8015 0000 2000 3010  ....W....... .0.
00000dd0: 0000 0000 8829 e86a 91ff ffff 0200 0000  .....).j........
00000de0: 0000 0000 c895 0000 1b01 0001 8015 0000  ................
00000df0: 2000 3010 2000 3010 c801 1400 0000 0000   .0. .0.........
00000e00: a029 e86a 91ff ffff cab2 0b00 1e01 0001  .).j............
00000e10: 8015 0000 2000 3010 0000 0000 c801 1400  .... .0.........
00000e20: 0000 0000 0000 0000 0000 0000 0400 0000  ................
00000e30: 0000 0000 689a 0400 5701 0001 8015 0000  ....h...W.......
00000e40: 2000 3010 0000 0000 586f e86a 91ff ffff   .0.....Xo.j....
00000e50: 0100 0000 0000 0000 8884 0200 6c01 0001  ............l...
00000e60: 8015 0000 2000 3010 8829 e86a c801 1400  .... .0..).j....
00000e70: 0000 0000 0000 0000 4100 0000 ac47 0000  ........A....G..
00000e80: 6d01 0001 8015 0000 2000 3010 e128 0000  m....... .0..(..
00000e90: c801 1400 0000 0000 0000 0000 0000 0000  ................
00000ea0: 0000 0000 0000 0000 001c 0200 0000 0000  ................
00000eb0: 2a66 0100 5101 0001 8015 0000 2000 3010  *f..Q....... .0.
00000ec0: 0000 0000 c801 1400 0000 0000 0000 0000  ................
00000ed0: 0100 0000 0000 0000 2000 3010 087e 0200  ........ .0..~..
00000ee0: 6a01 0001 8015 0000 2000 3010 0100 0000  j....... .0.....
00000ef0: c801 1400 0000 0000 0000 0000 0100 0000  ................
00000f00: 8c11 0100 6b01 0001 8015 0000 2000 3010  ....k....... .0.
00000f10: 1b01 0001 c801 1400 0000 0000 0000 0000  ................
00000f20: 0000 0000 0000 0000 0000 0000 0028 0000  .............(..
00000f30: 2000 3010 8c5f 0100 6701 0001 8015 0000   .0.._..g.......
00000f40: 2000 3010 0000 0000 c801 1400 0000 0000   .0.............
00000f50: 0000 0000 ffff ffff ffff ffff ffff ff07  ................
00000f60: 0800 0000 0700 0000 6e02 0200 5301 0001  ........n...S...
00000f70: 8015 0000 2000 3010 0100 0000 c801 1400  .... .0.........
00000f80: 0000 0000 0000 0000 2000 3010 0000 0000  ........ .0.....
00000f90: 0100 0000 0000 0000 0100 0000 0000 0000  ................
00000fa0: 0000 0000 cc3a 0300 3f01 0002 8015 0000  .....:..?.......
00000fb0: 2000 3010 7106 3200 c801 1400 0000 0000   .0.q.2.........
00000fc0: 0800 0000 0000 0000 0100 0000 0000 0000  ................
00000fd0: 8081 3010 3d00 0000 f542 0000 0000 0000  ..0.=....B......
00000fe0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
00000ff0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
    )",
};

TEST_F(CpuReaderParsePagePayloadTest, ParseExt4WithOverwrite) {
  const ExamplePage* test_case = &g_full_page_ext4;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_TRUE(page_header->lost_events);  // data loss
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  EXPECT_THAT(AllTracePackets(), IsEmpty());
}

// Page with a single event containing a __data_loc entry with value 0x0000
//
//            [timestamp            ] [32 byte payload next ]
//  00000000: D7 B3 0A 57 CF 02 00 00 20 00 00 00 00 00 00 00   ...W.... .......
//            [evt hdr  ] [id ]
//  00000010: 67 A6 13 00 0F 06 00 00 3D 01 00 00 45 00 00 00   g.......=...E...
//  00000020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
//
// name: tracing_mark_write
// ID: 1551
// format:
//     field:unsigned short common_type;    offset:0;    size:2;    signed:0;
//     field:unsigned char common_flags;    offset:2;    size:1;    signed:0;
//     field:unsigned char common_preempt_count;    offset:3;    size:1;
//     signed:0; field:int common_pid;    offset:4;    size:4;    signed:1;
//
//     field:char type;    offset:8;    size:1;    signed:0;
//     field:int pid;    offset:12;    size:4;    signed:1;
//     field:__data_loc char[] name;    offset:16;    size:4;    signed:0;
//     field:int value;    offset:20;    size:4;    signed:1;
//
static char g_zero_data_loc[] =
    R"(
00000000: D7B3 0A57 CF02 0000 2000 0000 0000 0000   ...W.... .......
00000010: 67A6 1300 0F06 0000 3D01 0000 4500 0000   g.......=...E...
00000020: 0000 0000 0000 0000 0000 0000 0000 0000   ................
00000030: 0000 0000 0000 0000 0000 0000 0000 0000   ................
  )";

TEST_F(CpuReaderParsePagePayloadTest, ZeroLengthDataLoc) {
  auto page = PageFromXxd(g_zero_data_loc);

  // Hand-build a translation table that handles dpu/tracing_mark_write for this
  // test page.
  // TODO(rsavitski): look into making these tests less verbose by feeding a
  // format string through proto_translation_table to get the format.
  std::vector<Field> common_fields;
  {  // common_pid
    common_fields.emplace_back(Field{});
    Field* field = &common_fields.back();
    field->ftrace_offset = 4;
    field->ftrace_size = 4;
    field->ftrace_type = kFtraceCommonPid32;
    field->proto_field_id = 2;
    field->proto_field_type = ProtoSchemaType::kInt32;
    SetTranslationStrategy(field->ftrace_type, field->proto_field_type,
                           &field->strategy);
  }
  using Dpu = protos::gen::DpuTracingMarkWriteFtraceEvent;
  Event evt{"tracing_mark_write",
            "dpu",
            {
                {8, 1, FtraceFieldType::kFtraceUint8, "type",
                 Dpu::kTypeFieldNumber, ProtoSchemaType::kUint32,
                 TranslationStrategy::kInvalidTranslationStrategy},
                {12, 4, FtraceFieldType::kFtraceInt32, "pid",
                 Dpu::kPidFieldNumber, ProtoSchemaType::kInt32,
                 TranslationStrategy::kInvalidTranslationStrategy},
                {16, 4, FtraceFieldType::kFtraceDataLoc, "name",
                 Dpu::kNameFieldNumber, ProtoSchemaType::kString,
                 TranslationStrategy::kInvalidTranslationStrategy},
                {20, 4, FtraceFieldType::kFtraceInt32, "value",
                 Dpu::kValueFieldNumber, ProtoSchemaType::kInt32,
                 TranslationStrategy::kInvalidTranslationStrategy},
            },
            /*ftrace_event_id=*/1551,
            /*proto_field_id=*/348,
            /*size=*/24};
  for (Field& field : evt.fields) {
    SetTranslationStrategy(field.ftrace_type, field.proto_field_type,
                           &field.strategy);
  }
  std::vector<Event> events;
  events.emplace_back(std::move(evt));

  NiceMock<MockFtraceProcfs> mock_ftrace;
  PrintkMap printk_formats;
  ProtoTranslationTable translation_table(
      &mock_ftrace, events, std::move(common_fields),
      ProtoTranslationTable::DefaultPageHeaderSpecForTesting(),
      InvalidCompactSchedEventFormatForTesting(), printk_formats);
  ProtoTranslationTable* table = &translation_table;

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("dpu", "tracing_mark_write")));

  FtraceMetadata metadata{};
  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  // successfully parsed the whole 32 byte event
  ASSERT_EQ(32u, page_header->size);
  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();
  EXPECT_EQ(bundle.event().size(), 1u);
  const protos::gen::FtraceEvent& event = bundle.event()[0];
  EXPECT_EQ(event.pid(), 317u);
  EXPECT_EQ(event.dpu_tracing_mark_write().type(), 69u);
  EXPECT_EQ(event.dpu_tracing_mark_write().pid(), 0);
  EXPECT_EQ(event.dpu_tracing_mark_write().value(), 0);
  EXPECT_EQ(event.dpu_tracing_mark_write().name(), "");
}

static ExamplePage g_zero_padded{
    "synthetic",
    R"(
    00000000: DBF4 87FE F901 0000 F00F 0000 0000 0000   ................
    00000010: 0700 0000 0500 0000 EE02 0000 50AA 4C00   ............P.L.
    00000020: AEFF FFFF 457C 3633 390A 0000 0000 0000   ....E|639.......
    00000030: E939 1300 0500 0000 EE02 0000 50AA 4C00   .9..........P.L.
    00000040: AEFF FFFF 427C 3633 397C 6361 6E63 656C   ....B|639|cancel
    00000050: 2074 696D 6572 0A00 4753 0A00 0500 0000    timer..GS......
    00000060: EE02 0000 50AA 4C00 AEFF FFFF 457C 3633   ....P.L.....E|63
    00000070: 390A 0000 0000 0000 C929 0800 0500 0000   9........)......
    00000080: EE02 0000 50AA 4C00 AEFF FFFF 437C 3633   ....P.L.....C|63
    00000090: 397C 5653 594E 432D 6170 707C 310A 0000   9|VSYNC-app|1...
    000000A0: 2A48 0600 6500 0101 EE02 0000 6170 7000   *H..e.......app.
    000000B0: 6163 6566 6C69 6E67 6572 0000 EF02 0000   aceflinger......
    000000C0: 6100 0000 0100 0000 0200 0000 E94D 1900   a............M..
    000000D0: 0500 0000 EE02 0000 50AA 4C00 AEFF FFFF   ........P.L.....
    000000E0: 437C 3633 397C 5653 502D 6D6F 6465 7C30   C|639|VSP-mode|0
    000000F0: 0A00 0000 0DD5 0400 0500 0000 EE02 0000   ................
    00000100: 50AA 4C00 AEFF FFFF 437C 3633 397C 5653   P.L.....C|639|VS
    00000110: 502D 7469 6D65 506F 696E 747C 3231 3733   P-timePoint|2173
    00000120: 3235 3939 3337 3132 360A 0000 2DF1 0300   259937126...-...
    00000130: 0500 0000 EE02 0000 50AA 4C00 AEFF FFFF   ........P.L.....
    00000140: 437C 3633 397C 5653 502D 7072 6564 6963   C|639|VSP-predic
    00000150: 7469 6F6E 7C32 3137 3332 3736 3230 3036   tion|21732762006
    00000160: 3538 0A00 30B0 0600 0500 0000 EE02 0000   58..0...........
    00000170: 50AA 4C00 AEFF FFFF 427C 3633 397C 6170   P.L.....B|639|ap
    00000180: 7020 616C 6172 6D20 696E 2031 3632 3633   p alarm in 16263
    00000190: 7573 3B20 5653 594E 4320 696E 2034 3732   us; VSYNC in 472
    000001A0: 3633 7573 0A00 0000 878F 0300 0500 0000   63us............
    000001B0: EE02 0000 50AA 4C00 AEFF FFFF 457C 3633   ....P.L.....E|63
    000001C0: 390A 0000 0000 0000 3029 1B00 5B00 0102   9.......0)..[...
    000001D0: EE02 0000 5469 6D65 7244 6973 7061 7463   ....TimerDispatc
    000001E0: 6800 0000 EE02 0000 6100 0000 0100 0000   h.......a.......
    000001F0: 0000 0000 7377 6170 7065 722F 3500 0000   ....swapper/5...
    00000200: 0000 0000 0000 0000 7800 0000 10DC 4302   ........x.....C.
    00000210: 5B00 0102 0000 0000 7377 6170 7065 722F   [.......swapper/
    00000220: 3500 0000 0000 0000 0000 0000 7800 0000   5...........x...
    00000230: 0000 0000 0000 0000 7263 756F 702F 3200   ........rcuop/2.
    00000240: 0000 0000 0000 0000 2000 0000 7800 0000   ........ ...x...
    00000250: CA71 0B00 6500 0102 2000 0000 7263 755F   .q..e... ...rcu_
    00000260: 7072 6565 6D70 7400 0000 0000 0B00 0000   preempt.........
    00000270: 7800 0000 0100 0000 0300 0000 0859 0100   x............Y..
    00000280: 3700 0102 2000 0000 0B00 0000 0000 0000   7... ...........
    00000290: 6899 4200 AEFF FFFF 0000 0000 0000 0000   h.B.............
    000002A0: 300F 1B00 5B00 0102 2000 0000 7263 756F   0...[... ...rcuo
    000002B0: 702F 3200 0000 0000 0000 0000 2000 0000   p/2......... ...
    000002C0: 7800 0000 0100 0000 0000 0000 6E64 726F   x...........ndro
    000002D0: 6964 2E73 7973 7465 6D75 6900 A009 0000   id.systemui.....
    000002E0: 7800 0000 17EC 3100 0500 0000 A009 0000   x.....1.........
    000002F0: 50AA 4C00 AEFF FFFF 427C 3234 3634 7C61   P.L.....B|2464|a
    00000300: 6E64 726F 6964 2E76 6965 772E 4163 6365   ndroid.view.Acce
    00000310: 7373 6962 696C 6974 7949 6E74 6572 6163   ssibilityInterac
    00000320: 7469 6F6E 436F 6E74 726F 6C6C 6572 2450   tionController$P
    00000330: 7269 7661 7465 4861 6E64 6C65 723A 2023   rivateHandler: #
    00000340: 320A 0000 8998 EB00 EA02 0000 A009 0000   2...............
    00000350: 4AD7 0C00 2697 0500 CE22 0000 0000 0000   J...&...."......
    00000360: 0000 0000 0100 0000 1100 0000 CA45 0400   .............E..
    00000370: EB02 0000 A009 0000 4AD7 0C00 0000 0000   ........J.......
    00000380: A402 0000 0000 0000 0000 0000 0000 0000   ................
    00000390: 0000 0000 0000 0000 CA6C 0400 6500 0104   .........l..e...
    000003A0: A009 0000 6269 6E64 6572 3A38 3931 305F   ....binder:8910_
    000003B0: 3400 6F00 3C2C 0000 7800 0000 0100 0000   4.o.<,..x.......
    000003C0: 0400 0000 673C 3400 0500 0000 A009 0000   ....g<4.........
    000003D0: 50AA 4C00 AEFF FFFF 457C 3234 3634 0A00   P.L.....E|2464..
    000003E0: 0000 0000 10EF 2000 5B00 0102 A009 0000   ...... .[.......
    000003F0: 6E64 726F 6964 2E73 7973 7465 6D75 6900   ndroid.systemui.
    00000400: A009 0000 7800 0000 0100 0000 0000 0000   ....x...........
    00000410: 7377 6170 7065 722F 3500 0000 0000 0000   swapper/5.......
    00000420: 0000 0000 7800 0000 D098 ED01 5B00 0102   ....x.......[...
    00000430: 0000 0000 7377 6170 7065 722F 3500 0000   ....swapper/5...
    00000440: 0000 0000 0000 0000 7800 0000 0000 0000   ........x.......
    00000450: 0000 0000 6E64 726F 6964 2E73 7973 7465   ....ndroid.syste
    00000460: 6D75 6900 A009 0000 7800 0000 F761 1F00   mui.....x....a..
    00000470: 0500 0000 A009 0000 50AA 4C00 AEFF FFFF   ........P.L.....
    00000480: 427C 3234 3634 7C61 6E64 726F 6964 2E76   B|2464|android.v
    00000490: 6965 772E 4163 6365 7373 6962 696C 6974   iew.Accessibilit
    000004A0: 7949 6E74 6572 6163 7469 6F6E 436F 6E74   yInteractionCont
    000004B0: 726F 6C6C 6572 2450 7269 7661 7465 4861   roller$PrivateHa
    000004C0: 6E64 6C65 723A 2023 320A 0000 E9F6 A500   ndler: #2.......
    000004D0: EA02 0000 A009 0000 4ED7 0C00 2697 0500   ........N...&...
    000004E0: CE22 0000 0000 0000 0000 0000 0100 0000   ."..............
    000004F0: 1100 0000 4A3F 0400 EB02 0000 A009 0000   ....J?..........
    00000500: 4ED7 0C00 0000 0000 2802 0000 0000 0000   N.......(.......
    00000510: 0000 0000 0000 0000 0000 0000 0000 0000   ................
    00000520: EA93 0400 6500 0104 A009 0000 6269 6E64   ....e.......bind
    00000530: 6572 3A38 3931 305F 3400 6F00 3C2C 0000   er:8910_4.o.<,..
    00000540: 7800 0000 0100 0000 0000 0000 0AD7 3A01   x.............:.
    00000550: 3100 1101 A009 0000 B028 39B1 CCFF FFFF   1........(9.....
    00000560: A837 F9A8 ADFF FFFF 0010 39B1 CCFF FFFF   .7........9.....
    00000570: 2000 0000 FFFF FFFF 44F5 0100 2E00 1101    .......D.......
    00000580: A009 0000 B028 39B1 CCFF FFFF AA79 0100   .....(9......y..
    00000590: 6500 1102 A009 0000 6B77 6F72 6B65 722F   e.......kworker/
    000005A0: 7531 363A 3130 0000 6001 0000 7800 0000   u16:10..`...x...
    000005B0: 0100 0000 0000 0000 8845 0100 3700 1102   .........E..7...
    000005C0: A009 0000 6001 0000 0000 0000 7C51 3800   ....`.......|Q8.
    000005D0: AEFF FFFF 0000 0000 0000 0000 89DD 7300   ..............s.
    000005E0: EA02 0000 A009 0000 50D7 0C00 2697 0500   ........P...&...
    000005F0: CE22 0000 0000 0000 0000 0000 0300 0000   ."..............
    00000600: 1100 0000 0AD5 0400 EB02 0000 A009 0000   ................
    00000610: 50D7 0C00 0000 0000 A404 0000 0000 0000   P...............
    00000620: 0000 0000 0000 0000 0000 0000 0000 0000   ................
    00000630: 4A7E 0500 6500 0104 A009 0000 6269 6E64   J~..e.......bind
    00000640: 6572 3A38 3931 305F 3400 6F00 3C2C 0000   er:8910_4.o.<,..
    00000650: 7800 0000 0100 0000 0000 0000 A790 2E00   x...............
    00000660: 0500 0000 A009 0000 50AA 4C00 AEFF FFFF   ........P.L.....
    00000670: 457C 3234 3634 0A00 0000 0000 9048 2800   E|2464.......H(.
    00000680: 5B00 0102 A009 0000 6E64 726F 6964 2E73   [.......ndroid.s
    00000690: 7973 7465 6D75 6900 A009 0000 7800 0000   ystemui.....x...
    000006A0: 0100 0000 0000 0000 7377 6170 7065 722F   ........swapper/
    000006B0: 3500 0000 0000 0000 0000 0000 7800 0000   5...........x...
    000006C0: B043 2100 5B00 0102 0000 0000 7377 6170   .C!.[.......swap
    000006D0: 7065 722F 3500 0000 0000 0000 0000 0000   per/5...........
    000006E0: 7800 0000 0000 0000 0000 0000 6269 6E64   x...........bind
    000006F0: 6572 3A32 3436 345F 3800 6900 EF0C 0000   er:2464_8.i.....
    00000700: 7800 0000 834C 0700 F002 0000 EF0C 0000   x....L..........
    00000710: 51D7 0C00 AA4E 5D00 6500 0103 EF0C 0000   Q....N].e.......
    00000720: 6E64 726F 6964 2E73 7973 7465 6D75 6900   ndroid.systemui.
    00000730: A009 0000 7800 0000 0100 0000 0500 0000   ....x...........
    00000740: D05E 6800 5B00 0102 EF0C 0000 6269 6E64   .^h.[.......bind
    00000750: 6572 3A32 3436 345F 3800 6900 EF0C 0000   er:2464_8.i.....
    00000760: 7800 0000 0100 0000 0000 0000 6269 6E64   x...........bind
    00000770: 6572 3A31 3936 375F 4200 0000 A20B 0000   er:1967_B.......
    00000780: 7000 0000 67CA 0600 E902 0000 A20B 0000   p...g...........
    00000790: AF07 0000 A20B 0000 7000 0000 7800 0000   ........p...x...
    000007A0: 7800 0000 B006 3B00 5B00 0102 A20B 0000   x.....;.[.......
    000007B0: 6269 6E64 6572 3A31 3936 375F 4200 0000   binder:1967_B...
    000007C0: A20B 0000 7800 0000 0100 0000 0000 0000   ....x...........
    000007D0: 7377 6170 7065 722F 3500 0000 0000 0000   swapper/5.......
    000007E0: 0000 0000 7800 0000 108B 5603 5B00 0102   ....x.....V.[...
    000007F0: 0000 0000 7377 6170 7065 722F 3500 0000   ....swapper/5...
    00000800: 0000 0000 0000 0000 7800 0000 0000 0000   ........x.......
    00000810: 0000 0000 6269 6E64 6572 3A32 3436 345F   ....binder:2464_
    00000820: 3800 6900 EF0C 0000 7800 0000 831A 0600   8.i.....x.......
    00000830: F002 0000 EF0C 0000 56D7 0C00 AAD2 5600   ........V.....V.
    00000840: 6500 0103 EF0C 0000 6E64 726F 6964 2E73   e.......ndroid.s
    00000850: 7973 7465 6D75 6900 A009 0000 7800 0000   ystemui.....x...
    00000860: 0100 0000 0000 0000 B027 4100 5B00 0102   .........'A.[...
    00000870: EF0C 0000 6269 6E64 6572 3A32 3436 345F   ....binder:2464_
    00000880: 3800 6900 EF0C 0000 7800 0000 0100 0000   8.i.....x.......
    00000890: 0000 0000 7377 6170 7065 722F 3500 0000   ....swapper/5...
    000008A0: 0000 0000 0000 0000 7800 0000 50F4 2A03   ........x...P.*.
    000008B0: 5B00 0102 0000 0000 7377 6170 7065 722F   [.......swapper/
    000008C0: 3500 0000 0000 0000 0000 0000 7800 0000   5...........x...
    000008D0: 0000 0000 0000 0000 6269 6E64 6572 3A32   ........binder:2
    000008E0: 3436 345F 3800 6900 EF0C 0000 7800 0000   464_8.i.....x...
    000008F0: 831A 0600 F002 0000 EF0C 0000 5BD7 0C00   ............[...
    00000900: 8A08 5300 6500 0103 EF0C 0000 6E64 726F   ..S.e.......ndro
    00000910: 6964 2E73 7973 7465 6D75 6900 A009 0000   id.systemui.....
    00000920: 7800 0000 0100 0000 0000 0000 B0BE 5000   x.............P.
    00000930: 5B00 0102 EF0C 0000 6269 6E64 6572 3A32   [.......binder:2
    00000940: 3436 345F 3800 6900 EF0C 0000 7800 0000   464_8.i.....x...
    00000950: 0100 0000 0000 0000 7377 6170 7065 722F   ........swapper/
    00000960: 3500 0000 0000 0000 0000 0000 7800 0000   5...........x...
    00000970: 50A1 5A0A 5B00 0102 0000 0000 7377 6170   P.Z.[.......swap
    00000980: 7065 722F 3500 0000 0000 0000 0000 0000   per/5...........
    00000990: 7800 0000 0000 0000 0000 0000 7263 756F   x...........rcuo
    000009A0: 702F 3200 0000 0000 0000 0000 2000 0000   p/2......... ...
    000009B0: 7800 0000 EA2B 0700 6500 0102 2000 0000   x....+..e... ...
    000009C0: 7263 756F 702F 3300 0000 0000 0000 0000   rcuop/3.........
    000009D0: 2800 0000 7800 0000 0100 0000 0000 0000   (...x...........
    000009E0: 90F9 1B00 5B00 0102 2000 0000 7263 756F   ....[... ...rcuo
    000009F0: 702F 3200 0000 0000 0000 0000 2000 0000   p/2......... ...
    00000A00: 7800 0000 0100 0000 0000 0000 7377 6170   x...........swap
    00000A10: 7065 722F 3500 0000 0000 0000 0000 0000   per/5...........
    00000A20: 7800 0000 303E D509 5B00 0102 0000 0000   x...0>..[.......
    00000A30: 7377 6170 7065 722F 3500 0000 0000 0000   swapper/5.......
    00000A40: 0000 0000 7800 0000 0000 0000 0000 0000   ....x...........
    00000A50: 6269 6E64 6572 3A32 3436 345F 3800 6900   binder:2464_8.i.
    00000A60: EF0C 0000 7800 0000 03AA 0900 F002 0000   ....x...........
    00000A70: EF0C 0000 66D7 0C00 EAFE 7F00 6500 0103   ....f.......e...
    00000A80: EF0C 0000 5363 7265 656E 4465 636F 7261   ....ScreenDecora
    00000A90: 7469 6F00 840B 0000 7800 0000 0100 0000   tio.....x.......
    00000AA0: 0200 0000 7028 4A00 5B00 0102 EF0C 0000   ....p(J.[.......
    00000AB0: 6269 6E64 6572 3A32 3436 345F 3800 6900   binder:2464_8.i.
    00000AC0: EF0C 0000 7800 0000 0100 0000 0000 0000   ....x...........
    00000AD0: 7377 6170 7065 722F 3500 0000 0000 0000   swapper/5.......
    00000AE0: 0000 0000 7800 0000 908D 0406 5B00 0102   ....x.......[...
    00000AF0: 0000 0000 7377 6170 7065 722F 3500 0000   ....swapper/5...
    00000B00: 0000 0000 0000 0000 7800 0000 0000 0000   ........x.......
    00000B10: 0000 0000 6C6F 6764 2E72 6561 6465 722E   ....logd.reader.
    00000B20: 7065 7200 5B06 0000 8200 0000 AAE6 2400   per.[.........$.
    00000B30: 6500 0102 5B06 0000 6C6F 6763 6174 0000   e...[...logcat..
    00000B40: 3000 0000 0000 0000 C105 0000 8200 0000   0...............
    00000B50: 0100 0000 0500 0000 90DB 2000 5B00 0102   .......... .[...
    00000B60: 5B06 0000 6C6F 6764 2E72 6561 6465 722E   [...logd.reader.
    00000B70: 7065 7200 5B06 0000 8200 0000 0100 0000   per.[...........
    00000B80: 0000 0000 6C6F 6763 6174 0000 3000 0000   ....logcat..0...
    00000B90: 0000 0000 C105 0000 8200 0000 7060 6100   ............p`a.
    00000BA0: 5B00 0102 C105 0000 6C6F 6763 6174 0000   [.......logcat..
    00000BB0: 3000 0000 0000 0000 C105 0000 8200 0000   0...............
    00000BC0: 0100 0000 0000 0000 7377 6170 7065 722F   ........swapper/
    00000BD0: 3500 0000 0000 0000 0000 0000 7800 0000   5...........x...
    00000BE0: D086 0202 5B00 0102 0000 0000 7377 6170   ....[.......swap
    00000BF0: 7065 722F 3500 0000 0000 0000 0000 0000   per/5...........
    00000C00: 7800 0000 0000 0000 0000 0000 6170 7000   x...........app.
    00000C10: 6163 6566 6C69 6E67 6572 0000 EF02 0000   aceflinger......
    00000C20: 6100 0000 2937 2700 0500 0000 EF02 0000   a...)7'.........
    00000C30: 50AA 4C00 AEFF FFFF 437C 3633 397C 5653   P.L.....C|639|VS
    00000C40: 502D 6D6F 6465 7C30 0A00 0000 8DC3 0300   P-mode|0........
    00000C50: 0500 0000 EF02 0000 50AA 4C00 AEFF FFFF   ........P.L.....
    00000C60: 437C 3633 397C 5653 502D 7469 6D65 506F   C|639|VSP-timePo
    00000C70: 696E 747C 3231 3733 3238 3530 3139 3236   int|217328501926
    00000C80: 340A 0000 6D43 0200 0500 0000 EF02 0000   4...mC..........
    00000C90: 50AA 4C00 AEFF FFFF 437C 3633 397C 5653   P.L.....C|639|VS
    00000CA0: 502D 7072 6564 6963 7469 6F6E 7C32 3137   P-prediction|217
    00000CB0: 3332 3932 3839 3739 3138 0A00 70FE 0600   3292897918..p...
    00000CC0: 0500 0000 EF02 0000 50AA 4C00 AEFF FFFF   ........P.L.....
    00000CD0: 427C 3633 397C 6170 7020 616C 6172 6D20   B|639|app alarm
    00000CE0: 696E 2037 3837 3875 733B 2056 5359 4E43   in 7878us; VSYNC
    00000CF0: 2069 6E20 3338 3837 3875 730A 0000 0000    in 38878us.....
    00000D00: C7AD 0100 0500 0000 EF02 0000 50AA 4C00   ............P.L.
    00000D10: AEFF FFFF 457C 3633 390A 0000 0000 0000   ....E|639.......
    00000D20: 3028 2B00 5B00 0102 EF02 0000 6170 7000   0(+.[.......app.
    00000D30: 6163 6566 6C69 6E67 6572 0000 EF02 0000   aceflinger......
    00000D40: 6100 0000 0100 0000 0000 0000 6C6F 6764   a...........logd
    00000D50: 2E72 6561 6465 722E 7065 7200 C611 0000   .reader.per.....
    00000D60: 8200 0000 8AE1 1A00 6500 0102 C611 0000   ........e.......
    00000D70: 6C6F 6763 6174 002F 3000 0000 0000 0000   logcat./0.......
    00000D80: BE11 0000 7800 0000 0100 0000 0400 0000   ....x...........
    00000D90: 3074 0D00 5B00 0102 C611 0000 6C6F 6764   0t..[.......logd
    00000DA0: 2E72 6561 6465 722E 7065 7200 C611 0000   .reader.per.....
    00000DB0: 8200 0000 0001 0000 0000 0000 6C6F 6763   ............logc
    00000DC0: 6174 002F 3000 0000 0000 0000 BE11 0000   at./0...........
    00000DD0: 7800 0000 4A34 3B00 3100 0101 BE11 0000   x...J4;.1.......
    00000DE0: 08D4 FF74 CCFF FFFF 40C9 2900 AEFF FFFF   ...t....@.).....
    00000DF0: 0044 8940 CBFF FFFF 2000 0000 FFFF FFFF   .D.@.... .......
    00000E00: A486 0100 2E00 0101 BE11 0000 08D4 FF74   ...............t
    00000E10: CCFF FFFF EA17 0100 6500 0102 BE11 0000   ........e.......
    00000E20: 6B77 6F72 6B65 722F 7531 363A 3130 0000   kworker/u16:10..
    00000E30: 6001 0000 7800 0000 0100 0000 0200 0000   `...x...........
    00000E40: E8BC 0000 3700 0102 BE11 0000 6001 0000   ....7.......`...
    00000E50: 0000 0000 7C51 3800 AEFF FFFF 0000 0000   ....|Q8.........
    00000E60: 0000 0000 B074 1600 5B00 0102 BE11 0000   .....t..[.......
    00000E70: 6C6F 6763 6174 002F 3000 0000 0000 0000   logcat./0.......
    00000E80: BE11 0000 7800 0000 0100 0000 0000 0000   ....x...........
    00000E90: 6C6F 6764 2E72 6561 6465 722E 7065 7200   logd.reader.per.
    00000EA0: C611 0000 8200 0000 6AFA 0B00 6500 0102   ........j...e...
    00000EB0: C611 0000 6C6F 6763 6174 002F 3000 0000   ....logcat./0...
    00000EC0: 0000 0000 BE11 0000 7800 0000 0100 0000   ........x.......
    00000ED0: 0500 0000 7023 0800 5B00 0102 C611 0000   ....p#..[.......
    00000EE0: 6C6F 6764 2E72 6561 6465 722E 7065 7200   logd.reader.per.
    00000EF0: C611 0000 8200 0000 0001 0000 0000 0000   ................
    00000F00: 6C6F 6763 6174 002F 3000 0000 0000 0000   logcat./0.......
    00000F10: BE11 0000 7800 0000 AA6B 1100 3100 0101   ....x....k..1...
    00000F20: BE11 0000 08D4 FF74 CCFF FFFF 40C9 2900   .......t....@.).
    00000F30: AEFF FFFF 0044 8940 CBFF FFFF 2000 0000   .....D.@.... ...
    00000F40: FFFF FFFF 64EA 0000 2E00 0101 BE11 0000   ....d...........
    00000F50: 08D4 FF74 CCFF FFFF EABC 0000 6500 0102   ...t........e...
    00000F60: BE11 0000 6B77 6F72 6B65 722F 7531 363A   ....kworker/u16:
    00000F70: 3130 0000 6001 0000 7800 0000 0100 0000   10..`...x.......
    00000F80: 0200 0000 48C3 0000 3700 0102 BE11 0000   ....H...7.......
    00000F90: 6001 0000 0000 0000 7C51 3800 AEFF FFFF   `.......|Q8.....
    00000FA0: 0000 0000 0000 0000 90C4 0F00 5B00 0102   ............[...
    00000FB0: BE11 0000 6C6F 6763 6174 002F 3000 0000   ....logcat./0...
    00000FC0: 0000 0000 BE11 0000 7800 0000 0100 0000   ........x.......
    00000FD0: 0000 0000 6C6F 6764 2E72 6561 6465 722E   ....logd.reader.
    00000FE0: 7065 7200 C611 0000 8200 0000 0000 0000   per.............
    00000FF0: 0000 0000 0000 0000 0000 0000 0000 0000   ................
    )",
};

// b/204564312: some (mostly 4.19) kernels rarely emit an invalid page, where
// the header says there's valid data, but the contents are a run of zeros
// (which doesn't decode to valid events per the ring buffer ABI). Confirm that
// the error is reported in the ftrace event bundle.
TEST_F(CpuReaderParsePagePayloadTest, InvalidZeroPaddedPage) {
  const ExamplePage* test_case = &g_zero_padded;
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  // Don't need enabled events, as the test checks that we can walk the event
  // headers down to the end of the page.
  FtraceDataSourceConfig ds_config = EmptyConfig();

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(0xff0u, page_header->size);
  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_ABI_ZERO_DATA_LENGTH);

  EXPECT_THAT(AllTracePackets(), IsEmpty());
}

static ExamplePage g_four_byte_commit{
    "synthetic",
    R"(
00000000: 105B DA5D C100 0000 0400 0000 0000 0000  .[.]............
00000010: 0000 0000 0000 0000 0000 0000 0000 0000  ................
    )",
};

TEST_F(CpuReaderParsePagePayloadTest, InvalidHeaderLength) {
  const ExamplePage* test_case = &g_four_byte_commit;
  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(4u, page_header->size);
  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_ABI_SHORT_DATA_LENGTH);

  EXPECT_THAT(AllTracePackets(), IsEmpty());
}

// Kernel code:
// trace_f2fs_truncate_partial_nodes(... nid = {1,2,3}, depth = 4, err = 0)
//
// After kernel commit 0b04d4c0542e("f2fs: Fix
// f2fs_truncate_partial_nodes ftrace event")
static ExamplePage g_f2fs_truncate_partial_nodes_new{
    "b281660544_new",
    R"(
00000000: 1555 c3e4 cb07 0000 3c00 0000 0000 0000  .U......<.......
00000010: 3e33 0b87 2700 0000 0c00 0000 7d02 0000  >3..'.......}...
00000020: c638 0000 3900 e00f 0000 0000 b165 0000  .8..9........e..
00000030: 0000 0000 0100 0000 0200 0000 0300 0000  ................
00000040: 0400 0000 0000 0000 0000 0000 0000 0000  ................
    )",
};

TEST_F(CpuReaderParsePagePayloadTest, F2fsTruncatePartialNodesNew) {
  const ExamplePage* test_case = &g_f2fs_truncate_partial_nodes_new;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  ds_config.event_filter.AddEnabledEvent(table->EventToFtraceId(
      GroupAndName("f2fs", "f2fs_truncate_partial_nodes")));

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();
  ASSERT_THAT(bundle.event(), SizeIs(1));
  auto& event = bundle.event()[0];
  EXPECT_EQ(event.f2fs_truncate_partial_nodes().dev(), 65081u);
  EXPECT_EQ(event.f2fs_truncate_partial_nodes().ino(), 26033u);
  // This field is disabled in ftrace_proto_gen.cc
  EXPECT_FALSE(event.f2fs_truncate_partial_nodes().has_nid());
  EXPECT_EQ(event.f2fs_truncate_partial_nodes().depth(), 4);
  EXPECT_EQ(event.f2fs_truncate_partial_nodes().err(), 0);
}

// Kernel code:
// trace_f2fs_truncate_partial_nodes(... nid = {1,2,3}, depth = 4, err = 0)
//
// Before kernel commit 0b04d4c0542e("f2fs: Fix
// f2fs_truncate_partial_nodes ftrace event")
static ExamplePage g_f2fs_truncate_partial_nodes_old{
    "b281660544_old",
    R"(
00000000: 8f90 aa0d 9e00 0000 3c00 0000 0000 0000  ........<.......
00000010: 3e97 0295 0e01 0000 0c00 0000 7d02 0000  >...........}...
00000020: 8021 0000 3900 e00f 0000 0000 0d66 0000  .!..9........f..
00000030: 0000 0000 0100 0000 0200 0000 0300 0000  ................
00000040: 0400 0000 0000 0000 0000 0000 0000 0000  ................
    )",
};

TEST_F(CpuReaderParsePagePayloadTest, F2fsTruncatePartialNodesOld) {
  const ExamplePage* test_case = &g_f2fs_truncate_partial_nodes_old;

  ProtoTranslationTable* table = GetTable(test_case->name);
  auto page = PageFromXxd(test_case->data);

  FtraceDataSourceConfig ds_config = EmptyConfig();
  auto id = table->EventToFtraceId(
      GroupAndName("f2fs", "f2fs_truncate_partial_nodes"));
  PERFETTO_LOG("Enabling: %zu", id);
  ds_config.event_filter.AddEnabledEvent(id);

  const uint8_t* parse_pos = page.get();
  std::optional<CpuReader::PageHeader> page_header =
      CpuReader::ParsePageHeader(&parse_pos, table->page_header_size_len());

  const uint8_t* page_end = page.get() + base::GetSysPageSize();
  ASSERT_TRUE(page_header.has_value());
  EXPECT_FALSE(page_header->lost_events);
  EXPECT_LE(parse_pos + page_header->size, page_end);

  FtraceParseStatus status = CpuReader::ParsePagePayload(
      parse_pos, &page_header.value(), table, &ds_config,
      CreateBundler(ds_config), &metadata_, &last_read_event_ts_);

  EXPECT_EQ(status, FtraceParseStatus::FTRACE_STATUS_OK);

  auto bundle = GetBundle();
  ASSERT_THAT(bundle.event(), SizeIs(1));
  auto& event = bundle.event()[0];
  EXPECT_EQ(event.f2fs_truncate_partial_nodes().dev(), 65081u);
  EXPECT_EQ(event.f2fs_truncate_partial_nodes().ino(), 26125u);
  // This field is disabled in ftrace_proto_gen.cc
  EXPECT_FALSE(event.f2fs_truncate_partial_nodes().has_nid());
  // Due to a kernel bug, nid[1] is parsed as depth.
  EXPECT_EQ(event.f2fs_truncate_partial_nodes().depth(), 2);
  // Due to a kernel bug, nid[2] is parsed as err.
  EXPECT_EQ(event.f2fs_truncate_partial_nodes().err(), 3);
}

// one print
char g_last_ts_test_page_0[] = R"(
    00000000: cd79 fb3a 2fa4 0400 2c00 0000 0000 0000  .y.:/...,.......
    00000010: 7eb6 e5eb 8f11 0000 0800 0000 0500 0000  ~...............
    00000020: 1e83 1400 42ab e0af ffff ffff 6669 7273  ....B.......firs
    00000030: 745f 7072 696e 740a 0000 0000 0000 0000  t_print.........
  )";

// one print
char g_last_ts_test_page_1[] = R"(
    00000000: 3c11 d579 99a5 0400 2c00 0000 0000 0000  <..y....,.......
    00000010: 3ed1 6315 3701 0000 0800 0000 0500 0000  >.c.7...........
    00000020: 9e8c 1400 42ab e0af ffff ffff 7365 636f  ....B.......seco
    00000030: 6e64 5f70 7269 6e74 0a00 0000 0000 0000  nd_print........
  )";

// data loss marker ("since last read") + multiple sched_switch + one print
char g_last_ts_test_page_2[] = R"(
    00000000: 8ac6 cb70 a8a5 0400 4c02 0080 ffff ffff  ...p....L.......
    00000010: 1000 0000 4701 0102 01b1 0f00 636f 6465  ....G.......code
    00000020: 0000 0000 0000 0000 0000 0000 01b1 0f00  ................
    00000030: 7800 0000 0100 0000 0000 0000 7377 6170  x...........swap
    00000040: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
    00000050: 7800 0000 b0e3 f602 4701 0102 0000 0000  x.......G.......
    00000060: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
    00000070: 0000 0000 7800 0000 0000 0000 0000 0000  ....x...........
    00000080: 6b77 6f72 6b65 722f 303a 3500 0000 0000  kworker/0:5.....
    00000090: ac85 1400 7800 0000 1002 0300 4701 0102  ....x.......G...
    000000a0: ac85 1400 6b77 6f72 6b65 722f 303a 3500  ....kworker/0:5.
    000000b0: 0000 0000 ac85 1400 7800 0000 8000 0000  ........x.......
    000000c0: 0000 0000 7377 6170 7065 722f 3000 0000  ....swapper/0...
    000000d0: 0000 0000 0000 0000 7800 0000 f086 7106  ........x.....q.
    000000e0: 4701 0102 0000 0000 7377 6170 7065 722f  G.......swapper/
    000000f0: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
    00000100: 0000 0000 0000 0000 6f62 6e6f 2d64 6573  ........obno-des
    00000110: 6b74 6f70 2d6e 6f00 d513 0000 7800 0000  ktop-no.....x...
    00000120: 3013 1000 4701 0102 d513 0000 6f62 6e6f  0...G.......obno
    00000130: 2d64 6573 6b74 6f70 2d6e 6f00 d513 0000  -desktop-no.....
    00000140: 7800 0000 0100 0000 0000 0000 7377 6170  x...........swap
    00000150: 7065 722f 3000 0000 0000 0000 0000 0000  per/0...........
    00000160: 7800 0000 10b0 2703 4701 0102 0000 0000  x.....'.G.......
    00000170: 7377 6170 7065 722f 3000 0000 0000 0000  swapper/0.......
    00000180: 0000 0000 7800 0000 0000 0000 0000 0000  ....x...........
    00000190: 6b77 6f72 6b65 722f 303a 3500 0000 0000  kworker/0:5.....
    000001a0: ac85 1400 7800 0000 70e7 0200 4701 0102  ....x...p...G...
    000001b0: ac85 1400 6b77 6f72 6b65 722f 303a 3500  ....kworker/0:5.
    000001c0: 0000 0000 ac85 1400 7800 0000 8000 0000  ........x.......
    000001d0: 0000 0000 6b73 6f66 7469 7271 642f 3000  ....ksoftirqd/0.
    000001e0: 0000 0000 0f00 0000 7800 0000 10a4 0200  ........x.......
    000001f0: 4701 0102 0f00 0000 6b73 6f66 7469 7271  G.......ksoftirq
    00000200: 642f 3000 0000 0000 0f00 0000 7800 0000  d/0.........x...
    00000210: 0100 0000 0000 0000 7377 6170 7065 722f  ........swapper/
    00000220: 3000 0000 0000 0000 0000 0000 7800 0000  0...........x...
    00000230: fef2 0a4d 7500 0000 0800 0000 0500 0000  ...Mu...........
    00000240: 1a8d 1400 42ab e0af ffff ffff 7468 6972  ....B.......thir
    00000250: 645f 7072 696e 740a 0000 0000 0000 0000  d_print.........
  )";

// Tests that |previous_bundle_end_timestamp| is correctly updated in cases
// where a single ProcessPagesForDataSource call produces multiple ftrace bundle
// packets (due to splitting on data loss markers).
TEST(CpuReaderTest, LastReadEventTimestampWithSplitBundles) {
  // build test buffer with 3 pages
  ProtoTranslationTable* table = GetTable("synthetic");
  std::vector<std::unique_ptr<uint8_t[]>> test_pages;
  test_pages.emplace_back(PageFromXxd(g_last_ts_test_page_0));
  test_pages.emplace_back(PageFromXxd(g_last_ts_test_page_1));
  test_pages.emplace_back(PageFromXxd(g_last_ts_test_page_2));
  size_t num_pages = test_pages.size();
  size_t page_sz = base::GetSysPageSize();
  auto buf = std::make_unique<uint8_t[]>(page_sz * num_pages);
  for (size_t i = 0; i < num_pages; i++) {
    void* dest = buf.get() + (i * page_sz);
    memcpy(dest, static_cast<const void*>(test_pages[i].get()), page_sz);
  }

  // build cfg requesting ftrace/print
  auto compact_sched_buf = std::make_unique<CompactSchedBuffer>();
  FtraceMetadata metadata{};
  FtraceDataSourceConfig ftrace_cfg = EmptyConfig();
  ftrace_cfg.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("ftrace", "print")));

  // invoke ProcessPagesForDataSource
  TraceWriterForTesting trace_writer;
  base::FlatSet<protos::pbzero::FtraceParseStatus> parse_errors;
  uint64_t last_read_event_ts = 0;
  bool success = CpuReader::ProcessPagesForDataSource(
      &trace_writer, &metadata, /*cpu=*/0, &ftrace_cfg, &parse_errors,
      &last_read_event_ts, buf.get(), num_pages, compact_sched_buf.get(), table,
      /*symbolizer=*/nullptr,
      /*ftrace_clock_snapshot=*/nullptr,
      protos::pbzero::FTRACE_CLOCK_UNSPECIFIED);

  EXPECT_TRUE(success);

  // We've read three pages, one print event on each. There is a data loss
  // marker on the third page, indicating that the kernel overwrote events
  // between 2nd and 3rd page (imagine our daemon getting cpu starved between
  // those reads).
  //
  // Therefore we expect two bundles, as we start a new one whenever we
  // encounter data loss (to set the |lost_events| field in the bundle proto).
  //
  // In terms of |previous_bundle_end_timestamp|, the first bundle will emit
  // zero since that's our initial input. The second bundle needs to emit the
  // timestamp of the last event in the first bundle.
  auto packets = trace_writer.GetAllTracePackets();
  ASSERT_EQ(2u, packets.size());

  // 2 prints
  auto const& first_bundle = packets[0].ftrace_events();
  EXPECT_FALSE(first_bundle.lost_events());
  ASSERT_EQ(2u, first_bundle.event().size());
  EXPECT_TRUE(first_bundle.has_previous_bundle_end_timestamp());
  EXPECT_EQ(0u, first_bundle.previous_bundle_end_timestamp());

  const uint64_t kSecondPrintTs = 1308020252356549ULL;
  EXPECT_EQ(kSecondPrintTs, first_bundle.event()[1].timestamp());
  EXPECT_EQ(0u, first_bundle.previous_bundle_end_timestamp());

  // 1 print + lost_events + updated previous_bundle_end_timestamp
  auto const& second_bundle = packets[1].ftrace_events();
  EXPECT_TRUE(second_bundle.lost_events());
  EXPECT_EQ(1u, second_bundle.event().size());
  EXPECT_EQ(kSecondPrintTs, second_bundle.previous_bundle_end_timestamp());
}

}  // namespace
}  // namespace perfetto
