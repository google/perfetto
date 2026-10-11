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

#include "src/trace_processor/sorter/trace_sorter.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::MockFunction;
using ::testing::NiceMock;

struct FtraceEventData {
  TraceBlobView packet;
  uint32_t cpu;
};

class MockTraceStorage : public TraceStorage {
 public:
  MockTraceStorage() = default;

  MOCK_METHOD(StringId, InternString, (base::StringView view), (override));
};

template <typename T>
class MockSink : public TraceSorter::Sink<T, MockSink<T>> {
 public:
  void Parse(int64_t ts, T data) { MockParse(ts, std::move(data)); }
  MOCK_METHOD(void, MockParse, (int64_t, T));
};

class TraceSorterTest : public ::testing::Test {
 public:
  TraceSorterTest() : test_buffer_(TraceBlob::Allocate(8)) {
    storage_ = new NiceMock<MockTraceStorage>();
    context_.storage.reset(storage_);
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    CreateSorter();
  }

  void CreateSorter(bool full_sort = true) {
    auto sorting_mode = full_sort ? TraceSorter::SortingMode::kFullSort
                                  : TraceSorter::SortingMode::kDefault;
    context_.sorter.reset(new TraceSorter(&context_, sorting_mode));
  }

 protected:
  TraceProcessorContext context_;
  NiceMock<MockTraceStorage>* storage_;
  TraceBlobView test_buffer_;
};

TEST_F(TraceSorterTest, TestFtrace) {
  TraceBlobView view = test_buffer_.slice_off(0, 1);

  auto sink = std::make_unique<MockSink<FtraceEventData>>();
  auto* sink_ptr = sink.get();
  auto stream =
      context_.sorter->CreateStream(context_.trace_id(), std::move(sink));

  EXPECT_CALL(*sink_ptr, MockParse(1000, _));
  stream->Push(1000, {std::move(view), 0});
  context_.sorter->ExtractEventsForced();
}

TEST_F(TraceSorterTest, TestTracePacket) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  TraceBlobView view = test_buffer_.slice_off(0, 1);

  auto sink = std::make_unique<MockSink<TracePacketData>>();
  auto* sink_ptr = sink.get();
  auto stream =
      context_.sorter->CreateStream(context_.trace_id(), std::move(sink));

  EXPECT_CALL(*sink_ptr, MockParse(1000, _));
  stream->Push(1000, {std::move(view), state});
  context_.sorter->ExtractEventsForced();
}

TEST_F(TraceSorterTest, Ordering) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  TraceBlobView view_1 = test_buffer_.slice_off(0, 1);
  TraceBlobView view_2 = test_buffer_.slice_off(0, 2);
  TraceBlobView view_3 = test_buffer_.slice_off(0, 3);
  TraceBlobView view_4 = test_buffer_.slice_off(0, 4);

  auto ftrace_sink = std::make_unique<MockSink<FtraceEventData>>();
  auto* ftrace_sink_ptr = ftrace_sink.get();
  auto ftrace_stream = context_.sorter->CreateStream(context_.trace_id(),
                                                     std::move(ftrace_sink));

  auto packet_sink = std::make_unique<MockSink<TracePacketData>>();
  auto* packet_sink_ptr = packet_sink.get();
  auto packet_stream = context_.sorter->CreateStream(context_.trace_id(),
                                                     std::move(packet_sink));

  InSequence s;
  EXPECT_CALL(*ftrace_sink_ptr, MockParse(1000, _));
  EXPECT_CALL(*packet_sink_ptr, MockParse(1001, _));
  EXPECT_CALL(*packet_sink_ptr, MockParse(1100, _));
  EXPECT_CALL(*ftrace_sink_ptr, MockParse(1200, _));

  ftrace_stream->Push(1200, {std::move(view_4), 2});
  packet_stream->Push(1001, {std::move(view_2), state});
  packet_stream->Push(1100, {std::move(view_3), state});
  ftrace_stream->Push(1000, {std::move(view_1), 0});
  context_.sorter->ExtractEventsForced();
}

TEST_F(TraceSorterTest, IncrementalExtraction) {
  CreateSorter(false);

  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);

  TraceBlobView view_1 = test_buffer_.slice_off(0, 1);
  TraceBlobView view_2 = test_buffer_.slice_off(0, 2);
  TraceBlobView view_3 = test_buffer_.slice_off(0, 3);
  TraceBlobView view_4 = test_buffer_.slice_off(0, 4);
  TraceBlobView view_5 = test_buffer_.slice_off(0, 5);

  auto sink = std::make_unique<MockSink<TracePacketData>>();
  auto* sink_ptr = sink.get();
  auto stream =
      context_.sorter->CreateStream(context_.trace_id(), std::move(sink));

  // Flush at the start of packet sequence to match behavior of the
  // service.
  context_.sorter->NotifyFlushEvent();
  stream->Push(1200, {std::move(view_2), state});
  stream->Push(1100, {std::move(view_1), state});

  // No data should be exttracted at this point because we haven't
  // seen two flushes yet.
  context_.sorter->NotifyReadBufferEvent();

  // Now that we've seen two flushes, we should be ready to start extracting
  // data on the next OnReadBuffer call (after two flushes as usual).
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyReadBufferEvent();

  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  stream->Push(1400, {std::move(view_4), state});
  stream->Push(1300, {std::move(view_3), state});

  // This ReadBuffer call should finally extract until the first OnReadBuffer
  // call.
  {
    InSequence s;
    EXPECT_CALL(*sink_ptr, MockParse(1100, _));
    EXPECT_CALL(*sink_ptr, MockParse(1200, _));
  }
  context_.sorter->NotifyReadBufferEvent();

  context_.sorter->NotifyFlushEvent();
  stream->Push(1500, {std::move(view_5), state});

  // Nothing should be extracted as we haven't seen the second flush.
  context_.sorter->NotifyReadBufferEvent();

  // Now we've seen the second flush we should extract the next two packets.
  context_.sorter->NotifyFlushEvent();
  {
    InSequence s;
    EXPECT_CALL(*sink_ptr, MockParse(1300, _));
    EXPECT_CALL(*sink_ptr, MockParse(1400, _));
  }
  context_.sorter->NotifyReadBufferEvent();

  // The forced extraction should get the last packet.
  EXPECT_CALL(*sink_ptr, MockParse(1500, _));
  context_.sorter->ExtractEventsForced();
}

// Simulate a producer bug where the third packet is emitted
// out of order. Verify that we track the stats correctly.
TEST_F(TraceSorterTest, OutOfOrder) {
  CreateSorter(false);

  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);

  TraceBlobView view_1 = test_buffer_.slice_off(0, 1);
  TraceBlobView view_2 = test_buffer_.slice_off(0, 2);
  TraceBlobView view_3 = test_buffer_.slice_off(0, 3);
  TraceBlobView view_4 = test_buffer_.slice_off(0, 4);

  auto sink = std::make_unique<MockSink<TracePacketData>>();
  auto* sink_ptr = sink.get();
  auto stream =
      context_.sorter->CreateStream(context_.trace_id(), std::move(sink));

  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  stream->Push(1200, {std::move(view_2), state});
  stream->Push(1100, {std::move(view_1), state});
  context_.sorter->NotifyReadBufferEvent();

  // Both of the packets should have been pushed through.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  {
    InSequence s;
    EXPECT_CALL(*sink_ptr, MockParse(1100, _));
    EXPECT_CALL(*sink_ptr, MockParse(1200, _));
  }
  context_.sorter->NotifyReadBufferEvent();

  // Now, pass the third packet out of order.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  stream->Push(1150, {std::move(view_3), state});
  context_.sorter->NotifyReadBufferEvent();

  // Third packet should not be pushed through.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyReadBufferEvent();

  // We should also increment the stat that this was out of order.
  ASSERT_EQ(
      context_.global_stats_tracker->GetStats(
          std::nullopt, std::nullopt, stats::sorter_push_event_out_of_order),
      1);

  // Third packet should not be pushed through.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  stream->Push(1170, {std::move(view_4), state});
  context_.sorter->NotifyReadBufferEvent();
  context_.sorter->ExtractEventsForced();

  // We should also increment the stat that this was out of order.
  ASSERT_EQ(
      context_.global_stats_tracker->GetStats(
          std::nullopt, std::nullopt, stats::sorter_push_event_out_of_order),
      2);
}

// Simulates a random stream of ftrace events happening on random CPUs.
// Tests that the output of the TraceSorter matches the timestamp order
// (% events happening at the same time on different CPUs).
TEST_F(TraceSorterTest, MultiQueueSorting) {
  std::minstd_rand0 rnd_engine(0);
  std::map<int64_t /*ts*/, std::vector<uint32_t /*cpu*/>> expectations;

  constexpr uint32_t kMaxCpus = 32;
  std::vector<std::unique_ptr<TraceSorter::Stream<FtraceEventData>>> streams;
  for (uint32_t i = 0; i < kMaxCpus; ++i) {
    auto sink = std::make_unique<MockSink<FtraceEventData>>();
    auto* sink_ptr = sink.get();
    streams.emplace_back(
        context_.sorter->CreateStream(context_.trace_id(), std::move(sink)));

    EXPECT_CALL(*sink_ptr, MockParse(_, _))
        .WillRepeatedly(
            [&expectations](int64_t timestamp, FtraceEventData ftrace) {
              EXPECT_EQ(expectations.begin()->first, timestamp);
              auto& cpus = expectations.begin()->second;
              bool cpu_found = false;
              for (auto it = cpus.begin(); it < cpus.end(); it++) {
                if (*it != ftrace.cpu)
                  continue;
                cpu_found = true;
                cpus.erase(it);
                break;
              }
              if (cpus.empty())
                expectations.erase(expectations.begin());
              EXPECT_TRUE(cpu_found);
            });
  }

  // Allocate a 1000 byte trace blob and push one byte chunks to be sorted with
  // random timestamps. This will stress test the sorter with worst case
  // scenarios and will (and has many times) expose any subtle bugs hiding in
  // the sorter logic.
  TraceBlobView tbv(TraceBlob::Allocate(1000));
  for (uint16_t i = 0; i < 1000; i++) {
    int64_t ts = abs(static_cast<int64_t>(rnd_engine()));
    uint8_t num_cpus = rnd_engine() % 3;
    for (uint8_t j = 0; j < num_cpus; j++) {
      uint32_t cpu = static_cast<uint32_t>(rnd_engine() % kMaxCpus);
      expectations[ts].push_back(cpu);
      streams[cpu]->Push(ts, {tbv.slice_off(i, 1), cpu});
    }
  }

  context_.sorter->ExtractEventsForced();
  EXPECT_TRUE(expectations.empty());
}

TEST_F(TraceSorterTest, SetSortingMode) {
  CreateSorter(false);

  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);

  TraceBlobView view_1 = test_buffer_.slice_off(0, 1);
  TraceBlobView view_2 = test_buffer_.slice_off(0, 2);

  auto sink = std::make_unique<MockSink<TracePacketData>>();
  auto* sink_ptr = sink.get();
  auto stream =
      context_.sorter->CreateStream(context_.trace_id(), std::move(sink));

  EXPECT_CALL(*sink_ptr, MockParse(1000, _));
  stream->Push(1000, {std::move(view_1), state});

  // Changing to full sorting mode should succeed as no events have been
  // extracted yet.
  EXPECT_TRUE(
      context_.sorter->SetSortingMode(TraceSorter::SortingMode::kFullSort));

  EXPECT_CALL(*sink_ptr, MockParse(2000, _));
  stream->Push(2000, {std::move(view_2), state});

  // Changing back to default sorting mode is not allowed.
  EXPECT_FALSE(
      context_.sorter->SetSortingMode(TraceSorter::SortingMode::kDefault));

  // Setting sorting mode to the current mode should succeed.
  EXPECT_TRUE(
      context_.sorter->SetSortingMode(TraceSorter::SortingMode::kFullSort));

  context_.sorter->ExtractEventsForced();

  // Setting sorting mode to the current mode should still succeed.
  EXPECT_TRUE(
      context_.sorter->SetSortingMode(TraceSorter::SortingMode::kFullSort));
}

TEST_F(TraceSorterTest, SetSortingModeAfterExtraction) {
  CreateSorter(false);

  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);

  TraceBlobView view_1 = test_buffer_.slice_off(0, 1);
  TraceBlobView view_2 = test_buffer_.slice_off(0, 2);

  auto sink = std::make_unique<MockSink<TracePacketData>>();
  auto* sink_ptr = sink.get();
  auto stream =
      context_.sorter->CreateStream(context_.trace_id(), std::move(sink));

  EXPECT_CALL(*sink_ptr, MockParse(1000, _));
  stream->Push(1000, {std::move(view_1), state});
  EXPECT_CALL(*sink_ptr, MockParse(2000, _));
  stream->Push(2000, {std::move(view_2), state});
  context_.sorter->ExtractEventsForced();

  // Changing to full sorting mode should fail as some events have already been
  // extracted.
  EXPECT_FALSE(
      context_.sorter->SetSortingMode(TraceSorter::SortingMode::kFullSort));
}

TEST_F(TraceSorterTest, MaxTimestampForTraceTwoTraces) {
  TraceId trace_1{1};
  TraceId trace_2{2};

  auto sink_1a = std::make_unique<MockSink<FtraceEventData>>();
  auto stream_1a = context_.sorter->CreateStream(trace_1, std::move(sink_1a));
  auto sink_1b = std::make_unique<MockSink<FtraceEventData>>();
  auto stream_1b = context_.sorter->CreateStream(trace_1, std::move(sink_1b));

  auto sink_2a = std::make_unique<MockSink<FtraceEventData>>();
  auto stream_2a = context_.sorter->CreateStream(trace_2, std::move(sink_2a));
  auto sink_2b = std::make_unique<MockSink<FtraceEventData>>();
  auto stream_2b = context_.sorter->CreateStream(trace_2, std::move(sink_2b));

  stream_1a->Push(1000, {test_buffer_.slice_off(0, 1), 0});
  stream_1a->Push(1200, {test_buffer_.slice_off(0, 1), 0});
  stream_1b->Push(800, {test_buffer_.slice_off(0, 1), 0});
  stream_1b->Push(1500, {test_buffer_.slice_off(0, 1), 0});

  stream_2a->Push(2000, {test_buffer_.slice_off(0, 1), 0});
  stream_2a->Push(2200, {test_buffer_.slice_off(0, 1), 0});
  stream_2b->Push(1800, {test_buffer_.slice_off(0, 1), 0});
  stream_2b->Push(2100, {test_buffer_.slice_off(0, 1), 0});

  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace_1), 1500);
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace_2), 2200);
}

TEST_F(TraceSorterTest, MaxTimestampForTraceEmptyQueue) {
  TraceId trace{1};
  auto sink = std::make_unique<MockSink<FtraceEventData>>();
  auto stream = context_.sorter->CreateStream(trace, std::move(sink));

  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), std::nullopt);
}

TEST_F(TraceSorterTest, MaxTimestampForTraceTimestampZero) {
  TraceId trace{1};
  auto sink = std::make_unique<MockSink<FtraceEventData>>();
  auto stream = context_.sorter->CreateStream(trace, std::move(sink));

  stream->Push(0, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 0);
}

TEST_F(TraceSorterTest, MaxTimestampForTraceOneEmptyAndOneNonEmptyQueue) {
  TraceId trace{1};
  auto sink_a = std::make_unique<MockSink<FtraceEventData>>();
  auto stream_a = context_.sorter->CreateStream(trace, std::move(sink_a));
  auto sink_b = std::make_unique<MockSink<FtraceEventData>>();
  auto stream_b = context_.sorter->CreateStream(trace, std::move(sink_b));

  stream_b->Push(1000, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 1000);
}

TEST_F(TraceSorterTest, MaxTimestampForTraceOutOfOrder) {
  TraceId trace{1};
  auto sink = std::make_unique<MockSink<FtraceEventData>>();
  auto stream = context_.sorter->CreateStream(trace, std::move(sink));

  stream->Push(2000, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 2000);

  stream->Push(1000, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 2000);

  stream->Push(2500, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 2500);

  stream->Push(1500, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 2500);
}

TEST_F(TraceSorterTest, MaxTimestampForTraceAfterExtraction) {
  TraceId trace{1};
  auto sink = std::make_unique<MockSink<FtraceEventData>>();
  auto* sink_ptr = sink.get();
  auto stream = context_.sorter->CreateStream(trace, std::move(sink));

  stream->Push(1000, {test_buffer_.slice_off(0, 1), 0});
  stream->Push(2000, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 2000);

  EXPECT_CALL(*sink_ptr, MockParse(1000, _));
  EXPECT_CALL(*sink_ptr, MockParse(2000, _));
  context_.sorter->ExtractEventsForced();
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), std::nullopt);
}

// A synthetic timestamp (given by the tokenizer to data without one, possibly
// taken from other traces) orders the event, but doesn't count towards the
// trace's end.
TEST_F(TraceSorterTest, MaxTimestampForTraceIgnoresSyntheticTimestamps) {
  TraceId trace{1};
  auto sink = std::make_unique<MockSink<FtraceEventData>>();
  auto* sink_ptr = sink.get();
  auto stream = context_.sorter->CreateStream(trace, std::move(sink));

  stream->Push(1000, {test_buffer_.slice_off(0, 1), 0});
  stream->PushWithSyntheticTimestamp(9000, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 1000);
  EXPECT_EQ(context_.sorter->max_timestamp(), 9000);

  stream->Push(2000, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace), 2000);

  // A trace with only synthetic timestamps has no end.
  TraceId synthetic_only{2};
  auto other_sink = std::make_unique<MockSink<FtraceEventData>>();
  auto* other_sink_ptr = other_sink.get();
  auto other =
      context_.sorter->CreateStream(synthetic_only, std::move(other_sink));
  other->PushWithSyntheticTimestamp(5000, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(synthetic_only),
            std::nullopt);

  // The synthetic timestamps still decide the parsing order.
  InSequence s;
  EXPECT_CALL(*sink_ptr, MockParse(1000, _));
  EXPECT_CALL(*sink_ptr, MockParse(2000, _));
  EXPECT_CALL(*other_sink_ptr, MockParse(5000, _));
  EXPECT_CALL(*sink_ptr, MockParse(9000, _));
  context_.sorter->ExtractEventsForced();
}

TEST_F(TraceSorterTest, MaxTimestampForTraceUnknownTraceId) {
  TraceId trace_1{1};
  TraceId trace_unknown{99};
  auto sink = std::make_unique<MockSink<FtraceEventData>>();
  auto stream = context_.sorter->CreateStream(trace_1, std::move(sink));

  stream->Push(1000, {test_buffer_.slice_off(0, 1), 0});
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace_unknown), std::nullopt);
}

TEST_F(TraceSorterTest, MaxFtraceTimestampForTraceNonFtraceOrSyntheticOnly) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  TraceId trace_non_ftrace{1};
  TraceId trace_synthetic{2};

  // Trace 1 has only non-ftrace queues (TrackEventData and JsonEvent) with
  // non-synthetic timestamps.
  auto track_event_sink = std::make_unique<MockSink<TrackEventData>>();
  auto track_event_stream = context_.sorter->CreateStream(
      trace_non_ftrace, std::move(track_event_sink));
  track_event_stream->Push(1000,
                           TrackEventData(test_buffer_.slice_off(0, 1), state));

  auto json_sink = std::make_unique<MockSink<JsonEvent>>();
  auto json_stream =
      context_.sorter->CreateStream(trace_non_ftrace, std::move(json_sink));
  json_stream->Push(2000, JsonEvent{});

  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace_non_ftrace), 2000);
  EXPECT_EQ(context_.sorter->MaxFtraceTimestampForTrace(trace_non_ftrace),
            std::nullopt);

  // Trace 2 has ftrace queues, but only synthetic-timestamp events.
  auto ftrace_sink = std::make_unique<MockSink<FtraceData>>();
  auto ftrace_stream = context_.sorter->CreateStream(
      trace_synthetic, std::move(ftrace_sink), /*is_ftrace=*/true);
  FtraceData ftrace_data;
  ftrace_data.packet = test_buffer_.slice_off(0, 1);
  ftrace_data.sequence_state = state;
  ftrace_stream->PushWithSyntheticTimestamp(5000, std::move(ftrace_data));

  auto sched_switch_sink = std::make_unique<MockSink<InlineSchedSwitch>>();
  auto sched_switch_stream = context_.sorter->CreateStream(
      trace_synthetic, std::move(sched_switch_sink), /*is_ftrace=*/true);
  sched_switch_stream->PushWithSyntheticTimestamp(6000, InlineSchedSwitch{});

  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace_synthetic),
            std::nullopt);
  EXPECT_EQ(context_.sorter->MaxFtraceTimestampForTrace(trace_synthetic),
            std::nullopt);
}

TEST_F(TraceSorterTest, MaxFtraceTimestampForTraceMaxAcrossFtraceQueues) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  TraceId trace_1{1};
  TraceId trace_2{2};

  // Create queues for trace_1 across ftrace-capable event types:
  // FtraceData, InlineSchedSwitch, InlineSchedWaking, and FtraceEventData.
  auto ftrace_sink = std::make_unique<MockSink<FtraceData>>();
  auto ftrace_stream = context_.sorter->CreateStream(
      trace_1, std::move(ftrace_sink), /*is_ftrace=*/true);
  FtraceData data1;
  data1.packet = test_buffer_.slice_off(0, 1);
  data1.sequence_state = state;
  ftrace_stream->Push(1000, std::move(data1));
  FtraceData data2;
  data2.packet = test_buffer_.slice_off(0, 1);
  data2.sequence_state = state;
  // A synthetic-timestamp event on an ftrace queue should be ignored.
  ftrace_stream->PushWithSyntheticTimestamp(9000, std::move(data2));

  auto sched_switch_sink = std::make_unique<MockSink<InlineSchedSwitch>>();
  auto sched_switch_stream = context_.sorter->CreateStream(
      trace_1, std::move(sched_switch_sink), /*is_ftrace=*/true);
  sched_switch_stream->Push(1800, InlineSchedSwitch{});

  auto sched_waking_sink = std::make_unique<MockSink<InlineSchedWaking>>();
  auto sched_waking_stream = context_.sorter->CreateStream(
      trace_1, std::move(sched_waking_sink), /*is_ftrace=*/true);
  sched_waking_stream->Push(2000, InlineSchedWaking{});

  auto ftrace_event_sink = std::make_unique<MockSink<FtraceEventData>>();
  auto ftrace_event_stream = context_.sorter->CreateStream(
      trace_1, std::move(ftrace_event_sink), /*is_ftrace=*/true);
  ftrace_event_stream->Push(2200,
                            FtraceEventData{test_buffer_.slice_off(0, 1), 0});

  // Non-ftrace queues for trace_1 with larger timestamps should be ignored by
  // MaxFtraceTimestampForTrace.
  auto track_event_sink = std::make_unique<MockSink<TrackEventData>>();
  auto track_event_stream =
      context_.sorter->CreateStream(trace_1, std::move(track_event_sink));
  track_event_stream->Push(3000,
                           TrackEventData(test_buffer_.slice_off(0, 1), state));

  auto json_sink = std::make_unique<MockSink<JsonEvent>>();
  auto json_stream =
      context_.sorter->CreateStream(trace_1, std::move(json_sink));
  json_stream->Push(4000, JsonEvent{});

  // Ftrace queue for trace_2 with a larger timestamp should also be ignored.
  auto trace2_ftrace_sink = std::make_unique<MockSink<FtraceData>>();
  auto trace2_ftrace_stream = context_.sorter->CreateStream(
      trace_2, std::move(trace2_ftrace_sink), /*is_ftrace=*/true);
  FtraceData data_t2;
  data_t2.packet = test_buffer_.slice_off(0, 1);
  data_t2.sequence_state = state;
  trace2_ftrace_stream->Push(5000, std::move(data_t2));

  EXPECT_EQ(context_.sorter->MaxFtraceTimestampForTrace(trace_1), 2200);
  EXPECT_EQ(context_.sorter->MaxTimestampForTrace(trace_1), 4000);
  EXPECT_EQ(context_.sorter->MaxFtraceTimestampForTrace(trace_2), 5000);
  EXPECT_EQ(context_.sorter->MaxFtraceTimestampForTrace(TraceId{99}),
            std::nullopt);
}

}  // namespace
}  // namespace perfetto::trace_processor
