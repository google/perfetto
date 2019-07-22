/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/proto_trace_tokenizer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/metadata.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/proto_trace_parser.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/systrace_parser.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/virtual_track_tracker.h"

#include "perfetto/common/sys_stats_counters.pbzero.h"
#include "perfetto/trace/android/packages_list.pbzero.h"
#include "perfetto/trace/chrome/chrome_benchmark_metadata.pbzero.h"
#include "perfetto/trace/ftrace/ftrace.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/ftrace/generic.pbzero.h"
#include "perfetto/trace/ftrace/power.pbzero.h"
#include "perfetto/trace/ftrace/sched.pbzero.h"
#include "perfetto/trace/ftrace/task.pbzero.h"
#include "perfetto/trace/interned_data/interned_data.pbzero.h"
#include "perfetto/trace/ps/process_tree.pbzero.h"
#include "perfetto/trace/sys_stats/sys_stats.pbzero.h"
#include "perfetto/trace/trace.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "perfetto/trace/track_event/task_execution.pbzero.h"
#include "perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::Args;
using ::testing::AtLeast;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::InvokeArgument;
using ::testing::NiceMock;
using ::testing::Pointwise;
using ::testing::Return;
using ::testing::UnorderedElementsAreArray;

class MockEventTracker : public EventTracker {
 public:
  MockEventTracker(TraceProcessorContext* context) : EventTracker(context) {}
  virtual ~MockEventTracker() = default;

  MOCK_METHOD9(PushSchedSwitch,
               void(uint32_t cpu,
                    int64_t timestamp,
                    uint32_t prev_pid,
                    base::StringView prev_comm,
                    int32_t prev_prio,
                    int64_t prev_state,
                    uint32_t next_pid,
                    base::StringView next_comm,
                    int32_t next_prio));

  MOCK_METHOD6(PushCounter,
               RowId(int64_t timestamp,
                     double value,
                     StringId name_id,
                     int64_t ref,
                     RefType ref_type,
                     bool resolve_utid_to_upid));

  MOCK_METHOD6(PushInstant,
               RowId(int64_t timestamp,
                     StringId name_id,
                     double value,
                     int64_t ref,
                     RefType ref_type,
                     bool resolve_utid_to_upid));
};

class MockProcessTracker : public ProcessTracker {
 public:
  MockProcessTracker(TraceProcessorContext* context)
      : ProcessTracker(context) {}

  MOCK_METHOD3(SetProcessMetadata,
               UniquePid(uint32_t pid,
                         base::Optional<uint32_t> ppid,
                         base::StringView process_name));

  MOCK_METHOD2(UpdateThreadName,
               UniqueTid(uint32_t tid, StringId thread_name_id));
  MOCK_METHOD2(UpdateThread, UniqueTid(uint32_t tid, uint32_t tgid));

  MOCK_METHOD1(GetOrCreateProcess, UniquePid(uint32_t pid));
};

// Mock trace storage that behaves like the real implementation, but allows for
// the interactions with string interning/lookup to be overridden/inspected.
class MockTraceStorage : public TraceStorage {
 public:
  MockTraceStorage() : TraceStorage() {
    ON_CALL(*this, InternString(_))
        .WillByDefault(Invoke([this](base::StringView str) {
          return TraceStorage::InternString(str);
        }));

    ON_CALL(*this, GetString(_)).WillByDefault(Invoke([this](StringId id) {
      return TraceStorage::GetString(id);
    }));
  }

  MOCK_METHOD1(InternString, StringId(base::StringView));
  MOCK_CONST_METHOD1(GetString, NullTermStringView(StringId));
};

class MockArgsTracker : public ArgsTracker {
 public:
  MockArgsTracker(TraceProcessorContext* context) : ArgsTracker(context) {}

  MOCK_METHOD4(AddArg,
               void(RowId row_id, StringId flat_key, StringId key, Variadic));
  MOCK_METHOD0(Flush, void());
};

class MockSliceTracker : public SliceTracker {
 public:
  MockSliceTracker(TraceProcessorContext* context) : SliceTracker(context) {}

  MOCK_METHOD6(Begin,
               base::Optional<uint32_t>(int64_t timestamp,
                                        int64_t ref,
                                        RefType ref_type,
                                        StringId cat,
                                        StringId name,
                                        SetArgsCallback args_callback));
  MOCK_METHOD6(End,
               base::Optional<uint32_t>(int64_t timestamp,
                                        int64_t ref,
                                        RefType ref_type,
                                        StringId cat,
                                        StringId name,
                                        SetArgsCallback args_callback));
  MOCK_METHOD7(Scoped,
               base::Optional<uint32_t>(int64_t timestamp,
                                        int64_t ref,
                                        RefType ref_type,
                                        StringId cat,
                                        StringId name,
                                        int64_t duration,
                                        SetArgsCallback args_callback));
};

class ProtoTraceParserTest : public ::testing::Test {
 public:
  ProtoTraceParserTest() {
    storage_ = new NiceMock<MockTraceStorage>();
    context_.storage.reset(storage_);
    context_.virtual_track_tracker.reset(new VirtualTrackTracker(&context_));
    context_.args_tracker.reset(new ArgsTracker(&context_));
    event_ = new MockEventTracker(&context_);
    context_.event_tracker.reset(event_);
    process_ = new MockProcessTracker(&context_);
    context_.process_tracker.reset(process_);
    slice_ = new MockSliceTracker(&context_);
    context_.slice_tracker.reset(slice_);
    context_.sorter.reset(new TraceSorter(&context_, 0 /*window size*/));
    context_.parser.reset(new ProtoTraceParser(&context_));
    context_.systrace_parser.reset(new SystraceParser(&context_));
  }

  void ResetTraceBuffers() {
    heap_buf_.reset(new protozero::ScatteredHeapBuffer());
    stream_writer_.reset(new protozero::ScatteredStreamWriter(heap_buf_.get()));
    heap_buf_->set_writer(stream_writer_.get());
    trace_.Reset(stream_writer_.get());
  }

  void SetUp() override { ResetTraceBuffers(); }

  void Tokenize() {
    trace_.Finalize();
    std::vector<uint8_t> trace_bytes = heap_buf_->StitchSlices();
    std::unique_ptr<uint8_t[]> raw_trace(new uint8_t[trace_bytes.size()]);
    memcpy(raw_trace.get(), trace_bytes.data(), trace_bytes.size());
    context_.chunk_reader.reset(new ProtoTraceTokenizer(&context_));
    context_.chunk_reader->Parse(std::move(raw_trace), trace_bytes.size());

    ResetTraceBuffers();
  }

  bool HasArg(ArgSetId set_id, StringId key_id, Variadic value) {
    const auto& args = storage_->args();
    auto rows =
        std::equal_range(args.set_ids().begin(), args.set_ids().end(), set_id);
    for (; rows.first != rows.second; rows.first++) {
      size_t index = static_cast<size_t>(
          std::distance(args.set_ids().begin(), rows.first));
      if (args.keys()[index] == key_id) {
        EXPECT_EQ(args.flat_keys()[index], key_id);
        EXPECT_EQ(args.arg_values()[index], value);
        if (args.flat_keys()[index] == key_id &&
            args.arg_values()[index] == value) {
          return true;
        }
      }
    }
    return false;
  }

 protected:
  std::unique_ptr<protozero::ScatteredHeapBuffer> heap_buf_;
  std::unique_ptr<protozero::ScatteredStreamWriter> stream_writer_;
  protos::pbzero::Trace trace_;
  TraceProcessorContext context_;
  MockEventTracker* event_;
  MockProcessTracker* process_;
  MockSliceTracker* slice_;
  NiceMock<MockTraceStorage>* storage_;
};

TEST_F(ProtoTraceParserTest, LoadSingleEvent) {
  auto* bundle = trace_.add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);

  static const char kProc1Name[] = "proc1";
  static const char kProc2Name[] = "proc2";
  auto* sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_comm(kProc2Name);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProc1Name);
  sched_switch->set_next_pid(100);
  sched_switch->set_next_prio(1024);

  EXPECT_CALL(*event_,
              PushSchedSwitch(10, 1000, 10, base::StringView(kProc2Name), 256,
                              32, 100, base::StringView(kProc1Name), 1024));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadEventsIntoRaw) {
  auto* bundle = trace_.add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  // This event is unknown and will only appear in
  // raw events table.
  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);
  auto* task = event->set_task_newtask();
  task->set_pid(123);
  static const char task_newtask[] = "task_newtask";
  task->set_comm(task_newtask);
  task->set_clone_flags(12);
  task->set_oom_score_adj(15);

  // This event has specific parsing logic, but will
  // also appear in raw events table.
  event = bundle->add_event();
  event->set_timestamp(1001);
  event->set_pid(12);
  auto* print = event->set_print();
  print->set_ip(20);
  static const char buf_value[] = "This is a print event";
  print->set_buf(buf_value);

  EXPECT_CALL(*storage_, InternString(base::StringView(task_newtask)))
      .Times(AtLeast(1));
  EXPECT_CALL(*storage_, InternString(base::StringView(buf_value)));
  EXPECT_CALL(*process_, UpdateThread(123, 123));

  Tokenize();

  const auto& raw = context_.storage->raw_events();
  ASSERT_EQ(raw.raw_event_count(), 2u);
  const auto& args = context_.storage->args();
  ASSERT_EQ(args.args_count(), 6u);
  ASSERT_EQ(args.arg_values()[0].int_value, 123);
  ASSERT_STREQ(
      context_.storage->GetString(args.arg_values()[1].string_value).c_str(),
      task_newtask);
  ASSERT_EQ(args.arg_values()[2].int_value, 12);
  ASSERT_EQ(args.arg_values()[3].int_value, 15);
  ASSERT_EQ(args.arg_values()[4].int_value, 20);
  ASSERT_STREQ(
      context_.storage->GetString(args.arg_values()[5].string_value).c_str(),
      buf_value);

  // TODO(taylori): Add test ftrace event with all field types
  // and test here.
}

TEST_F(ProtoTraceParserTest, LoadGenericFtrace) {
  auto* packet = trace_.add_packet();
  packet->set_timestamp(100);

  auto* bundle = packet->set_ftrace_events();
  bundle->set_cpu(4);

  auto* ftrace = bundle->add_event();
  ftrace->set_timestamp(100);
  ftrace->set_pid(10);

  auto* generic = ftrace->set_generic();
  generic->set_event_name("Test");

  auto* field = generic->add_field();
  field->set_name("meta1");
  field->set_str_value("value1");

  field = generic->add_field();
  field->set_name("meta2");
  field->set_int_value(-2);

  field = generic->add_field();
  field->set_name("meta3");
  field->set_uint_value(3);

  EXPECT_CALL(*storage_, InternString(base::StringView("Test")));
  EXPECT_CALL(*storage_, InternString(base::StringView("meta1")));
  EXPECT_CALL(*storage_, InternString(base::StringView("value1")));
  EXPECT_CALL(*storage_, InternString(base::StringView("meta2")));
  EXPECT_CALL(*storage_, InternString(base::StringView("meta3")));

  Tokenize();

  const auto& raw = storage_->raw_events();

  ASSERT_EQ(raw.raw_event_count(), 1u);
  ASSERT_EQ(raw.timestamps().back(), 100);
  ASSERT_EQ(storage_->GetThread(raw.utids().back()).tid, 10u);

  auto set_id = raw.arg_set_ids().back();

  const auto& args = storage_->args();
  auto id_it =
      std::equal_range(args.set_ids().begin(), args.set_ids().end(), set_id);

  // Ignore string calls as they are handled by checking InternString calls
  // above.

  auto it = id_it.first;
  auto row = static_cast<size_t>(std::distance(args.set_ids().begin(), it));
  ASSERT_EQ(args.arg_values()[++row].int_value, -2);
  ASSERT_EQ(args.arg_values()[++row].int_value, 3);
}

TEST_F(ProtoTraceParserTest, LoadMultipleEvents) {
  auto* bundle = trace_.add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);

  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";
  auto* sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName1);
  sched_switch->set_next_pid(100);
  sched_switch->set_next_prio(1024);

  event = bundle->add_event();
  event->set_timestamp(1001);
  event->set_pid(12);

  sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName2);
  sched_switch->set_next_pid(10);
  sched_switch->set_next_prio(512);

  EXPECT_CALL(*event_,
              PushSchedSwitch(10, 1000, 10, base::StringView(kProcName2), 256,
                              32, 100, base::StringView(kProcName1), 1024));

  EXPECT_CALL(*event_,
              PushSchedSwitch(10, 1001, 100, base::StringView(kProcName1), 256,
                              32, 10, base::StringView(kProcName2), 512));

  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadMultiplePackets) {
  auto* bundle = trace_.add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);

  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";
  auto* sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName1);
  sched_switch->set_next_pid(100);
  sched_switch->set_next_prio(1024);

  bundle = trace_.add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  event = bundle->add_event();
  event->set_timestamp(1001);
  event->set_pid(12);

  sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName2);
  sched_switch->set_next_pid(10);
  sched_switch->set_next_prio(512);

  EXPECT_CALL(*event_,
              PushSchedSwitch(10, 1000, 10, base::StringView(kProcName2), 256,
                              32, 100, base::StringView(kProcName1), 1024));

  EXPECT_CALL(*event_,
              PushSchedSwitch(10, 1001, 100, base::StringView(kProcName1), 256,
                              32, 10, base::StringView(kProcName2), 512));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, RepeatedLoadSinglePacket) {
  auto* bundle = trace_.add_packet()->set_ftrace_events();
  bundle->set_cpu(10);
  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);
  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";
  auto* sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName1);
  sched_switch->set_next_pid(100);
  sched_switch->set_next_prio(1024);
  EXPECT_CALL(*event_,
              PushSchedSwitch(10, 1000, 10, base::StringView(kProcName2), 256,
                              32, 100, base::StringView(kProcName1), 1024));
  Tokenize();

  bundle = trace_.add_packet()->set_ftrace_events();
  bundle->set_cpu(10);
  event = bundle->add_event();
  event->set_timestamp(1001);
  event->set_pid(12);
  sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName2);
  sched_switch->set_next_pid(10);
  sched_switch->set_next_prio(512);

  EXPECT_CALL(*event_,
              PushSchedSwitch(10, 1001, 100, base::StringView(kProcName1), 256,
                              32, 10, base::StringView(kProcName2), 512));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadMemInfo) {
  auto* packet = trace_.add_packet();
  uint64_t ts = 1000;
  packet->set_timestamp(ts);
  auto* bundle = packet->set_sys_stats();
  auto* meminfo = bundle->add_meminfo();
  meminfo->set_key(protos::pbzero::MEMINFO_MEM_TOTAL);
  uint32_t value = 10;
  meminfo->set_value(value);

  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts), value * 1024, _, 0,
                                   RefType::kRefNoRef, false));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadVmStats) {
  auto* packet = trace_.add_packet();
  uint64_t ts = 1000;
  packet->set_timestamp(ts);
  auto* bundle = packet->set_sys_stats();
  auto* meminfo = bundle->add_vmstat();
  meminfo->set_key(protos::pbzero::VMSTAT_COMPACT_SUCCESS);
  uint32_t value = 10;
  meminfo->set_value(value);

  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts), value, _, 0,
                                   RefType::kRefNoRef, false));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadCpuFreq) {
  auto* bundle = trace_.add_packet()->set_ftrace_events();
  bundle->set_cpu(12);
  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);
  auto* cpu_freq = event->set_cpu_frequency();
  cpu_freq->set_cpu_id(10);
  cpu_freq->set_state(2000);

  EXPECT_CALL(*event_,
              PushCounter(1000, 2000, _, 10, RefType::kRefCpuId, false));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadProcessPacket) {
  auto* tree = trace_.add_packet()->set_process_tree();
  auto* process = tree->add_processes();
  static const char kProcName1[] = "proc1";

  process->add_cmdline(kProcName1);
  process->set_pid(1);
  process->set_ppid(3);

  EXPECT_CALL(*process_,
              SetProcessMetadata(1, Eq(3u), base::StringView(kProcName1)));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadProcessPacket_FirstCmdline) {
  auto* tree = trace_.add_packet()->set_process_tree();
  auto* process = tree->add_processes();
  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";

  process->add_cmdline(kProcName1);
  process->add_cmdline(kProcName2);
  process->set_pid(1);
  process->set_ppid(3);

  EXPECT_CALL(*process_,
              SetProcessMetadata(1, Eq(3u), base::StringView(kProcName1)));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadThreadPacket) {
  auto* tree = trace_.add_packet()->set_process_tree();
  auto* thread = tree->add_threads();
  thread->set_tid(1);
  thread->set_tgid(2);

  EXPECT_CALL(*process_, UpdateThread(1, 2));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, ThreadNameFromThreadDescriptor) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
    thread_desc->set_thread_name("OldThreadName");
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
    thread_desc->set_thread_name("NewThreadName");
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(2);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(11);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
    thread_desc->set_thread_name("DifferentThreadName");
  }

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*storage_, InternString(base::StringView("OldThreadName")))
      .WillOnce(Return(1));
  EXPECT_CALL(*process_, UpdateThreadName(16, 1));
  // Packet with same thread, but different name should update the name.
  EXPECT_CALL(*storage_, InternString(base::StringView("NewThreadName")))
      .WillOnce(Return(2));
  EXPECT_CALL(*process_, UpdateThreadName(16, 2));
  EXPECT_CALL(*storage_, InternString(base::StringView("DifferentThreadName")))
      .WillOnce(Return(3));
  EXPECT_CALL(*process_, UpdateThreadName(11, 3));

  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithoutInternedData) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1020.
    event->set_thread_time_delta_us(5);  // absolute: 2010.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1005);
    event->set_thread_time_absolute_us(2003);
    event->add_category_iids(2);
    event->add_category_iids(3);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(2);
    legacy_event->set_phase('X');
    legacy_event->set_duration_us(23);         // absolute end: 1028.
    legacy_event->set_thread_duration_us(12);  // absolute end: 2015.
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15))
      .Times(3)
      .WillRepeatedly(Return(1));

  MockArgsTracker args(&context_);

  InSequence in_sequence;  // Below slices should be sorted by timestamp.
  EXPECT_CALL(*slice_, Scoped(1005000, 1, RefType::kRefUtid, 0, 0, 23000, _))
      .WillOnce(DoAll(
          InvokeArgument<6>(
              &args, TraceStorage::CreateRowId(TableId::kNestableSlices, 0u)),
          Return(0u)));
  EXPECT_CALL(*slice_, Begin(1010000, 1, RefType::kRefUtid, 0, 0, _))
      .WillOnce(DoAll(
          InvokeArgument<5>(
              &args, TraceStorage::CreateRowId(TableId::kNestableSlices, 1u)),
          Return(1u)));
  EXPECT_CALL(*slice_, End(1020000, 1, RefType::kRefUtid, 0, 0, _))
      .WillOnce(DoAll(
          InvokeArgument<5>(
              &args, TraceStorage::CreateRowId(TableId::kNestableSlices, 1u)),
          Return(1u)));

  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(storage_->thread_slices().slice_count(), 2u);
  EXPECT_EQ(storage_->thread_slices().slice_ids()[0], 0u);
  EXPECT_EQ(storage_->thread_slices().thread_timestamp_ns()[0], 2003000);
  EXPECT_EQ(storage_->thread_slices().thread_duration_ns()[0], 12000);
  EXPECT_EQ(storage_->thread_slices().slice_ids()[1], 1u);
  EXPECT_EQ(storage_->thread_slices().thread_timestamp_ns()[1], 2005000);
  EXPECT_EQ(storage_->thread_slices().thread_duration_ns()[1], 5000);
}

TEST_F(ProtoTraceParserTest, TrackEventWithInternedData) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');

    auto* interned_data = packet->set_interned_data();
    auto cat1 = interned_data->add_event_categories();
    cat1->set_iid(1);
    cat1->set_name("cat1");
    auto ev1 = interned_data->add_legacy_event_names();
    ev1->set_iid(1);
    ev1->set_name("ev1");
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1040);
    event->set_thread_time_absolute_us(2030);
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('I');
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1050);
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('i');
    legacy_event->set_instant_event_scope(
        protos::pbzero::TrackEvent::LegacyEvent::SCOPE_PROCESS);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1020.
    event->set_thread_time_delta_us(5);  // absolute: 2010.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1005);
    event->set_thread_time_absolute_us(2003);
    event->add_category_iids(2);
    event->add_category_iids(3);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(2);
    legacy_event->set_phase('X');
    legacy_event->set_duration_us(23);         // absolute end: 1028.
    legacy_event->set_thread_duration_us(12);  // absolute end: 2015.

    auto* interned_data = packet->set_interned_data();
    auto cat2 = interned_data->add_event_categories();
    cat2->set_iid(2);
    cat2->set_name("cat2");
    auto cat3 = interned_data->add_event_categories();
    cat3->set_iid(3);
    cat3->set_name("cat3");
    auto ev2 = interned_data->add_legacy_event_names();
    ev2->set_iid(2);
    ev2->set_name("ev2");
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15))
      .Times(5)
      .WillRepeatedly(Return(1));

  EXPECT_CALL(*process_, GetOrCreateProcess(15)).WillOnce(Return(2));

  MockArgsTracker args(&context_);

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*storage_, InternString(base::StringView("cat2,cat3")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev2")))
      .WillOnce(Return(2));
  EXPECT_CALL(*slice_, Scoped(1005000, 1, RefType::kRefUtid, 1, 2, 23000, _))
      .WillOnce(DoAll(
          InvokeArgument<6>(
              &args, TraceStorage::CreateRowId(TableId::kNestableSlices, 0u)),
          Return(0u)));

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(3));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev1")))
      .WillOnce(Return(4));
  EXPECT_CALL(*slice_, Begin(1010000, 1, RefType::kRefUtid, 3, 4, _))
      .WillOnce(DoAll(
          InvokeArgument<5>(
              &args, TraceStorage::CreateRowId(TableId::kNestableSlices, 1u)),
          Return(1u)));

  EXPECT_CALL(*slice_, End(1020000, 1, RefType::kRefUtid, 3, 4, _))
      .WillOnce(DoAll(
          InvokeArgument<5>(
              &args, TraceStorage::CreateRowId(TableId::kNestableSlices, 1u)),
          Return(1u)));

  EXPECT_CALL(*slice_, Scoped(1040000, 1, RefType::kRefUtid, 3, 4, 0, _))
      .WillOnce(DoAll(
          InvokeArgument<6>(
              &args, TraceStorage::CreateRowId(TableId::kNestableSlices, 2u)),
          Return(2u)));

  EXPECT_CALL(*slice_, Scoped(1050000, 2, RefType::kRefUpid, 3, 4, 0, _))
      .WillOnce(DoAll(
          InvokeArgument<6>(
              &args, TraceStorage::CreateRowId(TableId::kNestableSlices, 3u)),
          Return(3u)));

  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(storage_->thread_slices().slice_count(), 3u);
  EXPECT_EQ(storage_->thread_slices().slice_ids()[0], 0u);
  EXPECT_EQ(storage_->thread_slices().thread_timestamp_ns()[0], 2003000);
  EXPECT_EQ(storage_->thread_slices().thread_duration_ns()[0], 12000);
  EXPECT_EQ(storage_->thread_slices().slice_ids()[1], 1u);
  EXPECT_EQ(storage_->thread_slices().thread_timestamp_ns()[1], 2005000);
  EXPECT_EQ(storage_->thread_slices().thread_duration_ns()[1], 5000);
  EXPECT_EQ(storage_->thread_slices().slice_ids()[2], 2u);
  EXPECT_EQ(storage_->thread_slices().thread_timestamp_ns()[2], 2030000);
  EXPECT_EQ(storage_->thread_slices().thread_duration_ns()[2], 0);
}

TEST_F(ProtoTraceParserTest, TrackEventAsyncEvents) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('b');
    legacy_event->set_global_id(10);
    legacy_event->set_use_async_tts(true);

    auto* interned_data = packet->set_interned_data();
    auto cat1 = interned_data->add_event_categories();
    cat1->set_iid(1);
    cat1->set_name("cat1");
    auto ev1 = interned_data->add_legacy_event_names();
    ev1->set_iid(1);
    ev1->set_name("ev1");
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1020.
    event->set_thread_time_delta_us(5);  // absolute: 2010.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('e');
    legacy_event->set_global_id(10);
    legacy_event->set_use_async_tts(true);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1015);
    event->add_category_iids(2);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(2);
    legacy_event->set_phase('n');
    legacy_event->set_global_id(10);

    auto* interned_data = packet->set_interned_data();
    auto cat2 = interned_data->add_event_categories();
    cat2->set_iid(2);
    cat2->set_name("cat2");
    auto ev2 = interned_data->add_legacy_event_names();
    ev2->set_iid(2);
    ev2->set_name("ev2");
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1030);
    event->add_category_iids(2);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(2);
    legacy_event->set_phase('n');
    legacy_event->set_local_id(15);
    legacy_event->set_id_scope("scope1");
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15))
      .Times(4)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(*process_, GetOrCreateProcess(15)).WillOnce(Return(1));

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev1")))
      .WillOnce(Return(2));
  EXPECT_CALL(*slice_, Begin(1010000, 0, RefType::kRefTrack, 1, 2, _))
      .WillOnce(Return(0u));

  EXPECT_CALL(*storage_, InternString(base::StringView("cat2")))
      .WillOnce(Return(3));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev2")))
      .WillOnce(Return(4));
  EXPECT_CALL(*slice_, Scoped(1015000, 0, RefType::kRefTrack, 3, 4, 0, _));

  EXPECT_CALL(*slice_, End(1020000, 0, RefType::kRefTrack, 1, 2, _))
      .WillOnce(Return(0u));

  EXPECT_CALL(*storage_, InternString(base::StringView("scope1")))
      .WillOnce(Return(5));
  EXPECT_CALL(*slice_, Scoped(1030000, 1, RefType::kRefTrack, 3, 4, 0, _));

  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(storage_->virtual_tracks().virtual_track_count(), 2u);
  EXPECT_EQ(storage_->virtual_tracks().track_ids()[0], 0u);
  EXPECT_EQ(storage_->virtual_tracks().track_ids()[1], 1u);
  EXPECT_EQ(storage_->virtual_tracks().names()[0], 2u);
  EXPECT_EQ(storage_->virtual_tracks().names()[1], 4u);
  EXPECT_EQ(storage_->virtual_tracks().scopes()[0], VirtualTrackScope::kGlobal);
  EXPECT_EQ(storage_->virtual_tracks().scopes()[1],
            VirtualTrackScope::kProcess);
  EXPECT_EQ(storage_->virtual_tracks().upids()[0], 0u);
  EXPECT_EQ(storage_->virtual_tracks().upids()[1], 1u);

  EXPECT_EQ(storage_->virtual_track_slices().slice_count(), 1u);
  EXPECT_EQ(storage_->virtual_track_slices().slice_ids()[0], 0u);
  EXPECT_EQ(storage_->virtual_track_slices().thread_timestamp_ns()[0], 2005000);
  EXPECT_EQ(storage_->virtual_track_slices().thread_duration_ns()[0], 5000);
}

TEST_F(ProtoTraceParserTest, TrackEventWithoutIncrementalStateReset) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    // Event should be discarded because incremental state was never cleared.
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');
  }

  Tokenize();

  EXPECT_CALL(*slice_, Begin(_, _, _, _, _, _)).Times(0);
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithoutThreadDescriptor) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  {
    // Event should be discarded because no thread descriptor was seen yet.
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);
    event->set_thread_time_delta_us(5);
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');
  }

  Tokenize();

  EXPECT_CALL(*slice_, Begin(_, _, _, _, _, _)).Times(0);
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithDataLoss) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');
  }
  {
    // Event should be dropped because data loss occurred before.
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_previous_packet_dropped(true);  // Data loss occurred.
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);
    event->set_thread_time_delta_us(5);
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }
  {
    // Event should be dropped because incremental state is invalid.
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);
    event->set_thread_time_delta_us(5);
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }
  {
    // Event should be dropped because no new thread descriptor was seen yet.
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);
    event->set_thread_time_delta_us(5);
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(2000);
    thread_desc->set_reference_thread_time_us(3000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 2010.
    event->set_thread_time_delta_us(5);  // absolute: 3005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15))
      .Times(2)
      .WillRepeatedly(Return(1));

  InSequence in_sequence;  // Below slices should be sorted by timestamp.
  EXPECT_CALL(*slice_, Begin(1010000, 1, RefType::kRefUtid, 0, 0, _));
  EXPECT_CALL(*slice_, End(2010000, 1, RefType::kRefUtid, 0, 0, _));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventMultipleSequences) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');

    auto* interned_data = packet->set_interned_data();
    auto cat1 = interned_data->add_event_categories();
    cat1->set_iid(1);
    cat1->set_name("cat1");
    auto ev1 = interned_data->add_legacy_event_names();
    ev1->set_iid(1);
    ev1->set_name("ev1");
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(2);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(17);
    thread_desc->set_reference_timestamp_us(995);
    thread_desc->set_reference_thread_time_us(3000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(2);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1005.
    event->set_thread_time_delta_us(5);  // absolute: 3005.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');

    auto* interned_data = packet->set_interned_data();
    auto cat1 = interned_data->add_event_categories();
    cat1->set_iid(1);
    cat1->set_name("cat1");
    auto ev2 = interned_data->add_legacy_event_names();
    ev2->set_iid(1);
    ev2->set_name("ev2");
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1020.
    event->set_thread_time_delta_us(5);  // absolute: 2010.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(2);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1015.
    event->set_thread_time_delta_us(5);  // absolute: 3015.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15))
      .Times(2)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(*process_, UpdateThread(17, 15))
      .Times(2)
      .WillRepeatedly(Return(2));

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev2")))
      .WillOnce(Return(2));

  EXPECT_CALL(*slice_, Begin(1005000, 2, RefType::kRefUtid, 1, 2, _));

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev1")))
      .WillOnce(Return(3));

  EXPECT_CALL(*slice_, Begin(1010000, 1, RefType::kRefUtid, 1, 3, _));
  EXPECT_CALL(*slice_, End(1015000, 2, RefType::kRefUtid, 1, 2, _));
  EXPECT_CALL(*slice_, End(1020000, 1, RefType::kRefUtid, 1, 3, _));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithDebugAnnotations) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));
  MockArgsTracker args(&context_);

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* annotation1 = event->add_debug_annotations();
    annotation1->set_name_iid(1);
    annotation1->set_uint_value(10u);
    auto* annotation2 = event->add_debug_annotations();
    annotation2->set_name_iid(2);
    auto* nested = annotation2->set_nested_value();
    nested->set_nested_type(protos::pbzero::DebugAnnotation::NestedValue::DICT);
    nested->add_dict_keys("child1");
    nested->add_dict_keys("child2");
    auto* child1 = nested->add_dict_values();
    child1->set_nested_type(
        protos::pbzero::DebugAnnotation::NestedValue::UNSPECIFIED);
    child1->set_bool_value(true);
    auto* child2 = nested->add_dict_values();
    child2->set_nested_type(
        protos::pbzero::DebugAnnotation::NestedValue::ARRAY);
    auto* child21 = child2->add_array_values();
    child21->set_nested_type(
        protos::pbzero::DebugAnnotation::NestedValue::UNSPECIFIED);
    child21->set_string_value("child21");
    auto* child22 = child2->add_array_values();
    child22->set_nested_type(
        protos::pbzero::DebugAnnotation::NestedValue::UNSPECIFIED);
    child22->set_double_value(2.2);
    auto* child23 = child2->add_array_values();
    child23->set_nested_type(
        protos::pbzero::DebugAnnotation::NestedValue::UNSPECIFIED);
    child23->set_int_value(23);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');

    auto* interned_data = packet->set_interned_data();
    auto cat1 = interned_data->add_event_categories();
    cat1->set_iid(1);
    cat1->set_name("cat1");
    auto ev1 = interned_data->add_legacy_event_names();
    ev1->set_iid(1);
    ev1->set_name("ev1");
    auto an1 = interned_data->add_debug_annotation_names();
    an1->set_iid(1);
    an1->set_name("an1");
    auto an2 = interned_data->add_debug_annotation_names();
    an2->set_iid(2);
    an2->set_name("an2");
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1020.
    event->set_thread_time_delta_us(5);  // absolute: 2010.
    event->add_category_iids(1);
    auto* annotation3 = event->add_debug_annotations();
    annotation3->set_name_iid(3);
    annotation3->set_int_value(-3);
    auto* annotation4 = event->add_debug_annotations();
    annotation4->set_name_iid(4);
    annotation4->set_bool_value(true);
    auto* annotation5 = event->add_debug_annotations();
    annotation5->set_name_iid(5);
    annotation5->set_double_value(-5.5);
    auto* annotation6 = event->add_debug_annotations();
    annotation6->set_name_iid(6);
    annotation6->set_pointer_value(20u);
    auto* annotation7 = event->add_debug_annotations();
    annotation7->set_name_iid(7);
    annotation7->set_string_value("val7");
    auto* annotation8 = event->add_debug_annotations();
    annotation8->set_name_iid(8);
    annotation8->set_legacy_json_value("val8");
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('E');

    auto* interned_data = packet->set_interned_data();
    auto an3 = interned_data->add_debug_annotation_names();
    an3->set_iid(3);
    an3->set_name("an3");
    auto an4 = interned_data->add_debug_annotation_names();
    an4->set_iid(4);
    an4->set_name("an4");
    auto an5 = interned_data->add_debug_annotation_names();
    an5->set_iid(5);
    an5->set_name("an5");
    auto an6 = interned_data->add_debug_annotation_names();
    an6->set_iid(6);
    an6->set_name("an6");
    auto an7 = interned_data->add_debug_annotation_names();
    an7->set_iid(7);
    an7->set_name("an7");
    auto an8 = interned_data->add_debug_annotation_names();
    an8->set_iid(8);
    an8->set_name("an8");
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15))
      .Times(2)
      .WillRepeatedly(Return(1));

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev1")))
      .WillOnce(Return(2));
  EXPECT_CALL(*slice_, Begin(1010000, 1, RefType::kRefUtid, 1, 2, _))
      .WillOnce(DoAll(InvokeArgument<5>(&args, 1u), Return(1u)));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an1")))
      .WillOnce(Return(3));
  EXPECT_CALL(args, AddArg(1u, 3, 3, Variadic::UnsignedInteger(10u)));

  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an2")))
      .WillOnce(Return(4));
  EXPECT_CALL(*storage_, GetString(4)).WillOnce(Return("debug.an2"));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an2.child1")))
      .Times(2)
      .WillRepeatedly(Return(5));
  EXPECT_CALL(args, AddArg(1u, 5, 5, Variadic::Boolean(true)));

  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an2.child2")))
      .WillOnce(Return(6));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an2.child2[0]")))
      .WillOnce(Return(7));
  EXPECT_CALL(*storage_, InternString(base::StringView("child21")))
      .WillOnce(Return(8));
  EXPECT_CALL(args, AddArg(1u, 6, 7, Variadic::String(8)));

  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an2.child2")))
      .WillOnce(Return(6));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an2.child2[1]")))
      .WillOnce(Return(9));
  EXPECT_CALL(args, AddArg(1u, 6, 9, Variadic::Real(2.2)));

  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an2.child2")))
      .WillOnce(Return(6));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an2.child2[2]")))
      .WillOnce(Return(10));
  EXPECT_CALL(args, AddArg(1u, 6, 10, Variadic::Integer(23)));

  EXPECT_CALL(*slice_, End(1020000, 1, RefType::kRefUtid, 1, 2, _))
      .WillOnce(DoAll(InvokeArgument<5>(&args, 1u), Return(1u)));

  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an3")))
      .WillOnce(Return(11));
  EXPECT_CALL(args, AddArg(1u, 11, 11, Variadic::Integer(-3)));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an4")))
      .WillOnce(Return(12));
  EXPECT_CALL(args, AddArg(1u, 12, 12, Variadic::Boolean(true)));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an5")))
      .WillOnce(Return(13));
  EXPECT_CALL(args, AddArg(1u, 13, 13, Variadic::Real(-5.5)));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an6")))
      .WillOnce(Return(14));
  EXPECT_CALL(args, AddArg(1u, 14, 14, Variadic::Pointer(20u)));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an7")))
      .WillOnce(Return(15));
  EXPECT_CALL(*storage_, InternString(base::StringView("val7")))
      .WillOnce(Return(16));
  EXPECT_CALL(args, AddArg(1u, 15, 15, Variadic::String(16)));
  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an8")))
      .WillOnce(Return(17));
  EXPECT_CALL(*storage_, InternString(base::StringView("val8")))
      .WillOnce(Return(18));
  EXPECT_CALL(args, AddArg(1u, 17, 17, Variadic::Json(18)));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithTaskExecution) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));
  MockArgsTracker args(&context_);

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);
    auto* task_execution = event->set_task_execution();
    task_execution->set_posted_from_iid(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('B');

    auto* interned_data = packet->set_interned_data();
    auto cat1 = interned_data->add_event_categories();
    cat1->set_iid(1);
    cat1->set_name("cat1");
    auto ev1 = interned_data->add_legacy_event_names();
    ev1->set_iid(1);
    ev1->set_name("ev1");
    auto loc1 = interned_data->add_source_locations();
    loc1->set_iid(1);
    loc1->set_file_name("file1");
    loc1->set_function_name("func1");
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15)).WillOnce(Return(1));

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev1")))
      .WillOnce(Return(2));
  EXPECT_CALL(*slice_, Begin(1010000, 1, RefType::kRefUtid, 1, 2, _))
      .WillOnce(DoAll(InvokeArgument<5>(&args, 1u), Return(1u)));
  EXPECT_CALL(*storage_, InternString(base::StringView("file1")))
      .WillOnce(Return(3));
  EXPECT_CALL(*storage_, InternString(base::StringView("func1")))
      .WillOnce(Return(4));
  EXPECT_CALL(args, AddArg(1u, _, _, Variadic::String(3)));
  EXPECT_CALL(args, AddArg(1u, _, _, Variadic::String(4)));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventParseLegacyEventIntoRawTable) {
  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = trace_.add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);   // absolute: 1010.
    event->set_thread_time_delta_us(5);  // absolute: 2005.
    event->add_category_iids(1);

    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    // Represents a phase that isn't parsed into regular trace processor tables.
    legacy_event->set_phase('?');
    legacy_event->set_duration_us(23);
    legacy_event->set_thread_duration_us(15);
    legacy_event->set_global_id(99u);
    legacy_event->set_id_scope("scope1");
    legacy_event->set_use_async_tts('?');
    legacy_event->set_bind_id(98);
    legacy_event->set_bind_to_enclosing(true);
    legacy_event->set_flow_direction(
        protos::pbzero::TrackEvent::LegacyEvent::FLOW_INOUT);

    auto* annotation1 = event->add_debug_annotations();
    annotation1->set_name_iid(1);
    annotation1->set_uint_value(10u);

    auto* interned_data = packet->set_interned_data();
    auto cat1 = interned_data->add_event_categories();
    cat1->set_iid(1);
    cat1->set_name("cat1");
    auto ev1 = interned_data->add_legacy_event_names();
    ev1->set_iid(1);
    ev1->set_name("ev1");
    auto an1 = interned_data->add_debug_annotation_names();
    an1->set_iid(1);
    an1->set_name("an1");
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15)).WillOnce(Return(1));

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev1")))
      .WillOnce(Return(2));
  EXPECT_CALL(*storage_, InternString(base::StringView("scope1")))
      .Times(2)
      .WillRepeatedly(Return(3));

  EXPECT_CALL(*storage_, InternString(base::StringView("debug.an1")))
      .WillOnce(Return(4));

  context_.sorter->ExtractEventsForced();

  ::testing::Mock::VerifyAndClearExpectations(storage_);

  // Verify raw_events and args contents.
  const auto& raw_events = storage_->raw_events();
  EXPECT_EQ(raw_events.raw_event_count(), 1u);
  EXPECT_EQ(raw_events.timestamps()[0], 1010000);
  EXPECT_EQ(raw_events.name_ids()[0],
            storage_->InternString("track_event.legacy_event"));
  EXPECT_EQ(raw_events.cpus()[0], 0u);
  EXPECT_EQ(raw_events.utids()[0], 1u);
  EXPECT_EQ(raw_events.arg_set_ids()[0], 1u);

  EXPECT_EQ(storage_->args().args_count(), 13u);

  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.category"),
                     Variadic::String(1u)));
  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.name"),
                     Variadic::String(2u)));
  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.phase"),
                     Variadic::Integer('?')));
  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.duration_ns"),
                     Variadic::Integer(23000)));
  EXPECT_TRUE(HasArg(1u,
                     storage_->InternString("legacy_event.thread_timestamp_ns"),
                     Variadic::Integer(2005000)));
  EXPECT_TRUE(HasArg(1u,
                     storage_->InternString("legacy_event.thread_duration_ns"),
                     Variadic::Integer(15000)));
  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.use_async_tts"),
                     Variadic::Boolean(true)));
  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.global_id"),
                     Variadic::UnsignedInteger(99u)));
  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.id_scope"),
                     Variadic::String(3u)));
  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.bind_id"),
                     Variadic::UnsignedInteger(98u)));
  EXPECT_TRUE(HasArg(1u,
                     storage_->InternString("legacy_event.bind_to_enclosing"),
                     Variadic::Boolean(true)));
  EXPECT_TRUE(HasArg(1u, storage_->InternString("legacy_event.flow_direction"),
                     Variadic::String(storage_->InternString("inout"))));
  EXPECT_TRUE(HasArg(1u, 4u, Variadic::UnsignedInteger(10u)));
}

TEST_F(ProtoTraceParserTest, LoadChromeBenchmarkMetadata) {
  static const char kName[] = "name";
  static const char kTag1[] = "tag1";
  static const char kTag2[] = "tag2";

  context_.sorter.reset(new TraceSorter(
      &context_, std::numeric_limits<int64_t>::max() /*window size*/));

  auto* metadata = trace_.add_packet()->set_chrome_benchmark_metadata();
  metadata->set_benchmark_name(kName);
  metadata->add_story_tags(kTag1);
  metadata->add_story_tags(kTag2);

  Tokenize();

  EXPECT_CALL(*storage_, InternString(base::StringView(kName)))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView(kTag1)))
      .WillOnce(Return(2));
  EXPECT_CALL(*storage_, InternString(base::StringView(kTag2)))
      .WillOnce(Return(3));

  context_.sorter->ExtractEventsForced();

  const auto& meta_keys = storage_->metadata().keys();
  const auto& meta_values = storage_->metadata().values();
  EXPECT_EQ(meta_keys.size(), 3u);
  std::vector<std::pair<metadata::KeyIDs, Variadic>> meta_entries;
  for (size_t i = 0; i < meta_keys.size(); i++) {
    meta_entries.emplace_back(std::make_pair(meta_keys[i], meta_values[i]));
  }
  EXPECT_THAT(
      meta_entries,
      UnorderedElementsAreArray(
          {std::make_pair(metadata::benchmark_name, Variadic::String(1)),
           std::make_pair(metadata::benchmark_story_tags, Variadic::String(2)),
           std::make_pair(metadata::benchmark_story_tags,
                          Variadic::String(3))}));
}

TEST_F(ProtoTraceParserTest, AndroidPackagesList) {
  auto* packet = trace_.add_packet();
  auto* pkg_list = packet->set_packages_list();

  pkg_list->set_read_error(false);
  pkg_list->set_parse_error(true);
  {
    auto* pkg = pkg_list->add_packages();
    pkg->set_name("com.test.app");
    pkg->set_uid(1000);
    pkg->set_debuggable(false);
    pkg->set_profileable_from_shell(true);
    pkg->set_version_code(42);
  }
  {
    auto* pkg = pkg_list->add_packages();
    pkg->set_name("com.test.app2");
    pkg->set_uid(1001);
    pkg->set_debuggable(false);
    pkg->set_profileable_from_shell(false);
    pkg->set_version_code(43);
  }

  Tokenize();

  // Packet-level errors reflected in stats storage.
  const auto& stats = context_.storage->stats();
  EXPECT_FALSE(stats[stats::packages_list_has_read_errors].value);
  EXPECT_TRUE(stats[stats::packages_list_has_parse_errors].value);

  // Expect two metadata rows, each with an int_value of a separate arg set id.
  // The relevant arg sets have the info about the packages. To simplify test
  // structure, make an assumption that metadata storage is filled in in the
  // FIFO order of seen packages.
  const auto& args = context_.storage->args();
  const auto& metadata = context_.storage->metadata();
  const auto& meta_keys = metadata.keys();
  const auto& meta_values = metadata.values();

  ASSERT_TRUE(std::count(meta_keys.cbegin(), meta_keys.cend(),
                         metadata::android_packages_list) == 2);

  auto first_meta_idx = std::distance(
      meta_keys.cbegin(), std::find(meta_keys.cbegin(), meta_keys.cend(),
                                    metadata::android_packages_list));
  auto second_meta_idx = std::distance(
      meta_keys.cbegin(),
      std::find(meta_keys.cbegin() + first_meta_idx + 1, meta_keys.cend(),
                metadata::android_packages_list));

  uint32_t first_set_id = static_cast<uint32_t>(
      meta_values[static_cast<size_t>(first_meta_idx)].int_value);
  uint32_t second_set_id = static_cast<uint32_t>(
      meta_values[static_cast<size_t>(second_meta_idx)].int_value);

  // helper to look up arg values
  auto find_arg = [&args, this](ArgSetId set_id, const char* arg_name) {
    for (size_t i = 0; i < args.set_ids().size(); i++) {
      if (args.set_ids()[i] == set_id &&
          args.keys()[i] == storage_->InternString(arg_name))
        return args.arg_values()[i];
    }
    PERFETTO_FATAL("Didn't find expected argument");
  };

  auto first_name_id = find_arg(first_set_id, "name").string_value;
  EXPECT_STREQ(storage_->GetString(first_name_id).c_str(), "com.test.app");
  EXPECT_EQ(find_arg(first_set_id, "uid").uint_value, 1000u);
  EXPECT_EQ(find_arg(first_set_id, "debuggable").bool_value, false);
  EXPECT_EQ(find_arg(first_set_id, "profileable_from_shell").bool_value, true);
  EXPECT_EQ(find_arg(first_set_id, "version_code").int_value, 42);

  auto second_name_id = find_arg(second_set_id, "name").string_value;
  EXPECT_STREQ(storage_->GetString(second_name_id).c_str(), "com.test.app2");
  EXPECT_EQ(find_arg(second_set_id, "uid").uint_value, 1001u);
  EXPECT_EQ(find_arg(second_set_id, "debuggable").bool_value, false);
  EXPECT_EQ(find_arg(second_set_id, "profileable_from_shell").bool_value,
            false);
  EXPECT_EQ(find_arg(second_set_id, "version_code").int_value, 43);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
