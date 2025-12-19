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
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
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
  const auto& stats = context_.storage->stats();
  ASSERT_EQ(stats[stats::sorter_push_event_out_of_order].value, 1);

  // Third packet should not be pushed through.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  stream->Push(1170, {std::move(view_4), state});
  context_.sorter->NotifyReadBufferEvent();
  context_.sorter->ExtractEventsForced();

  // We should also increment the stat that this was out of order.
  ASSERT_EQ(stats[stats::sorter_push_event_out_of_order].value, 2);
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
