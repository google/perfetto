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
#include "src/trace_processor/trace_sorter.h"

#include "perfetto/common/sys_stats_counters.pbzero.h"
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
using ::testing::NiceMock;
using ::testing::Pointwise;
using ::testing::Return;

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

  MOCK_METHOD3(UpdateProcess,
               UniquePid(uint32_t pid,
                         base::Optional<uint32_t> ppid,
                         base::StringView process_name));

  MOCK_METHOD2(UpdateThread, UniqueTid(uint32_t tid, uint32_t tgid));
};

class MockTraceStorage : public TraceStorage {
 public:
  MockTraceStorage() : TraceStorage() {}

  MOCK_METHOD1(InternString, StringId(base::StringView));
  MOCK_CONST_METHOD1(GetString, NullTermStringView(StringId));
  MOCK_METHOD2(SetMetadata, void(size_t, Variadic));
  MOCK_METHOD2(AppendMetadata, void(size_t, Variadic));
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

  MOCK_METHOD5(Begin,
               void(int64_t timestamp,
                    UniqueTid utid,
                    StringId cat,
                    StringId name,
                    SetArgsCallback args_callback));
  MOCK_METHOD5(End,
               void(int64_t timestamp,
                    UniqueTid utid,
                    StringId cat,
                    StringId name,
                    SetArgsCallback args_callback));
  MOCK_METHOD6(Scoped,
               void(int64_t timestamp,
                    UniqueTid utid,
                    StringId cat,
                    StringId name,
                    int64_t duration,
                    SetArgsCallback args_callback));
};

class ProtoTraceParserTest : public ::testing::Test {
 public:
  ProtoTraceParserTest() {
    nice_storage_ = new NiceMock<MockTraceStorage>();
    context_.storage.reset(nice_storage_);
    context_.args_tracker.reset(new ArgsTracker(&context_));
    event_ = new MockEventTracker(&context_);
    context_.event_tracker.reset(event_);
    process_ = new MockProcessTracker(&context_);
    context_.process_tracker.reset(process_);
    slice_ = new MockSliceTracker(&context_);
    context_.slice_tracker.reset(slice_);
    context_.sorter.reset(new TraceSorter(&context_, 0 /*window size*/));
    context_.parser.reset(new ProtoTraceParser(&context_));
  }

  void ResetTraceBuffers() {
    heap_buf_.reset(new protozero::ScatteredHeapBuffer());
    stream_writer_.reset(new protozero::ScatteredStreamWriter(heap_buf_.get()));
    heap_buf_->set_writer(stream_writer_.get());
    trace_.Reset(stream_writer_.get());
  }

  void SetUp() override { ResetTraceBuffers(); }

  void InitStorage() {
    storage_ = new MockTraceStorage();
    context_.storage.reset(storage_);
  }

  void Tokenize() {
    trace_.Finalize();
    std::vector<uint8_t> trace_bytes = heap_buf_->StitchSlices();
    std::unique_ptr<uint8_t[]> raw_trace(new uint8_t[trace_bytes.size()]);
    memcpy(raw_trace.get(), trace_bytes.data(), trace_bytes.size());
    context_.chunk_reader.reset(new ProtoTraceTokenizer(&context_));
    context_.chunk_reader->Parse(std::move(raw_trace), trace_bytes.size());

    ResetTraceBuffers();
  }

 protected:
  std::unique_ptr<protozero::ScatteredHeapBuffer> heap_buf_;
  std::unique_ptr<protozero::ScatteredStreamWriter> stream_writer_;
  protos::pbzero::Trace trace_;
  TraceProcessorContext context_;
  MockEventTracker* event_;
  MockProcessTracker* process_;
  MockSliceTracker* slice_;
  NiceMock<MockTraceStorage>* nice_storage_;
  MockTraceStorage* storage_;
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
  InitStorage();

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
  ASSERT_EQ(args.arg_values()[1].string_value, 0u);
  ASSERT_EQ(args.arg_values()[2].int_value, 12);
  ASSERT_EQ(args.arg_values()[3].int_value, 15);
  ASSERT_EQ(args.arg_values()[4].int_value, 20);
  ASSERT_EQ(args.arg_values()[5].string_value, 0u);

  // TODO(taylori): Add test ftrace event with all field types
  // and test here.
}

TEST_F(ProtoTraceParserTest, LoadGenericFtrace) {
  InitStorage();
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

  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts), value * 1024, 0, 0,
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

  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts), value, 0, 0,
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
              PushCounter(1000, 2000, 0, 10, RefType::kRefCpuId, false));
  Tokenize();
}

TEST_F(ProtoTraceParserTest, LoadProcessPacket) {
  auto* tree = trace_.add_packet()->set_process_tree();
  auto* process = tree->add_processes();
  static const char kProcName1[] = "proc1";

  process->add_cmdline(kProcName1);
  process->set_pid(1);
  process->set_ppid(2);

  EXPECT_CALL(*process_,
              UpdateProcess(1, Eq(2u), base::StringView(kProcName1)));
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
  process->set_ppid(2);

  EXPECT_CALL(*process_,
              UpdateProcess(1, Eq(2u), base::StringView(kProcName1)));
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

TEST_F(ProtoTraceParserTest, TrackEventWithoutInternedData) {
  InitStorage();
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

  InSequence in_sequence;  // Below slices should be sorted by timestamp.
  EXPECT_CALL(*slice_, Scoped(1005000, 1, 0, 0, 23000, _));
  EXPECT_CALL(*slice_, Begin(1010000, 1, 0, 0, _));
  EXPECT_CALL(*slice_, End(1020000, 1, 0, 0, _));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithInternedData) {
  InitStorage();
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
      .Times(4)
      .WillRepeatedly(Return(1));

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*storage_, InternString(base::StringView("cat2,cat3")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev2")))
      .WillOnce(Return(2));
  EXPECT_CALL(*slice_, Scoped(1005000, 1, 1, 2, 23000, _));

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(3));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev1")))
      .WillOnce(Return(4));
  EXPECT_CALL(*slice_, Begin(1010000, 1, 3, 4, _));

  EXPECT_CALL(*slice_, End(1020000, 1, 3, 4, _));

  EXPECT_CALL(*slice_, Scoped(1040000, 1, 3, 4, 0, _));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithoutIncrementalStateReset) {
  InitStorage();
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

  EXPECT_CALL(*slice_, Begin(_, _, _, _, _)).Times(0);
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithoutThreadDescriptor) {
  InitStorage();
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

  EXPECT_CALL(*slice_, Begin(_, _, _, _, _)).Times(0);
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithDataLoss) {
  InitStorage();
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
  EXPECT_CALL(*slice_, Begin(1010000, 1, 0, 0, _));
  EXPECT_CALL(*slice_, End(2010000, 1, 0, 0, _));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventMultipleSequences) {
  InitStorage();
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

  EXPECT_CALL(*slice_, Begin(1005000, 2, 1, 2, _));

  EXPECT_CALL(*storage_, InternString(base::StringView("cat1")))
      .WillOnce(Return(1));
  EXPECT_CALL(*storage_, InternString(base::StringView("ev1")))
      .WillOnce(Return(3));

  EXPECT_CALL(*slice_, Begin(1010000, 1, 1, 3, _));
  EXPECT_CALL(*slice_, End(1015000, 2, 1, 2, _));
  EXPECT_CALL(*slice_, End(1020000, 1, 1, 3, _));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithDebugAnnotations) {
  InitStorage();
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
  EXPECT_CALL(*slice_, Begin(1010000, 1, 1, 2, _))
      .WillOnce(testing::InvokeArgument<4>(&args, 1u));
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

  EXPECT_CALL(*slice_, End(1020000, 1, 1, 2, _))
      .WillOnce(testing::InvokeArgument<4>(&args, 1u));

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
  EXPECT_CALL(args, AddArg(1u, 17, 17, Variadic::String(18)));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventWithTaskExecution) {
  InitStorage();
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
  EXPECT_CALL(*slice_, Begin(1010000, 1, 1, 2, _))
      .WillOnce(testing::InvokeArgument<4>(&args, 1u));
  EXPECT_CALL(*storage_, InternString(base::StringView("file1")))
      .WillOnce(Return(3));
  EXPECT_CALL(*storage_, InternString(base::StringView("func1")))
      .WillOnce(Return(4));
  EXPECT_CALL(args, AddArg(1u, _, _, Variadic::String(3)));
  EXPECT_CALL(args, AddArg(1u, _, _, Variadic::String(4)));

  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, LoadChromeBenchmarkMetadata) {
  static const char kName[] = "name";
  static const char kTag2[] = "tag1";
  static const char kTag1[] = "tag2";

  InitStorage();
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
  EXPECT_CALL(*storage_,
              SetMetadata(metadata::benchmark_name, Variadic::String(1)));
  EXPECT_CALL(*storage_, AppendMetadata(metadata::benchmark_story_tags,
                                        Variadic::String(2)));
  EXPECT_CALL(*storage_, AppendMetadata(metadata::benchmark_story_tags,
                                        Variadic::String(3)));

  context_.sorter->ExtractEventsForced();
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
