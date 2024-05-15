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

#include <map>
#include <random>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::MockFunction;
using ::testing::NiceMock;

constexpr std::optional<MachineId> kNullMachineId = std::nullopt;

class MockTraceParser : public ProtoTraceParserImpl {
 public:
  explicit MockTraceParser(TraceProcessorContext* context)
      : ProtoTraceParserImpl(context), machine_id_(context->machine_id()) {}

  MOCK_METHOD(void,
              MOCK_ParseFtracePacket,
              (uint32_t cpu,
               int64_t timestamp,
               const uint8_t* data,
               size_t length,
               std::optional<MachineId>));

  void ParseFtraceEvent(uint32_t cpu,
                        int64_t timestamp,
                        TracePacketData data) override {
    MOCK_ParseFtracePacket(cpu, timestamp, data.packet.data(),
                           data.packet.length(), machine_id_);
  }

  MOCK_METHOD(void,
              MOCK_ParseTracePacket,
              (int64_t ts, const uint8_t* data, size_t length));

  void ParseTrackEvent(int64_t, TrackEventData) override {}

  void ParseTracePacket(int64_t ts, TracePacketData data) override {
    TraceBlobView& tbv = data.packet;
    MOCK_ParseTracePacket(ts, tbv.data(), tbv.length());
  }

  std::optional<MachineId> machine_id_;
};

class MockTraceStorage : public TraceStorage {
 public:
  MockTraceStorage() : TraceStorage() {}

  MOCK_METHOD(StringId, InternString, (base::StringView view), (override));
};

class TraceSorterTest : public ::testing::Test {
 public:
  TraceSorterTest() : test_buffer_(TraceBlob::Allocate(8)) {
    storage_ = new NiceMock<MockTraceStorage>();
    context_.storage.reset(storage_);
    CreateSorter();
  }

  void CreateSorter(bool full_sort = true) {
    parser_ = new MockTraceParser(&context_);
    context_.proto_trace_parser.reset(parser_);
    auto sorting_mode = full_sort ? TraceSorter::SortingMode::kFullSort
                                  : TraceSorter::SortingMode::kDefault;
    context_.sorter.reset(new TraceSorter(&context_, sorting_mode));
  }

 protected:
  TraceProcessorContext context_;
  MockTraceParser* parser_;
  NiceMock<MockTraceStorage>* storage_;
  TraceBlobView test_buffer_;
};

TEST_F(TraceSorterTest, TestFtrace) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  TraceBlobView view = test_buffer_.slice_off(0, 1);
  EXPECT_CALL(*parser_,
              MOCK_ParseFtracePacket(0, 1000, view.data(), 1, kNullMachineId));
  context_.sorter->PushFtraceEvent(0 /*cpu*/, 1000 /*timestamp*/,
                                   std::move(view), state);
  context_.sorter->ExtractEventsForced();
}

TEST_F(TraceSorterTest, TestTracePacket) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  TraceBlobView view = test_buffer_.slice_off(0, 1);
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1000, view.data(), 1));
  context_.sorter->PushTracePacket(1000, state, std::move(view));
  context_.sorter->ExtractEventsForced();
}

TEST_F(TraceSorterTest, Ordering) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  TraceBlobView view_1 = test_buffer_.slice_off(0, 1);
  TraceBlobView view_2 = test_buffer_.slice_off(0, 2);
  TraceBlobView view_3 = test_buffer_.slice_off(0, 3);
  TraceBlobView view_4 = test_buffer_.slice_off(0, 4);

  InSequence s;

  EXPECT_CALL(*parser_, MOCK_ParseFtracePacket(0, 1000, view_1.data(), 1,
                                               kNullMachineId));
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1001, view_2.data(), 2));
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1100, view_3.data(), 3));
  EXPECT_CALL(*parser_, MOCK_ParseFtracePacket(2, 1200, view_4.data(), 4,
                                               kNullMachineId));

  context_.sorter->PushFtraceEvent(2 /*cpu*/, 1200 /*timestamp*/,
                                   std::move(view_4), state);
  context_.sorter->PushTracePacket(1001, state, std::move(view_2));
  context_.sorter->PushTracePacket(1100, state, std::move(view_3));
  context_.sorter->PushFtraceEvent(0 /*cpu*/, 1000 /*timestamp*/,
                                   std::move(view_1), state);
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

  // Flush at the start of packet sequence to match behavior of the
  // service.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->PushTracePacket(1200, state, std::move(view_2));
  context_.sorter->PushTracePacket(1100, state, std::move(view_1));

  // No data should be exttracted at this point because we haven't
  // seen two flushes yet.
  context_.sorter->NotifyReadBufferEvent();

  // Now that we've seen two flushes, we should be ready to start extracting
  // data on the next OnReadBufer call (after two flushes as usual).
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyReadBufferEvent();

  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  context_.sorter->PushTracePacket(1400, state, std::move(view_4));
  context_.sorter->PushTracePacket(1300, state, std::move(view_3));

  // This ReadBuffer call should finally extract until the first OnReadBuffer
  // call.
  {
    InSequence s;
    EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1100, test_buffer_.data(), 1));
    EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1200, test_buffer_.data(), 2));
  }
  context_.sorter->NotifyReadBufferEvent();

  context_.sorter->NotifyFlushEvent();
  context_.sorter->PushTracePacket(1500, state, std::move(view_5));

  // Nothing should be extracted as we haven't seen the second flush.
  context_.sorter->NotifyReadBufferEvent();

  // Now we've seen the second flush we should extract the next two packets.
  context_.sorter->NotifyFlushEvent();
  {
    InSequence s;
    EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1300, test_buffer_.data(), 3));
    EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1400, test_buffer_.data(), 4));
  }
  context_.sorter->NotifyReadBufferEvent();

  // The forced extraction should get the last packet.
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1500, test_buffer_.data(), 5));
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

  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  context_.sorter->PushTracePacket(1200, state, std::move(view_2));
  context_.sorter->PushTracePacket(1100, state, std::move(view_1));
  context_.sorter->NotifyReadBufferEvent();

  // Both of the packets should have been pushed through.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  {
    InSequence s;
    EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1100, test_buffer_.data(), 1));
    EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1200, test_buffer_.data(), 2));
  }
  context_.sorter->NotifyReadBufferEvent();

  // Now, pass the third packet out of order.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  context_.sorter->PushTracePacket(1150, state, std::move(view_3));
  context_.sorter->NotifyReadBufferEvent();

  // The third packet should still be pushed through.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1150, test_buffer_.data(), 3));
  context_.sorter->NotifyReadBufferEvent();

  // But we should also increment the stat that this was out of order.
  ASSERT_EQ(
      context_.storage->stats()[stats::sorter_push_event_out_of_order].value,
      1);

  // Push the fourth packet also out of order but after third.
  context_.sorter->NotifyFlushEvent();
  context_.sorter->NotifyFlushEvent();
  context_.sorter->PushTracePacket(1170, state, std::move(view_4));
  context_.sorter->NotifyReadBufferEvent();

  // The fourt packet should still be pushed through.
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(1170, test_buffer_.data(), 4));
  context_.sorter->ExtractEventsForced();

  // But we should also increment the stat that this was out of order.
  ASSERT_EQ(
      context_.storage->stats()[stats::sorter_push_event_out_of_order].value,
      2);
}

// Simulates a random stream of ftrace events happening on random CPUs.
// Tests that the output of the TraceSorter matches the timestamp order
// (% events happening at the same time on different CPUs).
TEST_F(TraceSorterTest, MultiQueueSorting) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  std::minstd_rand0 rnd_engine(0);
  std::map<int64_t /*ts*/, std::vector<uint32_t /*cpu*/>> expectations;

  EXPECT_CALL(*parser_, MOCK_ParseFtracePacket(_, _, _, _, _))
      .WillRepeatedly(Invoke([&expectations](uint32_t cpu, int64_t timestamp,
                                             const uint8_t*, size_t,
                                             std::optional<MachineId>) {
        EXPECT_EQ(expectations.begin()->first, timestamp);
        auto& cpus = expectations.begin()->second;
        bool cpu_found = false;
        for (auto it = cpus.begin(); it < cpus.end(); it++) {
          if (*it != cpu)
            continue;
          cpu_found = true;
          cpus.erase(it);
          break;
        }
        if (cpus.empty())
          expectations.erase(expectations.begin());
        EXPECT_TRUE(cpu_found);
      }));

  // Allocate a 1000 byte trace blob and push one byte chunks to be sorted with
  // random timestamps. This will stress test the sorter with worst case
  // scenarios and will (and has many times) expose any subtle bugs hiding in
  // the sorter logic.
  TraceBlobView tbv(TraceBlob::Allocate(1000));
  for (uint16_t i = 0; i < 1000; i++) {
    int64_t ts = abs(static_cast<int64_t>(rnd_engine()));
    uint8_t num_cpus = rnd_engine() % 3;
    for (uint8_t j = 0; j < num_cpus; j++) {
      uint32_t cpu = static_cast<uint32_t>(rnd_engine() % 32);
      expectations[ts].push_back(cpu);
      context_.sorter->PushFtraceEvent(cpu, ts, tbv.slice_off(i, 1), state);
    }
  }

  context_.sorter->ExtractEventsForced();
  EXPECT_TRUE(expectations.empty());
}

// An generalized version of MultiQueueSorting with multiple machines.
TEST_F(TraceSorterTest, MultiMachineSorting) {
  auto state = PacketSequenceStateGeneration::CreateFirst(&context_);
  std::minstd_rand0 rnd_engine(0);

  struct ExpectedMachineAndCpu {
    std::optional<MachineId> machine_id;
    uint32_t cpu;

    bool operator==(const ExpectedMachineAndCpu& other) const {
      return std::tie(machine_id, cpu) == std::tie(other.machine_id, other.cpu);
    }
    bool operator!=(const ExpectedMachineAndCpu& other) const {
      return !operator==(other);
    }
  };
  std::map<int64_t /*ts*/, std::vector<ExpectedMachineAndCpu>> expectations;

  // The total number of machines (including the default one).
  constexpr size_t num_machines = 5;
  std::vector<MockTraceParser*> extra_parsers;
  std::vector<std::unique_ptr<TraceProcessorContext>> extra_contexts;
  // Set up extra machines and add to the sorter.
  // MachineIdValue are 1..(num_machines-1).
  for (auto i = 1u; i < num_machines; i++) {
    TraceProcessorContext::InitArgs args{context_.config, context_.storage, i};
    auto ctx = std::make_unique<TraceProcessorContext>(args);
    auto parser = std::make_unique<MockTraceParser>(ctx.get());
    extra_parsers.push_back(parser.get());
    ctx->proto_trace_parser = std::move(parser);
    extra_contexts.push_back(std::move(ctx));
    context_.sorter->AddMachineContext(extra_contexts.back().get());
  }

  // Set up the expectation for the default machine.
  EXPECT_CALL(*parser_, MOCK_ParseFtracePacket(_, _, _, _, _))
      .WillRepeatedly(Invoke([&expectations](uint32_t cpu, int64_t timestamp,
                                             const uint8_t*, size_t,
                                             std::optional<MachineId>) {
        EXPECT_EQ(expectations.begin()->first, timestamp);
        auto& machines_and_cpus = expectations.begin()->second;
        bool found = false;
        for (auto it = machines_and_cpus.begin(); it < machines_and_cpus.end();
             it++) {
          // The default machine is called machine ID == std::nullopt.
          if (*it != ExpectedMachineAndCpu{kNullMachineId, cpu})
            continue;
          found = true;
          machines_and_cpus.erase(it);
          break;
        }
        if (machines_and_cpus.empty())
          expectations.erase(expectations.begin());
        EXPECT_TRUE(found);
      }));
  // Set up expectations for remote machines.
  for (auto* parser : extra_parsers) {
    EXPECT_CALL(*parser, MOCK_ParseFtracePacket(_, _, _, _, _))
        .WillRepeatedly(Invoke(
            [&expectations](uint32_t cpu, int64_t timestamp, const uint8_t*,
                            size_t, std::optional<MachineId> machine_id) {
              EXPECT_TRUE(machine_id.has_value());
              EXPECT_EQ(expectations.begin()->first, timestamp);
              auto& machines_and_cpus = expectations.begin()->second;
              bool found = false;
              for (auto it = machines_and_cpus.begin();
                   it < machines_and_cpus.end(); it++) {
                // Remote machines are called with non-null machine_id.
                if (*it != ExpectedMachineAndCpu{machine_id, cpu})
                  continue;
                found = true;
                machines_and_cpus.erase(it);
                break;
              }
              if (machines_and_cpus.empty())
                expectations.erase(expectations.begin());
              EXPECT_TRUE(found);
            }));
  }

  // Allocate a 1000 byte trace blob (per-machine) and push one byte chunks to
  // be sorted with random timestamps.
  constexpr size_t alloc_size = 1000;
  TraceBlobView tbv(TraceBlob::Allocate(alloc_size * num_machines));
  for (size_t m = 0; m < num_machines; m++) {
    // TraceProcessorContext::machine_id is nullopt for the default machine or a
    // monotonic counter starting from 1. 0 is a reserved value that isn't used.
    std::optional<MachineId> machine;
    if (m)
      machine = extra_contexts[m - 1]->machine_id();

    for (uint16_t i = 0; i < alloc_size; i++) {
      int64_t ts = abs(static_cast<int64_t>(rnd_engine()));
      uint8_t num_cpus = rnd_engine() % 3;
      for (uint8_t j = 0; j < num_cpus; j++) {
        uint32_t cpu = static_cast<uint32_t>(rnd_engine() % 32);
        expectations[ts].push_back(ExpectedMachineAndCpu{machine, cpu});
        context_.sorter->PushFtraceEvent(
            cpu, ts, tbv.slice_off(m * alloc_size + i, 1), state, machine);
      }
    }
  }

  context_.sorter->ExtractEventsForced();
  EXPECT_TRUE(expectations.empty());
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
