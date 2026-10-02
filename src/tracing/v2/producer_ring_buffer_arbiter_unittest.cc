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

// Tests for ProducerRingBufferArbiter, the producer object that coordinates
// all tracing v2 writers of one ring buffer. They cover the jobs that one
// writer cannot do alone:
// - Creation: the arbiter rejects a mapping with an invalid layout.
// - Writers: WriterIDs from the SMB arbiter, NullTraceWriters after a
//   disconnect, and no stall before the service reader is attached.
// - Drain requests: a writer asks for a drain at the occupancy threshold.
//   The arbiter merges the requests of all writers into one task.
// - Flush: the drain request goes out before the callback. The callback
//   also runs after a disconnect, or after the arbiter is destroyed.

#include "src/tracing/v2/producer_ring_buffer_arbiter.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/v2/producer_ring_buffer_test.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {

namespace test {

// Gives tests access to the private state of ProducerRingBufferArbiter.
class ProducerRingBufferArbiterTestPeer {
 public:
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
using ProducerRingBufferArbiterTest = ProducerRingBufferTest;

// --- Creation ---

TEST_F(ProducerRingBufferArbiterTest, CreateRejectsInvalidLayout) {
  auto create = [&](std::unique_ptr<SharedMemory> memory, uint32_t chunk_size) {
    return ProducerRingBufferArbiter::Create(&task_runner_, &endpoint_,
                                             smb_arbiter_.get(),
                                             std::move(memory), chunk_size);
  };
  EXPECT_FALSE(create(nullptr, kChunkSize));
  // 4096 bytes minus the header is not a whole number of chunks.
  EXPECT_FALSE(
      create(std::make_unique<InProcessSharedMemory>(4096), kChunkSize));
  // Three chunks is not a power of two.
  EXPECT_FALSE(create(CreateRingBufferMemory(3), kChunkSize));
  // Chunk size below the minimum.
  EXPECT_FALSE(create(CreateRingBufferMemory(4), 8));
  EXPECT_TRUE(create(CreateRingBufferMemory(4), kChunkSize));
}

// --- Writers ---

TEST_F(ProducerRingBufferArbiterTest, StalledWriterDrainsOnEndpointThread) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);

  // No task runs here. The writer runs on the endpoint thread, so each wait
  // for space sends the drain request at once, through
  // NotifyReader(kWriterStalled).
  const std::string payload(200, 's');
  for (int i = 0; i < 12; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 0u);
  EXPECT_GT(num_drain_requests_, 0u);
  writer.reset();
  task_runner_.RunUntilIdle();

  EXPECT_EQ(ReadPackets().size(), 12u);
}

TEST_F(ProducerRingBufferArbiterTest, WriterDoesNotWaitBeforeReaderAttached) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);
  const std::string payload(200, 'o');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_GT(writer->drop_count(), 0u);
  EXPECT_EQ(num_drain_requests_, 0u);
}

TEST_F(ProducerRingBufferArbiterTest, NullTraceWriterAfterDisconnect) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  arbiter_->Disconnect();
  auto disconnected_writer = CreateWriter();
  ASSERT_TRUE(disconnected_writer);
  EXPECT_EQ(disconnected_writer->writer_id(), 0u);
  WritePacket(disconnected_writer.get(), "discarded");
}

TEST_F(ProducerRingBufferArbiterTest, NoWriterIdAfterSmbArbiterShutdown) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  ASSERT_TRUE(smb_arbiter_->TryShutdown());
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
}

// --- Drain requests and flush ---

// Publications below the threshold stay buffered. Once it is reached,
// further notifications share the pending drain task.
TEST_F(ProducerRingBufferArbiterTest,
       DrainRequestsAreCoalescedAtOccupancyThreshold) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/8);
  auto writer = CreateWriter();
  // Two of these packets do not fit one 256-byte chunk.
  const std::string large(200, 'x');

  WritePacket(writer.get(), large);
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 0u);  // 1 of 8 positions waits.

  WritePacket(writer.get(), large);  // 2 of 8: asks for a drain.
  WritePacket(writer.get(), large);  // Merged into the pending task.
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
  EXPECT_EQ(ReadPackets().size(), 3u);
}

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
}

}  // namespace
}  // namespace perfetto::tracing_v2
