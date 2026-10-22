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

// Tests for ProducerRingBufferArbiter, the producer side of one tracing v2
// ring buffer. They cover:
// - Creation: Create() picks the chunk size, ring buffer size and drain
//   threshold from the config, and returns the error status of a failed
//   allocation.
// - Attach: a new arbiter stays pending until the endpoint supplies a reply.
// - Endpoints without v2: the ProducerEndpoint defaults discard v2 packets.
// - Writers: WriterIDs from the SMB arbiter, and NullTraceWriters after a
//   disconnect.
// - Drain requests: RequestDrain() merges requests into one task. On the
//   endpoint thread, kUrgent sends the request at once.
// - Flush: the drain request goes out before the callback. The callback
//   also runs after a disconnect, or after the arbiter is destroyed.
//
// The writer decides when to ask for a drain and when to wait. Those tests
// are in trace_writer_v2_impl_unittest.cc.

#include "src/tracing/v2/producer_ring_buffer_arbiter.h"

#include <stddef.h>
#include <stdint.h>

#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/v2/producer_ring_buffer_test.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {

namespace test {

// Gives tests access to the private state of ProducerRingBufferArbiter.
class ProducerRingBufferArbiterTestPeer {
 public:
  static ProducerRingBufferArbiter::ReaderState reader_state(
      const ProducerRingBufferArbiter* arbiter) {
    return arbiter->reader_state_.load();
  }

  // Sets the merge flag of PostDrainTask() without posting the drain task.
  // This is the state of a writer that claimed the pending drain task but
  // did not post it yet. A test cannot stop a real writer in that window.
  static void SetDrainTaskPending(ProducerRingBufferArbiter* arbiter) {
    arbiter->drain_task_pending_.store(1);
  }
};

}  // namespace test

namespace {

using ::testing::_;
using DrainUrgency = ProducerRingBufferArbiter::DrainUrgency;
using ReaderState = ProducerRingBufferArbiter::ReaderState;

class ProducerRingBufferArbiterTest : public ProducerRingBufferTest {
 protected:
  ReaderState reader_state() const {
    return test::ProducerRingBufferArbiterTestPeer::reader_state(
        arbiter_.get());
  }

  // Writes one packet with |writer|, flushes, and returns true if the fake
  // service read it from the ring buffer.
  bool PacketReachesRingBuffer(TraceWriter* writer) {
    WritePacket(writer, "probe");
    writer->Flush();
    task_runner_.RunUntilIdle();
    const auto packets = ReadPackets();
    return packets.size() == 1 &&
           packets[0].packet.for_testing().str() == "probe";
  }

  // A chunk size option as {size_bytes, weight}.
  // A nullopt weight leaves the weight absent.
  using Option = std::pair<uint32_t, std::optional<uint32_t>>;

  static DataSourceConfig MakeConfig(
      std::initializer_list<Option> options,
      std::optional<int32_t> drain_occupancy_percent = std::nullopt) {
    DataSourceConfig config;
    auto* v2_config = config.mutable_experimental_tracing_v2();
    for (const auto& [size, weight] : options) {
      auto* option = v2_config->add_chunk_size_options();
      option->set_size_bytes(size);
      if (weight)
        option->set_weight(*weight);
    }
    if (drain_occupancy_percent)
      v2_config->set_drain_occupancy_percent(*drain_occupancy_percent);
    return config;
  }

  base::StatusOr<std::unique_ptr<ProducerRingBufferArbiter>> CreateArbiter(
      const DataSourceConfig& config,
      size_t shmem_size_hint_bytes) {
    return ProducerRingBufferArbiter::Create(
        &task_runner_, &endpoint_, config, shmem_size_hint_bytes,
        [](size_t size) -> base::StatusOr<std::shared_ptr<SharedMemory>> {
          return std::shared_ptr<SharedMemory>(
              InProcessSharedMemory::Create(size));
        });
  }

  // The chunk size chosen from |options|, or 0 if Create() fails.
  uint32_t ChosenChunkSize(std::initializer_list<Option> options) {
    auto arbiter_or = CreateArbiter(MakeConfig(options), /*hint=*/0);
    return arbiter_or.ok() ? (*arbiter_or)->ring_buffer()->chunk_size() : 0;
  }
};

// --- The ring buffer and its attach reply ---

// MockProducerEndpoint does not override InitializeV2RingBuffer() or
// CreateTraceWriterV2(), so these calls use the ProducerEndpoint defaults.
TEST_F(ProducerRingBufferArbiterTest, EndpointWithoutV2DiscardsV2Packets) {
  EXPECT_CALL(endpoint_, AttachV2RingBuffer(_, _, _)).Times(0);
  EXPECT_CALL(endpoint_, CreateTraceWriter(_, _)).Times(0);
  endpoint_.InitializeV2RingBuffer({});
  auto writer = endpoint_.CreateTraceWriterV2(kTargetBuffer,
                                              BufferExhaustedPolicy::kDrop);
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
  WritePacket(writer.get(), "discarded");
}

// Create() returns the error status of the allocation function.
TEST_F(ProducerRingBufferArbiterTest, CreateFailsIfAllocationFails) {
  auto arbiter_or = ProducerRingBufferArbiter::Create(
      &task_runner_, &endpoint_, DataSourceConfig(), BudgetFor(4),
      [](size_t) -> base::StatusOr<std::shared_ptr<SharedMemory>> {
        return base::ErrStatus("no memory");
      });
  ASSERT_FALSE(arbiter_or.ok());
  EXPECT_STREQ(arbiter_or.status().c_message(), "no memory");
}

TEST_F(ProducerRingBufferArbiterTest, CreateDoesNotAttachRingBuffer) {
  EXPECT_CALL(endpoint_, AttachV2RingBuffer(_, _, _)).Times(0);
  CreateRingBufferArbiter(/*num_chunks=*/4);
  EXPECT_EQ(arbiter_->memory()->size(),
            sizeof(RingBufferHeader) + BudgetFor(4));
  EXPECT_EQ(reader_state(), ReaderState::kPending);
  EXPECT_FALSE(arbiter_->IsReaderAttached());

  AttachReader();
  EXPECT_EQ(reader_state(), ReaderState::kAttached);
  EXPECT_TRUE(arbiter_->IsReaderAttached());

  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_NE(writer->writer_id(), 0u);
  EXPECT_TRUE(PacketReachesRingBuffer(writer.get()));
}

TEST_F(ProducerRingBufferArbiterTest, RejectedAttachGivesNullTraceWriters) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto pending_writer = CreateWriter();
  EXPECT_NE(pending_writer->writer_id(), 0u);

  RejectAttach();
  EXPECT_EQ(reader_state(), ReaderState::kDetached);
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);

  // Existing writers keep the mapping.
  WritePacket(pending_writer.get(), "kept mapping");
}

// --- Writers ---

TEST_F(ProducerRingBufferArbiterTest, NullTraceWriterAfterDisconnect) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  arbiter_->Disconnect();
  auto disconnected_writer = CreateWriter();
  ASSERT_TRUE(disconnected_writer);
  EXPECT_EQ(disconnected_writer->writer_id(), 0u);
  WritePacket(disconnected_writer.get(), "discarded");
}

// The service's accept reply can arrive after a disconnect. kDetached is
// terminal, so the arbiter ignores it.
TEST_F(ProducerRingBufferArbiterTest, LateAttachReplyAfterDisconnectIsIgnored) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  arbiter_->Disconnect();
  AttachReader();
  EXPECT_FALSE(arbiter_->IsReaderAttached());
  EXPECT_EQ(CreateWriter()->writer_id(), 0u);
}

// v1 and v2 writers of one producer take WriterIDs from one pool, because the
// service keys sequences by (producer, WriterID).
TEST_F(ProducerRingBufferArbiterTest, V1AndV2WritersShareWriterIds) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto v1_writer = smb_arbiter_->CreateTraceWriter(
      kTargetBuffer, BufferExhaustedPolicy::kDrop);
  auto v2_writer = CreateWriter();
  EXPECT_NE(v1_writer->writer_id(), 0u);
  EXPECT_NE(v2_writer->writer_id(), 0u);
  EXPECT_NE(v1_writer->writer_id(), v2_writer->writer_id());

  // Either live ID keeps the SMB arbiter from shutting down.
  v1_writer.reset();
  EXPECT_FALSE(smb_arbiter_->TryShutdown());
  v2_writer.reset();
  EXPECT_TRUE(smb_arbiter_->TryShutdown());
}

TEST_F(ProducerRingBufferArbiterTest, NoWriterIdAfterSmbArbiterShutdown) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  ASSERT_TRUE(smb_arbiter_->TryShutdown());
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
}

// --- Drain requests ---

// Requests from all threads share one pending drain task. The task clears
// the flag before it sends, so a later request posts a new task.
TEST_F(ProducerRingBufferArbiterTest, RoutineDrainRequestsShareOneTask) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  arbiter_->RequestDrain(DrainUrgency::kRoutine);
  arbiter_->RequestDrain(DrainUrgency::kRoutine);
  std::thread other_writer_thread(
      [&] { arbiter_->RequestDrain(DrainUrgency::kRoutine); });
  other_writer_thread.join();
  EXPECT_EQ(num_drain_requests_, 0u);
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);

  arbiter_->RequestDrain(DrainUrgency::kRoutine);
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 2u);
}

// On the endpoint thread, kUrgent sends the request at once, so the caller
// can block this thread next. It leaves a pending task alone. That task still
// sends its own request.
TEST_F(ProducerRingBufferArbiterTest, UrgentDrainIsDirectOnEndpointThread) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  arbiter_->RequestDrain(DrainUrgency::kRoutine);
  arbiter_->RequestDrain(DrainUrgency::kUrgent);
  EXPECT_EQ(num_drain_requests_, 1u);
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 2u);
}

// On another thread, kUrgent posts a task. Only the endpoint thread sends
// requests to the service.
TEST_F(ProducerRingBufferArbiterTest, UrgentDrainIsPostedFromOtherThread) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  std::thread other_writer_thread(
      [&] { arbiter_->RequestDrain(DrainUrgency::kUrgent); });
  other_writer_thread.join();
  EXPECT_EQ(num_drain_requests_, 0u);
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
}

// --- Flush ---

TEST_F(ProducerRingBufferArbiterTest, FlushRunsCallbackAfterDrainRequest) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  auto writer = CreateWriter();
  WritePacket(writer.get(), "flushed");

  // No ack: the callback does not wait for CommitData.
  EXPECT_CALL(endpoint_, CommitData(_, _)).Times(0);
  std::vector<ReadPacket> packets_at_callback;
  bool flushed = false;
  writer->Flush([&] {
    flushed = true;
    packets_at_callback = ReadPackets();
  });
  EXPECT_FALSE(flushed);
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
  // The test endpoint drains when it gets the request. So the data is in the
  // trace buffer when the callback runs.
  ASSERT_EQ(packets_at_callback.size(), 1u);
  EXPECT_EQ(packets_at_callback[0].packet.for_testing().str(), "flushed");
}

// A writer set the merge flag but did not post its drain task yet. The flush
// must still send a drain request before its callback runs.
TEST_F(ProducerRingBufferArbiterTest, FlushDrainsWhileAnotherDrainIsNotPosted) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  test::ProducerRingBufferArbiterTestPeer::SetDrainTaskPending(arbiter_.get());
  auto writer = CreateWriter();
  WritePacket(writer.get(), "flushed");

  std::vector<ReadPacket> packets_at_callback;
  writer->Flush([&] { packets_at_callback = ReadPackets(); });
  task_runner_.RunUntilIdle();
  ASSERT_EQ(packets_at_callback.size(), 1u);
  EXPECT_EQ(packets_at_callback[0].packet.for_testing().str(), "flushed");
}

// Drain requests do not depend on the reader state. Before the reader
// attaches, the service ignores them. The data stays in the ring buffer.
TEST_F(ProducerRingBufferArbiterTest, FlushBeforeReaderAttachedKeepsData) {
  // With 8 chunks, one packet does not reach the drain threshold.
  CreateRingBufferArbiter(/*num_chunks=*/8);
  auto writer = CreateWriter();
  WritePacket(writer.get(), "p");
  bool flushed = false;
  writer->Flush([&] { flushed = true; });
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
  EXPECT_EQ(num_drain_requests_, 1u);
  EXPECT_EQ(ReadPackets().size(), 0u);

  AttachReader();
  writer->Flush();
  task_runner_.RunUntilIdle();
  EXPECT_EQ(ReadPackets().size(), 1u);
}

TEST_F(ProducerRingBufferArbiterTest, FlushAfterDisconnectRunsCallback) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto writer = CreateWriter();
  arbiter_->Disconnect();
  bool flushed = false;
  writer->Flush([&] { flushed = true; });
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
}

TEST_F(ProducerRingBufferArbiterTest, FlushRunsIfArbiterIsDestroyedFirst) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  bool flushed = false;
  arbiter_->Flush([&] { flushed = true; });
  reader_.reset();
  reader_ring_buffer_.reset();
  arbiter_.reset();
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
  // The drain task saw that the arbiter is gone, and sent nothing.
  EXPECT_EQ(num_drain_requests_, 0u);
}

// --- Ring buffer sizing ---

// Weight 0 means never, and an absent weight counts.
TEST_F(ProducerRingBufferArbiterTest, ZeroWeightChunkSizeIsNeverChosen) {
  EXPECT_EQ(ChosenChunkSize({{512, 0}, {1024, std::nullopt}}), 1024u);
}

// Without an option that can be chosen, the chunk size is kMinChunkSize.
TEST_F(ProducerRingBufferArbiterTest, UnusableOptionsUseDefaultChunkSize) {
  EXPECT_EQ(ChosenChunkSize({}), kMinChunkSize);
  EXPECT_EQ(ChosenChunkSize({{512, 0}, {1024, 0}}), kMinChunkSize);
  EXPECT_EQ(ChosenChunkSize({{100, std::nullopt}, {258, 5}}), kMinChunkSize);
}

// An invalid size is never chosen.
// The service rejects it, but an older service does not.
TEST_F(ProducerRingBufferArbiterTest, InvalidChunkSizeIsNeverChosen) {
  EXPECT_EQ(ChosenChunkSize({{100, 5}, {258, 5}, {2048, 1}}), 2048u);
}

// The total weight exceeds 32 bits. Equal sizes make the result deterministic.
TEST_F(ProducerRingBufferArbiterTest, LargeWeightTotalDoesNotOverflow) {
  EXPECT_EQ(
      ChosenChunkSize({{512, std::numeric_limits<uint32_t>::max()}, {512, 1}}),
      512u);
}

// The layout uses the chosen chunk size, also when it is not a power of two.
TEST_F(ProducerRingBufferArbiterTest, LayoutUsesChosenChunkSize) {
  // The default 128 KiB hint fits 170 chunks of 768 bytes, rounded down to 128.
  // The threshold must use this count, not a count based on kMinChunkSize.
  auto arbiter_or =
      CreateArbiter(MakeConfig({{768, std::nullopt}}), /*hint=*/0);
  ASSERT_TRUE(arbiter_or.ok());
  EXPECT_EQ((*arbiter_or)->ring_buffer()->chunk_size(), 768u);
  EXPECT_EQ((*arbiter_or)->memory()->size(),
            sizeof(RingBufferHeader) + 128 * 768);
  EXPECT_EQ((*arbiter_or)->drain_occupancy_threshold(), 32u);

  // A 1 KiB hint holds two chunks of 512 bytes, but only one of 1024 bytes.
  auto small = CreateArbiter(MakeConfig({{512, std::nullopt}}), 1024);
  ASSERT_TRUE(small.ok());
  EXPECT_EQ((*small)->memory()->size(), sizeof(RingBufferHeader) + 2 * 512);
  EXPECT_FALSE(CreateArbiter(MakeConfig({{1024, std::nullopt}}), 1024).ok());
}

// The threshold is a percent of the chunk count, rounded down, and at least 1.
TEST_F(ProducerRingBufferArbiterTest, DrainOccupancyThreshold) {
  struct Case {
    uint32_t num_chunks;
    std::optional<int32_t> percent;
    uint32_t threshold;
  };
  const Case cases[] = {
      // Absent and 0 mean 25%.
      {8, std::nullopt, 2},
      {8, 0, 2},
      // -1 means one position.
      {8, -1, 1},
      {8, 50, 4},
      {8, 100, 8},
      // 99% of 128 is 126.72.
      {128, 99, 126},
      // 1% of 128 is 1.28.
      {128, 1, 1},
      // 25% of 2 rounds down to 0, so the minimum of 1 applies.
      {2, std::nullopt, 1},
  };
  for (size_t i = 0; i < base::ArraySize(cases); ++i) {
    SCOPED_TRACE(i);
    const Case& c = cases[i];
    auto arbiter_or =
        CreateArbiter(MakeConfig({}, c.percent), c.num_chunks * kMinChunkSize);
    ASSERT_TRUE(arbiter_or.ok());
    EXPECT_EQ((*arbiter_or)->memory()->size(),
              sizeof(RingBufferHeader) + c.num_chunks * kMinChunkSize);
    EXPECT_EQ((*arbiter_or)->drain_occupancy_threshold(), c.threshold);
  }
}

}  // namespace
}  // namespace perfetto::tracing_v2
