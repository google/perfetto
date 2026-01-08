/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "perfetto/base/status.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_sched_event_tracker.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_tokenizer.h"
#include "src/trace_processor/importers/proto/additional_modules.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::_;
using ::testing::Args;
using ::testing::AtLeast;
using ::testing::DoAll;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IgnoreResult;
using ::testing::InSequence;
using ::testing::InvokeArgument;
using ::testing::NiceMock;
using ::testing::Pointwise;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::UnorderedElementsAreArray;

class MockSchedEventTracker : public FtraceSchedEventTracker {
 public:
  explicit MockSchedEventTracker(TraceProcessorContext* context)
      : FtraceSchedEventTracker(context) {}

  MOCK_METHOD(void,
              PushSchedSwitch,
              (uint32_t cpu,
               int64_t timestamp,
               int64_t prev_pid,
               base::StringView prev_comm,
               int32_t prev_prio,
               int64_t prev_state,
               int64_t next_pid,
               base::StringView next_comm,
               int32_t next_prio),
              (override));
};

class MockProcessTracker : public ProcessTracker {
 public:
  explicit MockProcessTracker(TraceProcessorContext* context)
      : ProcessTracker(context) {}

  MOCK_METHOD(void,
              UpdateThreadName,
              (UniqueTid utid,
               StringId thread_name_id,
               ThreadNamePriority priority),
              (override));
  MOCK_METHOD(UniqueTid, UpdateThread, (int64_t tid, int64_t tgid), (override));

  MOCK_METHOD(UniquePid, GetOrCreateProcess, (int64_t pid), (override));
  MOCK_METHOD(void,
              SetProcessNameIfUnset,
              (UniquePid upid, StringId process_name_id),
              (override));
};
class MockBoundInserter : public ArgsTracker::BoundInserter {
 public:
  MockBoundInserter()
      : ArgsTracker::BoundInserter(&tracker_, nullptr, 0u, 0u),
        tracker_(nullptr) {
    ON_CALL(*this, AddArg(_, _, _, _)).WillByDefault(ReturnRef(*this));
  }

  MOCK_METHOD(ArgsTracker::BoundInserter&,
              AddArg,
              (StringId flat_key,
               StringId key,
               Variadic v,
               ArgsTracker::UpdatePolicy update_policy),
              (override));

 private:
  ArgsTracker tracker_;
};

class MockEventTracker : public EventTracker {
 public:
  explicit MockEventTracker(TraceProcessorContext* context)
      : EventTracker(context) {}
  ~MockEventTracker() override = default;

  MOCK_METHOD(void,
              PushSchedSwitch,
              (uint32_t cpu,
               int64_t timestamp,
               uint32_t prev_pid,
               base::StringView prev_comm,
               int32_t prev_prio,
               int64_t prev_state,
               uint32_t next_pid,
               base::StringView next_comm,
               int32_t next_prio));

  MOCK_METHOD(std::optional<CounterId>,
              PushCounter,
              (int64_t timestamp, double value, TrackId track_id),
              (override));
};

class FuchsiaTraceParserTest : public ::testing::Test {
 public:
  FuchsiaTraceParserTest() {
    context_.storage = std::make_unique<TraceStorage>();
    storage_ = context_.storage.get();
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.import_logs_tracker =
        std::make_unique<ImportLogsTracker>(&context_, 1);
    context_.stack_profile_tracker.reset(new StackProfileTracker(&context_));
    context_.args_translation_table.reset(new ArgsTranslationTable(storage_));
    context_.metadata_tracker =
        std::make_unique<MetadataTracker>(context_.storage.get());
    context_.machine_tracker = std::make_unique<MachineTracker>(&context_, 0);
    context_.cpu_tracker = std::make_unique<CpuTracker>(&context_);
    event_ = new MockEventTracker(&context_);
    context_.event_tracker.reset(event_);
    sched_ = new MockSchedEventTracker(&context_);
    context_.ftrace_sched_tracker.reset(sched_);
    process_ = new NiceMock<MockProcessTracker>(&context_);
    context_.process_tracker.reset(process_);
    context_.process_track_translation_table.reset(
        new ProcessTrackTranslationTable(storage_));
    context_.slice_tracker = std::make_unique<SliceTracker>(&context_);
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(storage_);
    context_.clock_tracker = std::make_unique<ClockTracker>(
        std::make_unique<ClockSynchronizerListenerImpl>(&context_));
    clock_ = context_.clock_tracker.get();
    context_.flow_tracker = std::make_unique<FlowTracker>(&context_);
    context_.sorter = std::make_unique<TraceSorter>(
        &context_, TraceSorter::SortingMode::kFullSort);
    context_.descriptor_pool_ = std::make_unique<DescriptorPool>();
    context_.register_additional_proto_modules = &RegisterAdditionalModules;
    tokenizer_ = std::make_unique<FuchsiaTraceTokenizer>(&context_);
  }

  void push_word(uint64_t word) { trace_bytes_.push_back(word); }

  void ResetTraceBuffers() {
    trace_bytes_.clear();
    // Write the FXT Magic Bytes
    push_word(0x0016547846040010);
  }

  void SetUp() override { ResetTraceBuffers(); }

  base::Status Tokenize() {
    const size_t num_bytes = trace_bytes_.size() * sizeof(uint64_t);
    std::unique_ptr<uint8_t[]> raw_trace(new uint8_t[num_bytes]);
    memcpy(raw_trace.get(), trace_bytes_.data(), num_bytes);

    auto status = tokenizer_->Parse(TraceBlobView(
        TraceBlob::TakeOwnership(std::move(raw_trace), num_bytes)));

    ResetTraceBuffers();
    return status;
  }

 protected:
  std::vector<uint64_t> trace_bytes_;

  TraceProcessorContext context_;
  MockEventTracker* event_;
  MockSchedEventTracker* sched_;
  MockProcessTracker* process_;
  ClockTracker* clock_;
  TraceStorage* storage_;
  std::unique_ptr<FuchsiaTraceTokenizer> tokenizer_;
};

TEST_F(FuchsiaTraceParserTest, CorruptedFxt) {
  // Invalid record of size 0
  push_word(0x0016547846040000);
  EXPECT_FALSE(Tokenize().ok());
}

TEST_F(FuchsiaTraceParserTest, InlineInstantEvent) {
  // Inline name of 8 bytes
  uint64_t name_ref = uint64_t{0x8008} << 48;
  // Inline category of 8 bytes
  uint64_t category_ref = uint64_t{0x8008} << 32;
  // Inline threadref
  uint64_t threadref = uint64_t{0};
  // Instant Event
  uint64_t event_type = 0 << 16;
  uint64_t size = 6 << 4;
  uint64_t record_type = 4;

  auto header =
      name_ref | category_ref | threadref | event_type | size | record_type;
  push_word(header);
  // Timestamp
  push_word(0x5555555555555555);
  // Pid + tid
  push_word(0xBBBBBBBBBBBBBBBB);
  push_word(0xCCCCCCCCCCCCCCCC);
  // Inline Category
  push_word(0xDDDDDDDDDDDDDDDD);
  // Inline Name
  push_word(0xEEEEEEEEEEEEEEEE);
  EXPECT_TRUE(Tokenize().ok());

  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_non_numeric_counters].value, 0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_timestamp_overflow].value,
            0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_record_read_error].value,
            0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_event].value, 0);
  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_invalid_event_arg_type].value,
      0);
  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_invalid_event_arg_name].value,
      0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_string_ref].value,
            0);
}

TEST_F(FuchsiaTraceParserTest, BooleanArguments) {
  // Inline name of 8 bytes
  uint64_t name_ref = uint64_t{0x8008} << 48;
  // Inline category of 8 bytes
  uint64_t category_ref = uint64_t{0x8008} << 32;
  // Inline threadref
  uint64_t threadref = uint64_t{0};
  // 2 arguments
  uint64_t argument_count = uint64_t{2} << 20;
  // Instant Event
  uint64_t event_type = 0 << 16;
  uint64_t size = 10 << 4;
  uint64_t record_type = 4;

  auto header = name_ref | category_ref | threadref | event_type |
                argument_count | size | record_type;
  push_word(header);
  // Timestamp
  push_word(0x5555555555555555);
  // Pid + tid
  push_word(0xBBBBBBBBBBBBBBBB);
  push_word(0xCCCCCCCCCCCCCCCC);
  // Inline Category
  push_word(0xDDDDDDDDDDDDDDDD);
  // Inline Name
  push_word(0xEEEEEEEEEEEEEEEE);
  // Boolean argument true
  push_word(0x0000'0001'8008'0029);
  // 8 byte arg name stream
  push_word(0x0000'0000'0000'0000);
  // Boolean argument false
  push_word(0x0000'0000'8008'002A);
  // 8 byte arg name stream
  push_word(0x0000'0000'0000'0000);
  EXPECT_TRUE(Tokenize().ok());

  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_non_numeric_counters].value, 0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_timestamp_overflow].value,
            0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_record_read_error].value,
            0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_event].value, 0);
  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_invalid_event_arg_type].value,
      0);
  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_invalid_event_arg_name].value,
      0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_string_ref].value,
            0);
}

TEST_F(FuchsiaTraceParserTest, FxtWithProtos) {
  // Serialize some protos to bytes
  protozero::HeapBuffered<protos::pbzero::Trace> protos;
  {
    auto* packet = protos->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
  }
  {
    auto* packet = protos->add_packet();
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
    auto* packet = protos->add_packet();
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
    auto* packet = protos->add_packet();
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

  protos->Finalize();
  std::vector<uint8_t> perfetto_bytes = protos.SerializeAsArray();

  // Set up an FXT Perfetto Blob Header
  uint64_t blob_type_perfetto = uint64_t{3} << 48;
  uint64_t unpadded_blob_size_bytes = uint64_t{perfetto_bytes.size()} << 32;
  uint64_t blob_name_ref = uint64_t{0x8008} << 16;
  uint64_t size_words = ((perfetto_bytes.size() + 7) / 8 + 2) << 4;
  uint64_t record_type = 5;

  uint64_t header = blob_type_perfetto | unpadded_blob_size_bytes |
                    blob_name_ref | size_words | record_type;

  // Pad the blob to a multiple of 8 bytes.
  while (perfetto_bytes.size() % 8) {
    perfetto_bytes.push_back(0);
  }

  push_word(header);
  // Inline Name Ref
  push_word(0xBBBBBBBBBBBBBBBB);
  trace_bytes_.insert(trace_bytes_.end(),
                      reinterpret_cast<uint64_t*>(perfetto_bytes.data()),
                      reinterpret_cast<uint64_t*>(perfetto_bytes.data() +
                                                  perfetto_bytes.size()));
  EXPECT_CALL(*process_, UpdateThread(16, 15)).WillRepeatedly(Return(1u));

  tables::ThreadTable::Row row(16);
  row.upid = 1u;
  storage_->mutable_thread_table()->Insert(row);

  MockBoundInserter inserter;

  StringId unknown_cat = storage_->InternString("unknown(1)");
  ASSERT_NE(storage_, nullptr);

  constexpr TrackId track{1u};
  constexpr TrackId thread_time_track{0u};

  InSequence in_sequence;  // Below slices should be sorted by timestamp.
  // Only the begin thread time can be imported into the counter table.
  EXPECT_CALL(*event_, PushCounter(1005000, testing::DoubleEq(2003000),
                                   thread_time_track));
  EXPECT_CALL(*event_, PushCounter(1010000, testing::DoubleEq(2005000),
                                   thread_time_track));
  EXPECT_CALL(*event_, PushCounter(1020000, testing::DoubleEq(2010000),
                                   thread_time_track));

  auto status = Tokenize();
  EXPECT_TRUE(status.ok());
  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(storage_->slice_table().row_count(), 2u);
  auto rr_0 = storage_->slice_table().FindById(SliceId(0u));
  EXPECT_TRUE(rr_0);
  EXPECT_EQ(rr_0->ts(), 1005000);
  EXPECT_EQ(rr_0->track_id(), track);

  auto rr_1 = storage_->slice_table().FindById(SliceId(1u));
  EXPECT_TRUE(rr_1);
  EXPECT_EQ(rr_1->ts(), 1010000);
  EXPECT_EQ(rr_1->track_id(), track);
  EXPECT_EQ(rr_1->dur(), 10000);
  EXPECT_EQ(rr_1->category(), unknown_cat);
}

TEST_F(FuchsiaTraceParserTest, SchedulerEvents) {
  uint64_t thread1_tid = 0x1AAA'AAAA'AAAA'AAAA;
  uint64_t thread2_tid = 0x2CCC'CCCC'CCCC'CCCC;

  // We'll emit a wake up for thread 1, a switch to thread 2, and a switch back
  // to thread 1 and expect to see that the process tracker was properly updated

  uint64_t wakeup_record_type = uint64_t{2} << 60;
  uint64_t context_switch_record_type = uint64_t{1} << 60;
  uint64_t cpu = 1 << 20;
  uint64_t record_type = 8;

  uint64_t wakeup_size = uint64_t{3} << 4;
  uint64_t context_switch_size = uint64_t{4} << 4;

  uint64_t wakeup_header = wakeup_record_type | cpu | record_type | wakeup_size;
  push_word(wakeup_header);
  // Timestamp
  push_word(0x1);
  // wakeup tid
  push_word(thread1_tid);

  uint64_t context_switch_header =
      context_switch_record_type | cpu | record_type | context_switch_size;
  push_word(context_switch_header);
  // Timestamp
  push_word(0x2);
  // outgoing tid
  push_word(thread1_tid);
  // incoming tid
  push_word(thread2_tid);

  push_word(context_switch_header);
  // Timestamp
  push_word(0x3);
  // outgoing tid
  push_word(thread2_tid);
  // incoming tid
  push_word(thread1_tid);

  // We should get:
  // - A thread1 update call on wake up
  // - thread1 & thread2 update calls on the first context switch
  // - thread2 & thread1 update cals on the second context switch
  EXPECT_CALL(*process_, UpdateThread(static_cast<uint32_t>(thread1_tid), _))
      .Times(3);
  EXPECT_CALL(*process_, UpdateThread(static_cast<uint32_t>(thread2_tid), _))
      .Times(2);

  EXPECT_TRUE(Tokenize().ok());

  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_non_numeric_counters].value, 0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_timestamp_overflow].value,
            0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_record_read_error].value,
            0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_event].value, 0);
  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_invalid_event_arg_type].value,
      0);
  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_invalid_event_arg_name].value,
      0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_string_ref].value,
            0);

  context_.sorter->ExtractEventsForced();
}

TEST_F(FuchsiaTraceParserTest, LegacySchedulerEvents) {
  uint64_t thread1_pid = 0x1AAA'AAAA'AAAA'AAAA;
  uint64_t thread1_tid = 0x1BBB'BBBB'BBBB'BBBB;
  uint64_t thread2_pid = 0x2CCC'CCCC'CCCC'CCCC;
  uint64_t thread2_tid = 0x2DDD'DDDD'DDDD'DDDD;

  // We'll emit a wake up for thread 1, a switch to thread 2, and a switch back
  // to thread 1 and expect to see that the process tracker was properly updated

  uint64_t context_switch_size = uint64_t{6} << 4;
  uint64_t cpu = 1 << 16;
  uint64_t record_type = 8;
  uint64_t outoing_state = 2 << 24;
  uint64_t outoing_thread = 0;   // Inline thread-ref
  uint64_t incoming_thread = 0;  // Inline thread-ref
  uint64_t outgoing_prio = uint64_t{1} << 44;
  uint64_t incoming_prio = uint64_t{1} << 52;
  uint64_t outgoing_idle_prio = uint64_t{0} << 44;

  uint64_t context_switch_header =
      record_type | context_switch_size | cpu | outoing_state | outoing_thread |
      incoming_thread | outgoing_prio | incoming_prio;
  uint64_t wakeup_header = record_type | context_switch_size | cpu |
                           outoing_state | outoing_thread | incoming_thread |
                           outgoing_idle_prio | incoming_prio;

  push_word(wakeup_header);
  // Timestamp
  push_word(0x1);
  // outgoing pid+tid
  push_word(0);  // Idle thread
  push_word(0);  // Idle thread
  // incoming pid+tid
  push_word(thread1_pid);
  push_word(thread1_tid);

  push_word(context_switch_header);
  // Timestamp
  push_word(0x2);
  // outgoing pid+tid
  push_word(thread1_pid);
  push_word(thread1_tid);
  // incoming pid+tid
  push_word(thread2_pid);
  push_word(thread2_tid);

  push_word(context_switch_header);
  // Timestamp
  push_word(0x3);
  // outgoing pid+tid
  push_word(thread2_pid);
  push_word(thread2_tid);
  // incoming pid+tid
  push_word(thread1_pid);
  push_word(thread1_tid);

  // We should get:
  // - A thread1 update call on wake up
  // - thread1 & thread2 update calls on the first context switch
  // - thread2 & thread1 update cals on the second context switch
  EXPECT_CALL(*process_, UpdateThread(static_cast<uint32_t>(thread1_tid), _))
      .Times(3);
  EXPECT_CALL(*process_, UpdateThread(static_cast<uint32_t>(thread2_tid), _))
      .Times(2);

  EXPECT_TRUE(Tokenize().ok());

  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_non_numeric_counters].value, 0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_timestamp_overflow].value,
            0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_record_read_error].value,
            0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_event].value, 0);
  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_invalid_event_arg_type].value,
      0);
  EXPECT_EQ(
      context_.storage->stats()[stats::fuchsia_invalid_event_arg_name].value,
      0);
  EXPECT_EQ(context_.storage->stats()[stats::fuchsia_invalid_string_ref].value,
            0);

  context_.sorter->ExtractEventsForced();
}

}  // namespace
}  // namespace perfetto::trace_processor
