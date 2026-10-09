/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "perfetto/trace_processor/trace_parser.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/thread_state_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/counter_tables_py.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/trace_parser_impl.h"
#include "src/trace_processor/trace_processor_storage_impl.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class ReplaySink : public dataframe::Dataframe::Sink {
 public:
  using Values = std::vector<dataframe::Dataframe::SinkValue>;
  void OnInsert(uint32_t row,
                const dataframe::Dataframe::SinkValue* values,
                uint32_t count) override {
    EXPECT_EQ(row, rows.size());
    rows.emplace_back(values, values + count);
  }
  void OnUpdate(uint32_t row,
                uint32_t col,
                const dataframe::Dataframe::SinkValue& value) override {
    ASSERT_GE(row, frontier);
    if (row < storage_frontier)
      ++patches_after_eviction;
    ASSERT_LT(row, rows.size());
    ASSERT_LT(col, rows[row].size());
    rows[row][col] = value;
    ++updates;
  }
  void OnStorageFrontierAdvance(uint32_t row) override {
    EXPECT_GE(row, storage_frontier);
    storage_frontier = row;
  }
  void OnFrontierAdvance(uint32_t row) override {
    EXPECT_GE(row, frontier);
    EXPECT_LE(row, rows.size());
    frontier = row;
  }
  void OnClear() override {
    rows.clear();
    frontier = 0;
    storage_frontier = 0;
  }
  uint32_t frontier = 0;
  uint32_t storage_frontier = 0;
  uint32_t patches_after_eviction = 0;
  std::vector<Values> rows;
  uint32_t updates = 0;
};

class GeneratedCounterSink : public tables::CounterTable::Sink {
 public:
  void OnRowInsert(tables::CounterTable::Id id,
                   const tables::CounterTable::Row& row) override {
    ids.push_back(id.value);
    values.push_back(row.value);
  }
  void OnCellUpdate(tables::CounterTable::Id,
                    uint32_t,
                    const dataframe::Dataframe::SinkValue&) override {
    ++updates;
  }
  std::vector<uint32_t> ids;
  std::vector<double> values;
  uint32_t updates = 0;
};

using Sinks = std::map<std::string, std::unique_ptr<ReplaySink>>;

std::unique_ptr<TraceParserImpl> ParseIntoSinks(const std::string& input,
                                                bool drop,
                                                Sinks& sinks,
                                                bool frontier = false,
                                                bool slice_args = false) {
  TraceParserOptions config;
  config.drop_unread_table_columns = drop;
  config.experimental_ftrace_sched_frontier = frontier;
  config.experimental_slice_args_streaming = slice_args;
  config.parsing_config.drop_ftrace_data_before = DropFtraceDataBefore::kNoDrop;
  config.parsing_config.soft_drop_ftrace_data_before =
      SoftDropFtraceDataBefore::kNoDrop;
  config.on_trace_storage_created = [&, drop](TraceStorage* storage) {
    storage->SetTableSinks(
        [&](const char* name) {
          auto& sink = sinks[name];
          sink = std::make_unique<ReplaySink>();
          return sink.get();
        },
        drop);
  };
  auto tp = std::make_unique<TraceParserImpl>(config);
  auto status =
      tp->Parse(TraceBlobView(TraceBlob::CopyFrom(input.data(), input.size())));
  EXPECT_TRUE(status.ok()) << status.message();
  status = tp->NotifyEndOfFile();
  EXPECT_TRUE(status.ok()) << status.message();
  return tp;
}

void ExpectSameSinkRecords(const Sinks& baseline, const Sinks& streaming) {
  ASSERT_EQ(baseline.size(), streaming.size());
  for (const auto& entry : baseline) {
    if (entry.first == "__intrinsic_stats") {
      continue;  // Execution duration stats vary between parser instances.
    }
    ASSERT_TRUE(streaming.count(entry.first));
    EXPECT_EQ(entry.second->rows, streaming.at(entry.first)->rows)
        << entry.first;
  }
}

struct PublicOutput {
  std::map<std::string, uint32_t> rows;
  std::map<std::string, uint32_t> storage_frontiers;
  std::map<std::string, uint32_t> completion_frontiers;
  std::set<std::string> strings;
  uint32_t late_patches = 0;
  bool ended = false;
  bool fail = false;
  base::Status Insert(const char* table, uint32_t row) {
    if (fail)
      return base::ErrStatus("output failure");
    EXPECT_EQ(row, rows[table]++);
    return base::OkStatus();
  }
  base::Status Update(const char* table, uint32_t row) {
    if (fail)
      return base::ErrStatus("output failure");
    if (row < storage_frontiers[table])
      ++late_patches;
    return base::OkStatus();
  }
  struct Slice : SliceSink {
    explicit Slice(PublicOutput* owner) : owner(owner) {}
    PublicOutput* owner;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      if (row.name)
        owner->strings.emplace(row.name->value);
      return owner->Insert(tables::SliceTable::Name(), id);
    }
    base::Status OnDurUpdate(uint32_t id, int64_t) override {
      return owner->Update(tables::SliceTable::Name(), id);
    }
    base::Status OnStorageFrontierAdvance(uint32_t row) override {
      owner->storage_frontiers[tables::SliceTable::Name()] = row;
      return base::OkStatus();
    }
    base::Status OnCompletionFrontierAdvance(uint32_t row) override {
      owner->completion_frontiers[tables::SliceTable::Name()] = row;
      return base::OkStatus();
    }
    base::Status OnEndOfTrace() override {
      owner->ended = true;
      return base::OkStatus();
    }
  } slice{this};
  struct Arg : ArgSink {
    explicit Arg(PublicOutput* owner) : owner(owner) {}
    PublicOutput* owner;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      (void)row;
      return owner->Insert(tables::ArgTable::Name(), id);
    }
    base::Status OnStorageFrontierAdvance(uint32_t row) override {
      owner->storage_frontiers[tables::ArgTable::Name()] = row;
      return base::OkStatus();
    }
    base::Status OnCompletionFrontierAdvance(uint32_t row) override {
      owner->completion_frontiers[tables::ArgTable::Name()] = row;
      return base::OkStatus();
    }
    base::Status OnEndOfTrace() override {
      owner->ended = true;
      return base::OkStatus();
    }
  } args{this};
  struct Sched : SchedSink {
    explicit Sched(PublicOutput* owner) : owner(owner) {}
    PublicOutput* owner;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      (void)row;
      return owner->Insert(tables::SchedSliceTable::Name(), id);
    }
    base::Status OnDurUpdate(uint32_t id, int64_t) override {
      return owner->Update(tables::SchedSliceTable::Name(), id);
    }
    base::Status OnStorageFrontierAdvance(uint32_t row) override {
      owner->storage_frontiers[tables::SchedSliceTable::Name()] = row;
      return base::OkStatus();
    }
    base::Status OnCompletionFrontierAdvance(uint32_t row) override {
      owner->completion_frontiers[tables::SchedSliceTable::Name()] = row;
      return base::OkStatus();
    }
    base::Status OnEndOfTrace() override {
      owner->ended = true;
      return base::OkStatus();
    }
  } sched{this};
  struct ThreadState : ThreadStateSink {
    explicit ThreadState(PublicOutput* owner) : owner(owner) {}
    PublicOutput* owner;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      (void)row;
      return owner->Insert(tables::ThreadStateTable::Name(), id);
    }
    base::Status OnDurUpdate(uint32_t id, int64_t) override {
      return owner->Update(tables::ThreadStateTable::Name(), id);
    }
    base::Status OnStorageFrontierAdvance(uint32_t row) override {
      owner->storage_frontiers[tables::ThreadStateTable::Name()] = row;
      return base::OkStatus();
    }
    base::Status OnCompletionFrontierAdvance(uint32_t row) override {
      owner->completion_frontiers[tables::ThreadStateTable::Name()] = row;
      return base::OkStatus();
    }
    base::Status OnEndOfTrace() override {
      owner->ended = true;
      return base::OkStatus();
    }
  } thread_states{this};
  struct Counter : CounterSink {
    explicit Counter(PublicOutput* owner) : owner(owner) {}
    PublicOutput* owner;
    uint32_t track_updates = 0;
    double patched_value = 0;
    double inserted_value = 0;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      inserted_value = row.value;
      return owner->Insert(tables::CounterTable::Name(), id);
    }
    base::Status OnTrackIdUpdate(uint32_t id, uint32_t) override {
      ++track_updates;
      return owner->Update(tables::CounterTable::Name(), id);
    }
    base::Status OnValueUpdate(uint32_t id, double value) override {
      patched_value = value;
      return owner->Update(tables::CounterTable::Name(), id);
    }
    base::Status OnStorageFrontierAdvance(uint32_t row) override {
      owner->storage_frontiers[tables::CounterTable::Name()] = row;
      return base::OkStatus();
    }
  } counters{this};
  TraceParser::Sinks sinks() {
    return {&slice, &args, &sched, &thread_states, &counters};
  }
};

TEST(StreamingTablesTest, GeneratedTypedSinkAndStorageMask) {
  TraceStorage storage;
  GeneratedCounterSink sink;
  auto* counters = storage.mutable_counter_table();
  counters->SetSink(&sink, true);
  auto first = counters->Insert({10, TrackId{0}, 2.5, {}});
  auto second = counters->Insert({20, TrackId{0}, 4.5, {}});
  EXPECT_EQ(first.id.value, 0u);
  EXPECT_EQ(second.id.value, 1u);
  EXPECT_EQ(sink.values, (std::vector<double>{2.5, 4.5}));
  counters->dataframe()
      .SetCellUncheckedLegacy<dataframe::Uint32,
                              dataframe::SparseNullWithPopcountAlways>(
          tables::CounterTable::ColumnIndex::arg_set_id, 0,
          std::make_optional(uint32_t{42}));
  EXPECT_EQ(sink.updates, 1u);
}

TEST(StreamingTablesTest, JsonParserProducesSameSinkRecordsWithEmptyColumns) {
  const std::string json = R"({"traceEvents":[
    {"ph":"M","pid":10,"tid":11,"name":"thread_name","args":{"name":"worker"}},
    {"ph":"B","pid":10,"tid":11,"ts":1,"name":"outer","args":{"begin":"a"}},
    {"ph":"X","pid":10,"tid":11,"ts":2,"dur":1,"name":"inner","args":{"n":7}},
    {"ph":"E","pid":10,"tid":11,"ts":4,"args":{"end":"b"}},
    {"ph":"C","pid":10,"tid":11,"ts":5,"name":"memory","args":{"bytes":123}},
    {"ph":"i","pid":10,"tid":11,"ts":6,"name":"instant","s":"t"}
  ]})";
  Sinks baseline, streaming;
  auto normal_tp = ParseIntoSinks(json, false, baseline);
  auto streamed_tp = ParseIntoSinks(json, true, streaming);
  ExpectSameSinkRecords(baseline, streaming);
  EXPECT_TRUE(streaming.count(tables::CounterTable::Name()));
  EXPECT_TRUE(streaming.count(tables::ArgTable::Name()));
  auto* slices = streamed_tp->context()->storage->mutable_slice_table();
  EXPECT_EQ(slices->row_count(), 3u);
  EXPECT_GT(streaming[tables::SliceTable::Name()]->updates, 0u);
  EXPECT_EQ(streamed_tp->context()
                ->storage->metadata_table()
                .dataframe()
                .column(tables::MetadataTable::ColumnIndex::key_type)
                .storage.unchecked_get<dataframe::String>()
                .size(),
            0u);
}

TEST(StreamingTablesTest, ProtoTrackEventsAndSchedulingProduceSameSinkRecords) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;
  auto* packet = trace->add_packet();
  packet->set_timestamp(1);
  packet->set_trusted_packet_sequence_id(1);
  auto* descriptor = packet->set_track_descriptor();
  descriptor->set_uuid(123);
  descriptor->set_name("streaming test track");
  for (uint32_t i = 0; i < 2; ++i) {
    packet = trace->add_packet();
    packet->set_timestamp(10 + i * 10);
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_track_uuid(123);
    event->set_type(i == 0 ? protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN
                           : protos::pbzero::TrackEvent::TYPE_SLICE_END);
    if (i == 0) {
      event->set_name("work");
    }
  }
  packet = trace->add_packet();
  auto* bundle = packet->set_ftrace_events();
  bundle->set_cpu(0);
  for (uint32_t i = 0; i < 3; ++i) {
    auto* event = bundle->add_event();
    event->set_timestamp(100 + i * 100);
    event->set_pid(i);
    auto* sched = event->set_sched_switch();
    sched->set_prev_pid(static_cast<int32_t>(i));
    sched->set_prev_comm("previous");
    sched->set_prev_prio(120);
    sched->set_prev_state(0);
    sched->set_next_pid(static_cast<int32_t>(i + 1));
    sched->set_next_comm("next");
    sched->set_next_prio(120);
  }
  std::string input = trace.SerializeAsString();
  Sinks baseline, streaming;
  auto normal_tp = ParseIntoSinks(input, false, baseline);
  auto streamed_tp = ParseIntoSinks(input, true, streaming);
  ExpectSameSinkRecords(baseline, streaming);
  auto& sched = streamed_tp->context()->storage->sched_slice_table();
  EXPECT_GT(sched.row_count(), 0u);
  EXPECT_FALSE(sched.dataframe().retains_column(
      tables::SchedSliceTable::ColumnIndex::priority));
  EXPECT_FALSE(sched.dataframe().retains_column(
      tables::SchedSliceTable::ColumnIndex::ucpu));
  EXPECT_EQ(sched.dataframe()
                .column(tables::SchedSliceTable::ColumnIndex::priority)
                .storage.unchecked_get<dataframe::Int32>()
                .size(),
            0u);
}

TEST(StreamingTablesTest, FtraceFrontierAdvancesAndKeepsSinkRecords) {
  // Four CPUs move together. CPU 4 deliberately stalls in the pinned variant.
  for (bool stalled : {false, true}) {
    protozero::HeapBuffered<protos::pbzero::Trace> input_trace;
    for (uint32_t i = 0; i < 12000; ++i) {
      auto* bundle = input_trace->add_packet()->set_ftrace_events();
      uint32_t cpu = stalled && (i == 0 || i == 11999) ? 4 : i % 4;
      bundle->set_cpu(cpu);
      auto* event = bundle->add_event();
      event->set_timestamp(100 + i * 100);
      event->set_pid(i < 4 ? 0u : 1 + cpu);
      auto* sched = event->set_sched_switch();
      sched->set_prev_pid(static_cast<int32_t>(i < 4 ? 0 : 1 + cpu));
      sched->set_prev_comm("task");
      sched->set_prev_prio(120);
      sched->set_prev_state(0);
      sched->set_next_pid(static_cast<int32_t>(1 + cpu));
      sched->set_next_comm("task");
      sched->set_next_prio(120);
    }
    Sinks baseline, streaming;
    std::string input = input_trace.SerializeAsString();
    auto normal = ParseIntoSinks(input, true, baseline);
    auto windowed = ParseIntoSinks(input, true, streaming, true);
    ExpectSameSinkRecords(baseline, streaming);
    PublicOutput public_output;
    TraceParser::Config public_config;
    public_config.drop_ftrace_data_before = DropFtraceDataBefore::kNoDrop;
    public_config.soft_drop_ftrace_data_before =
        SoftDropFtraceDataBefore::kNoDrop;
    auto public_parser =
        TraceParser::CreateInstance(public_config, public_output.sinks());
    ASSERT_TRUE(public_parser
                    ->Parse(TraceBlobView(
                        TraceBlob::CopyFrom(input.data(), input.size())))
                    .ok());
    ASSERT_TRUE(public_parser->NotifyEndOfFile().ok());
    EXPECT_EQ(public_output.rows[tables::SchedSliceTable::Name()], 12000u);
    EXPECT_GT(public_output.storage_frontiers[tables::SchedSliceTable::Name()],
              11000u);
    EXPECT_GT(public_output.late_patches, 0u);
    EXPECT_TRUE(public_output.ended);
    const auto& table = windowed->context()->storage->sched_slice_table();
    EXPECT_EQ(table.row_count(), 12000u);
    const auto& df = table.dataframe();
    const auto& sink = *streaming.at(tables::SchedSliceTable::Name());
    EXPECT_EQ(sink.frontier, df.completion_frontier());
    EXPECT_EQ(sink.storage_frontier, df.first_retained_row());
    EXPECT_GT(sink.patches_after_eviction, 0u);
    EXPECT_GT(df.first_retained_row(), 11000u);
    for (uint32_t column = 1; column < df.column_count(); ++column)
      EXPECT_FALSE(df.retains_column(column));
    if (stalled) {
      EXPECT_EQ(df.completion_frontier(), 0u);
      // CPU 4 resumes at the very end and patches its long-evicted row 0.
      EXPECT_EQ(std::get<int64_t>(
                    sink.rows[0][tables::SchedSliceTable::ColumnIndex::dur]),
                1199900);
      EXPECT_EQ(df.first_retained_row(), 11264u);
    } else {
      EXPECT_GT(df.completion_frontier(), 10000u);
      EXPECT_GT(df.first_retained_row(), df.completion_frontier());
    }
  }
}

TEST(StreamingTablesTest, ThreadStateHistoryStreamsLatePatchesAndWakerIds) {
  Sinks baseline, streaming;
  auto exercise = [](bool stream, Sinks& sinks) {
    TraceParserOptions config;
    config.drop_unread_table_columns = stream;
    config.experimental_ftrace_sched_frontier = stream;
    config.on_trace_storage_created = [&, stream](TraceStorage* storage) {
      storage->SetTableSinks(
          [&](const char* name) {
            auto& sink = sinks[name];
            sink = std::make_unique<ReplaySink>();
            return sink.get();
          },
          stream);
    };
    auto tp = std::make_unique<TraceProcessorStorageImpl>(config);
    auto* context = tp->context()->ForkContextForTrace(
        TraceId{0}, /*default_raw_machine_id=*/0);
    auto* tracker = ThreadStateTracker::GetOrCreate(context);
    auto* storage = context->storage.get();
    auto waker = context->process_tracker->GetOrCreateThread(11);
    auto sleeper = context->process_tracker->GetOrCreateThread(12);
    auto worker = context->process_tracker->GetOrCreateThread(13);
    auto running = storage->InternString("Running");
    auto runnable = storage->InternString("R");
    auto blocked = storage->InternString("D");
    tracker->PushThreadState(10, waker, running, 0);
    tracker->PushThreadState(10, sleeper, blocked);
    for (uint32_t i = 0; i < 6000; ++i)
      tracker->PushThreadState(20 + i, worker, i % 2 ? running : runnable, 1);
    EXPECT_EQ(tracker->GetPrevEndState(sleeper), blocked);
    // Both the sleeper's blocked row and waker's running row are long evicted.
    tracker->PushBlockedReason(sleeper, true,
                               storage->InternString("blocked1"));
    tracker->PushWakingEvent(10000, sleeper, waker, 8);
    // A runnable successor still permits late patches to the blocked row.
    tracker->PushBlockedReason(sleeper, false,
                               storage->InternString("blocked2"));
    tracker->PushWakingEvent(10001, sleeper, waker, 0);
    tracker->UpdatePendingState(sleeper, blocked, std::nullopt, waker, 0);
    EXPECT_EQ(tracker->GetPrevEndState(sleeper), blocked);
    tracker->PushWakingEvent(10002, sleeper, waker, 0);
    tracker->PushSchedSwitchEvent(10003, 0, sleeper, runnable, worker);
    EXPECT_TRUE(tp->NotifyEndOfFile().ok());
    return tp;
  };
  auto full = exercise(false, baseline);
  auto streamed = exercise(true, streaming);
  ExpectSameSinkRecords(baseline, streaming);
  const auto& table = streamed->context()->storage->thread_state_table();
  const auto& df = table.dataframe();
  EXPECT_GT(df.first_retained_row(), 5000u);
  EXPECT_EQ(df.completion_frontier(), 0u);
  for (uint32_t col = 1; col < df.column_count(); ++col)
    EXPECT_FALSE(df.retains_column(col));
  EXPECT_EQ(df.column(tables::ThreadStateTable::ColumnIndex::ts)
                .storage.unchecked_get<dataframe::Int64>()
                .size(),
            0u);
  const auto& sink = *streaming.at(tables::ThreadStateTable::Name());
  EXPECT_GT(sink.patches_after_eviction, 0u);
  EXPECT_EQ(
      std::get<uint32_t>(
          sink.rows[6002][tables::ThreadStateTable::ColumnIndex::waker_id]),
      0u);
  EXPECT_EQ(std::get<uint32_t>(
                sink.rows[1][tables::ThreadStateTable::ColumnIndex::io_wait]),
            0u);
}

TEST(StreamingTablesTest, JsonSlicesArgsAndFlowsSurviveLongOpenParentEviction) {
  std::string input = R"({"traceEvents":[
    {"ph":"B","ts":0,"tts":10,"pid":1,"tid":1,"name":"outer","args":{"root":1}})";
  for (uint32_t i = 0; i < 6000; ++i) {
    auto ts = std::to_string(4 * i + 1);
    input += R"(,{"ph":"B","pid":1,"tid":1,"name":"child","cat":"test","ts":)" +
             ts + R"(,"tts":)" + std::to_string(100 + 10 * i) +
             R"(,"args":{"index":)" + std::to_string(i) + "}}";
    if (i == 0 || i == 5999) {
      input +=
          R"(,{"ph":")" + std::string(i == 0 ? "s" : "f") +
          R"(","pid":1,"tid":1,"id":7,"cat":"flow","name":"link","bp":"e","ts":)" +
          std::to_string(4 * i + 2) + "}";
    }
    input += R"(,{"ph":"E","pid":1,"tid":1,"name":"child","cat":"test","ts":)" +
             std::to_string(4 * i + 3) + R"(,"tts":)" +
             std::to_string(107 + 10 * i) + "}";
  }
  input +=
      R"(,{"ph":"E","ts":25000,"tts":30000,"pid":1,"tid":1,"name":"outer","args":{"finished":true}}]})";
  Sinks baseline, streaming;
  auto full = ParseIntoSinks(input, true, baseline);
  auto streamed = ParseIntoSinks(input, true, streaming, false, true);
  ExpectSameSinkRecords(baseline, streaming);
  const auto& slice = streamed->context()->storage->slice_table();
  const auto& args = streamed->context()->storage->arg_table();
  EXPECT_EQ(slice.row_count(), 6001u);
  EXPECT_GT(slice.dataframe().first_retained_row(), 5000u);
  EXPECT_GT(args.dataframe().first_retained_row(), 5000u);
  EXPECT_GT(streaming.at(tables::SliceTable::Name())->patches_after_eviction,
            0u);
  EXPECT_GT(streaming.at(tables::FlowTable::Name())->rows.size(), 0u);
  for (const auto* df : {&slice.dataframe(), &args.dataframe()})
    for (uint32_t col = 1; col < df->column_count(); ++col)
      EXPECT_FALSE(df->retains_column(col));
}

TEST(StreamingTablesTest, ProtoSliceThreadCountersPatchEvictedParent) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;
  auto* packet = trace->add_packet();
  packet->set_timestamp(1);
  packet->set_trusted_packet_sequence_id(1);
  auto* descriptor = packet->set_track_descriptor();
  descriptor->set_uuid(123);
  auto* thread = descriptor->set_thread();
  thread->set_pid(1);
  thread->set_tid(2);
  auto emit = [&](uint64_t ts, bool begin, const char* name) {
    auto* p = trace->add_packet();
    p->set_timestamp(ts);
    p->set_trusted_packet_sequence_id(1);
    auto* event = p->set_track_event();
    event->set_track_uuid(123);
    event->set_name(name);
    event->set_type(begin ? protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN
                          : protos::pbzero::TrackEvent::TYPE_SLICE_END);
    event->set_thread_time_absolute_us(static_cast<int64_t>(ts + 100));
    event->set_thread_instruction_count_absolute(static_cast<int64_t>(ts * 10));
  };
  emit(2, true, "outer");
  for (uint32_t i = 0; i < 3000; ++i) {
    emit(10 + i * 10, true, "child");
    emit(15 + i * 10, false, "child");
  }
  emit(40000, false, "outer");
  Sinks baseline, streaming;
  auto full = ParseIntoSinks(trace.SerializeAsString(), true, baseline);
  auto streamed =
      ParseIntoSinks(trace.SerializeAsString(), true, streaming, false, true);
  ExpectSameSinkRecords(baseline, streaming);
  auto* sink = streaming.at(tables::SliceTable::Name()).get();
  EXPECT_EQ(sink->rows.size(), 3001u);
  EXPECT_GT(sink->patches_after_eviction, 0u);
  EXPECT_EQ(std::get<int64_t>(
                sink->rows[0][tables::SliceTable::ColumnIndex::thread_ts]),
            102000);
  EXPECT_EQ(std::get<int64_t>(
                sink->rows[0][tables::SliceTable::ColumnIndex::thread_dur]),
            39998000);
  EXPECT_EQ(
      std::get<int64_t>(
          sink->rows
              [0][tables::SliceTable::ColumnIndex::thread_instruction_delta]),
      399980);
}

TEST(StreamingTablesTest, PublicParserEntrypointAndLifecycle) {
  PublicOutput output;
  auto parser =
      TraceParser::CreateInstance(TraceParser::Config{}, output.sinks());
  std::string input =
      R"({"traceEvents":[{"ph":"B","ts":0,"pid":1,"tid":2,"name":"outer"})";
  for (uint32_t i = 0; i < 3000; ++i)
    input += R"(,{"ph":"X","ts":)" + std::to_string(i + 1) +
             R"(,"dur":1,"pid":1,"tid":2,"name":"work","args":{"key":)" +
             std::to_string(i) + "}}";
  input += R"(,{"ph":"E","ts":4000,"pid":1,"tid":2}]})";
  auto bytes = std::make_unique<uint8_t[]>(input.size());
  memcpy(bytes.get(), input.data(), input.size());
  EXPECT_TRUE(parser
                  ->Parse(TraceBlobView(
                      TraceBlob::TakeOwnership(std::move(bytes), input.size())))
                  .ok());
  EXPECT_TRUE(parser->NotifyEndOfFile().ok());
  EXPECT_EQ(output.rows[tables::SliceTable::Name()], 3001u);
  EXPECT_GT(output.rows[tables::ArgTable::Name()], 0u);
  EXPECT_GT(output.storage_frontiers[tables::SliceTable::Name()], 2000u);
  EXPECT_GT(output.completion_frontiers[tables::ArgTable::Name()], 2000u);
  EXPECT_GT(output.late_patches, 0u);
  EXPECT_TRUE(output.strings.count("work"));
  EXPECT_TRUE(output.ended);
  EXPECT_FALSE(parser->Parse(TraceBlobView{}).ok());
  EXPECT_FALSE(parser->NotifyEndOfFile().ok());
}

TEST(StreamingTablesTest, PublicOutputErrorsAreSticky) {
  PublicOutput output;
  auto parser =
      TraceParser::CreateInstance(TraceParser::Config{}, output.sinks());
  output.fail = true;
  std::string input =
      R"({"traceEvents":[{"ph":"X","ts":1,"dur":1,"pid":1,"tid":1,"name":"work"}]})";
  auto status = parser->Parse(
      TraceBlobView(TraceBlob::CopyFrom(input.data(), input.size())));
  ASSERT_TRUE(status.ok());
  status = parser->NotifyEndOfFile();
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.message(), "output failure");
  EXPECT_EQ(parser->Parse(TraceBlobView{}).message(), status.message());
  EXPECT_EQ(parser->NotifyEndOfFile().message(), status.message());
  EXPECT_FALSE(output.ended);
}

TEST(StreamingTablesTest, CounterSinkPatchesEvictedRowsAndDeferredTracks) {
  PublicOutput output;
  TraceParserImpl parser(TraceParser::Config{}, output.sinks());
  auto* context = parser.context()->ForkContextForTrace(TraceId{0}, 0);
  auto utid = context->process_tracker->GetOrCreateThread(42);
  for (uint32_t i = 0; i < 3000; ++i)
    context->event_tracker->PushProcessCounterForThread(
        EventTracker::RssStat{"rss"}, i, static_cast<double>(i) + 0.25, utid);
  auto* counters = context->storage->mutable_counter_table();
  EXPECT_EQ(output.rows[tables::CounterTable::Name()], 3000u);
  EXPECT_DOUBLE_EQ(output.counters.inserted_value, 2999.25);
  EXPECT_GT(counters->dataframe().first_retained_row(), 2000u);
  for (uint32_t col = 1; col < counters->dataframe().column_count(); ++col)
    EXPECT_FALSE(counters->dataframe().retains_column(col));
  // Models the backwards-looking GPU counter value patch, below storage.
  (*counters)[tables::CounterTable::Id{0}].set_value(12.5);
  EXPECT_DOUBLE_EQ(output.counters.patched_value, 12.5);
  context->event_tracker->FlushPendingEvents();
  EXPECT_EQ(output.counters.track_updates, 3000u);
  EXPECT_GT(output.late_patches, 2000u);
  EXPECT_EQ(counters->dataframe().completion_frontier(), 0u);
}

TEST(StreamingTablesTest, PublicParserErrorsAreSticky) {
  auto parser = TraceParser::CreateInstance(TraceParser::Config{});
  std::string garbage(8192, '~');
  auto status = parser->Parse(
      TraceBlobView(TraceBlob::CopyFrom(garbage.data(), garbage.size())));
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(parser->Parse(TraceBlobView{}).message(), status.message());
  EXPECT_EQ(parser->NotifyEndOfFile().message(), status.message());
}

}  // namespace
}  // namespace perfetto::trace_processor
