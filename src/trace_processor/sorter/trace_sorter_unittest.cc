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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string_view>
#include <tuple>
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
using ::testing::ElementsAre;
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

  MOCK_METHOD(StringId, InternString, (std::string_view view), (override));
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

  // Creates a stream whose parsed events are appended to |parsed| as
  // (timestamp, cpu).
  std::unique_ptr<TraceSorter::Stream<FtraceEventData>> CreateRecordingStream(
      std::vector<std::pair<int64_t, uint32_t>>* parsed) {
    auto sink = std::make_unique<MockSink<FtraceEventData>>();
    EXPECT_CALL(*sink, MockParse(_, _))
        .WillRepeatedly([parsed](int64_t ts, FtraceEventData data) {
          parsed->emplace_back(ts, data.cpu);
        });
    return context_.sorter->CreateStream(std::move(sink));
  }

  // Signals the flushes and read buffer which extract, in default sorting
  // mode, the events pushed before the previous such call.
  void ReadBuffer() {
    context_.sorter->NotifyFlushEvent();
    context_.sorter->NotifyFlushEvent();
    context_.sorter->NotifyReadBufferEvent();
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
  auto stream = context_.sorter->CreateStream(std::move(sink));

  EXPECT_CALL(*sink_ptr, MockParse(1000, _));
  stream->Push(1000, {std::move(view), 0});
  context_.sorter->ExtractEventsForced();
}

TEST_F(TraceSorterTest, TestTracePacket) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  TraceBlobView view = test_buffer_.slice_off(0, 1);

  auto sink = std::make_unique<MockSink<TracePacketData>>();
  auto* sink_ptr = sink.get();
  auto stream = context_.sorter->CreateStream(std::move(sink));

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
  auto ftrace_stream = context_.sorter->CreateStream(std::move(ftrace_sink));

  auto packet_sink = std::make_unique<MockSink<TracePacketData>>();
  auto* packet_sink_ptr = packet_sink.get();
  auto packet_stream = context_.sorter->CreateStream(std::move(packet_sink));

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
  auto stream = context_.sorter->CreateStream(std::move(sink));

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
  auto stream = context_.sorter->CreateStream(std::move(sink));

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
TEST_F(TraceSorterTest, MultiStreamSorting) {
  std::minstd_rand0 rnd_engine(0);
  std::map<int64_t /*ts*/, std::vector<uint32_t /*cpu*/>> expectations;

  constexpr uint32_t kMaxCpus = 32;
  std::vector<std::unique_ptr<TraceSorter::Stream<FtraceEventData>>> streams;
  for (uint32_t i = 0; i < kMaxCpus; ++i) {
    auto sink = std::make_unique<MockSink<FtraceEventData>>();
    auto* sink_ptr = sink.get();
    streams.emplace_back(context_.sorter->CreateStream(std::move(sink)));

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

TEST_F(TraceSorterTest, EqualTimestampsAcrossStreamsInArrivalOrder) {
  std::vector<uint32_t> order;
  std::vector<std::unique_ptr<TraceSorter::Stream<FtraceEventData>>> streams;
  for (uint32_t i = 0; i < 2; ++i) {
    auto sink = std::make_unique<MockSink<FtraceEventData>>();
    EXPECT_CALL(*sink, MockParse(_, _))
        .WillRepeatedly([&order](int64_t, FtraceEventData data) {
          order.push_back(data.cpu);
        });
    streams.emplace_back(context_.sorter->CreateStream(std::move(sink)));
  }

  // The second stream's events arrive first and in between the first's.
  TraceBlobView tbv(TraceBlob::Allocate(4));
  streams[1]->Push(10, {tbv.slice_off(0, 1), 0});
  streams[0]->Push(10, {tbv.slice_off(1, 1), 1});
  streams[1]->Push(10, {tbv.slice_off(2, 1), 2});
  streams[0]->Push(10, {tbv.slice_off(3, 1), 3});

  context_.sorter->ExtractEventsForced();
  EXPECT_THAT(order, ElementsAre(0, 1, 2, 3));
}

using Events = std::vector<std::pair<int64_t, uint32_t>>;

// The order events must be extracted in: by timestamp, then arrival.
Events SortedByTs(Events events) {
  std::stable_sort(
      events.begin(), events.end(),
      [](const auto& a, const auto& b) { return a.first < b.first; });
  return events;
}

constexpr uint32_t kBlock = TraceSorter::kBlockEvents;

// Events from several streams, out of order within and across blocks and with
// many equal timestamps, extracted incrementally from partly drained blocks
// and then by forced extractions, with more events pushed after the first.
TEST_F(TraceSorterTest, BlocksExtractInTsThenArrivalOrder) {
  CreateSorter(false);
  std::minstd_rand0 rnd_engine(0);
  Events parsed;
  std::vector<std::unique_ptr<TraceSorter::Stream<FtraceEventData>>> streams;
  for (uint32_t i = 0; i < 3; ++i) {
    streams.push_back(CreateRecordingStream(&parsed));
  }
  TraceBlobView tbv(TraceBlob::Allocate(1));
  Events pushed;
  auto push = [&](int64_t ts) {
    auto n = static_cast<uint32_t>(pushed.size());
    pushed.emplace_back(ts, n);
    streams[rnd_engine() % streams.size()]->Push(ts, {tbv.slice_off(0, 1), n});
  };

  // Jittered across block edges. Each extraction takes the events pushed
  // before the previous one, so batches larger than the jitter keep every
  // event in order.
  constexpr uint32_t kJitter = 8192;
  constexpr uint32_t kEvents = 4 * kBlock + 17;
  for (uint32_t i = 0; i < kEvents; ++i) {
    push(static_cast<int64_t>((i + rnd_engine() % kJitter) / 4));
    if (i % (kBlock / 2 + 1000) == 0) {
      ReadBuffer();
    }
  }
  ASSERT_GT(parsed.size(), 0u);
  ASSERT_LT(parsed.size(), pushed.size());
  context_.sorter->ExtractEventsForced();
  ASSERT_EQ(parsed.size(), pushed.size());

  // Another block and a bit, in reverse.
  for (uint32_t i = 0; i < kBlock + 5; ++i) {
    push(static_cast<int64_t>(kEvents + kBlock + 5 - i));
  }
  context_.sorter->ExtractEventsForced();
  EXPECT_EQ(parsed, SortedByTs(pushed));
  EXPECT_EQ(
      context_.global_stats_tracker->GetStats(
          std::nullopt, std::nullopt, stats::sorter_push_event_out_of_order),
      0);
}

// Destroying the sorter discards the events of partly extracted blocks.
TEST_F(TraceSorterTest, DestroyWithPartlyExtractedBlocks) {
  CreateSorter(false);
  Events parsed;
  auto stream = CreateRecordingStream(&parsed);
  TraceBlobView tbv(TraceBlob::Allocate(1));
  for (uint32_t i = 0; i < kBlock + kBlock / 2; ++i) {
    stream->Push(1000 + i, {tbv.slice_off(0, 1), i});
  }
  ReadBuffer();
  for (uint32_t i = 0; i < kBlock; ++i) {
    stream->Push(1000 + kBlock + i, {tbv.slice_off(0, 1), i});
  }
  ReadBuffer();
  ASSERT_EQ(parsed.size(), kBlock + 1u);

  // Every pending event is discarded, freeing its token buffer allocation:
  // ~BumpAllocator CHECKs they all are.
  context_.sorter.reset(nullptr);
  EXPECT_EQ(parsed.size(), kBlock + 1u);
}

// JSON events, which order equal timestamps by phase, sorted across blocks,
// some of them sorted before the order changed.
TEST_F(TraceSorterTest, SlowSortingAcrossBlocks) {
  std::minstd_rand0 rnd_engine(0);
  auto sink = std::make_unique<MockSink<JsonEvent>>();
  std::vector<uint32_t> parsed;
  EXPECT_CALL(*sink, MockParse(_, _))
      .WillRepeatedly(
          [&parsed](int64_t, JsonEvent event) { parsed.push_back(event.pid); });
  auto stream = context_.sorter->CreateStream(std::move(sink));

  struct Pushed {
    int64_t ts;
    int64_t key;
    uint32_t id;
  };
  std::vector<Pushed> pushed;
  constexpr uint32_t kEvents = 3 * kBlock + 17;
  for (uint32_t i = 0; i < kEvents; ++i) {
    JsonEvent event;
    event.pid = i;
    // 'X' events, which turn on slow sorting, only after the first block.
    uint32_t phases = i < kBlock + 10 ? 2 : 3;
    int64_t key = std::numeric_limits<int64_t>::max();
    switch (rnd_engine() % phases) {
      case 0:
        event.phase = 'B';
        break;
      case 1:
        event.phase = 'E';
        key = std::numeric_limits<int64_t>::min();
        break;
      default:
        event.phase = 'X';
        event.dur = static_cast<int64_t>(rnd_engine() % 10);
        key -= event.dur;
        break;
    }
    // Few distinct timestamps, overlapping across blocks, so many ties.
    auto ts = static_cast<int64_t>(i / 8 + rnd_engine() % 4096);
    pushed.push_back({ts, key, i});
    stream->Push(ts, std::move(event));
  }
  context_.sorter->ExtractEventsForced();

  std::stable_sort(pushed.begin(), pushed.end(),
                   [](const Pushed& a, const Pushed& b) {
                     return std::tie(a.ts, a.key) < std::tie(b.ts, b.key);
                   });
  std::vector<uint32_t> expected;
  for (const Pushed& p : pushed) {
    expected.push_back(p.id);
  }
  EXPECT_EQ(parsed, expected);
}

TEST_F(TraceSorterTest, SetSortingMode) {
  CreateSorter(false);

  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);

  TraceBlobView view_1 = test_buffer_.slice_off(0, 1);
  TraceBlobView view_2 = test_buffer_.slice_off(0, 2);

  auto sink = std::make_unique<MockSink<TracePacketData>>();
  auto* sink_ptr = sink.get();
  auto stream = context_.sorter->CreateStream(std::move(sink));

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
  auto stream = context_.sorter->CreateStream(std::move(sink));

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

}  // namespace
}  // namespace perfetto::trace_processor
