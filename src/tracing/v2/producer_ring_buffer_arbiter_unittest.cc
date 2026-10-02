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
// - Instance choice: v1 or v2 for each instance, from the protocol mask, the
//   config and its probability.
// - Ring buffer and attach: creation for the first v2 instance, final
//   creation failures, and the attach replies.
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

#include <string>
#include <thread>
#include <vector>

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
};

// --- Choice of v1 or v2 per instance ---

TEST_F(ProducerRingBufferArbiterTest, V1OnlyConnectionDoesNotSelectV2) {
  protocol_abi_versions_ = kProtocolAbiV1;
  // Even if the service permits v2 in the config, the common mask must too.
  SetupInstance(kInstance, V2Config());
  EXPECT_FALSE(CreateWriter());
  EXPECT_EQ(num_allocations_, 0u);
  EXPECT_EQ(num_attach_requests_, 0u);
}

TEST_F(ProducerRingBufferArbiterTest, V2OnlyConnectionCanSelectV2) {
  protocol_abi_versions_ = kProtocolAbiV2;
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_TRUE(PacketReachesRingBuffer(writer.get()));
}

TEST_F(ProducerRingBufferArbiterTest, InstanceUsesV1WithoutServiceSupport) {
  DataSourceConfig config = V2Config();
  config.set_supports_tracing_v2(false);
  SetupInstance(kInstance, config);
  EXPECT_FALSE(CreateWriter());
  EXPECT_EQ(num_allocations_, 0u);
  EXPECT_EQ(reader_state(), ReaderState::kNoRingBuffer);
}

TEST_F(ProducerRingBufferArbiterTest, InstanceUsesV1WithZeroProbability) {
  SetupInstance(kInstance, V2Config(/*probability_percent=*/0));
  EXPECT_FALSE(CreateWriter());
  EXPECT_EQ(num_allocations_, 0u);
}

TEST_F(ProducerRingBufferArbiterTest, UnknownInstanceUsesV1) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  EXPECT_FALSE(CreateWriter(BufferExhaustedPolicy::kDrop, kInstance + 1));
}

TEST_F(ProducerRingBufferArbiterTest, StoppedInstanceUsesV1) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  arbiter_->OnInstanceStopped(kInstance);
  EXPECT_FALSE(CreateWriter());
}

// --- The ring buffer and its attach request ---

TEST_F(ProducerRingBufferArbiterTest, FirstV2InstanceAttachesRingBuffer) {
  EXPECT_CALL(endpoint_, AttachV2RingBuffer(_, kChunkSize, _));
  CreateRingBufferArbiter(/*num_chunks=*/4);
  EXPECT_EQ(num_allocations_, 1u);
  EXPECT_EQ(service_memory_->size(), sizeof(RingBufferHeader) + BudgetFor(4));
  EXPECT_FALSE(arbiter_->IsReaderAttached());

  AttachReader();
  EXPECT_EQ(reader_state(), ReaderState::kAttached);
  EXPECT_TRUE(arbiter_->IsReaderAttached());

  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_NE(writer->writer_id(), 0u);
  EXPECT_TRUE(PacketReachesRingBuffer(writer.get()));
}

TEST_F(ProducerRingBufferArbiterTest, InstancesShareOneRingBuffer) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  SetupInstance(kInstance + 1, V2Config());
  EXPECT_EQ(num_allocations_, 1u);
  EXPECT_EQ(num_attach_requests_, 1u);

  auto writer = CreateWriter(BufferExhaustedPolicy::kDrop, kInstance + 1);
  ASSERT_TRUE(writer);
  EXPECT_TRUE(PacketReachesRingBuffer(writer.get()));
}

TEST_F(ProducerRingBufferArbiterTest, NoRingBufferIfNoLayoutFitsTheBudget) {
  // The budget holds one chunk, below kMinChunksPerRing.
  SetupInstance(kInstance, V2Config(), /*num_chunks=*/1);
  EXPECT_EQ(num_allocations_, 0u);
  EXPECT_EQ(reader_state(), ReaderState::kDetached);

  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
}

TEST_F(ProducerRingBufferArbiterTest, AllocationFailureIsFinal) {
  fail_allocation_ = true;
  SetupInstance(kInstance, V2Config());
  EXPECT_EQ(num_allocations_, 1u);
  EXPECT_EQ(num_attach_requests_, 0u);
  EXPECT_EQ(reader_state(), ReaderState::kDetached);

  // No v1 fallback for a v2 instance.
  auto null_writer = CreateWriter();
  ASSERT_TRUE(null_writer);
  EXPECT_EQ(null_writer->writer_id(), 0u);

  // A later v2 instance does not try again.
  fail_allocation_ = false;
  SetupInstance(kInstance + 1, V2Config());
  EXPECT_EQ(num_allocations_, 1u);
  auto later_writer = CreateWriter(BufferExhaustedPolicy::kDrop, kInstance + 1);
  ASSERT_TRUE(later_writer);
  EXPECT_EQ(later_writer->writer_id(), 0u);
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
  // A later instance does not try again.
  WritePacket(pending_writer.get(), "kept mapping");
  SetupInstance(kInstance + 1, V2Config());
  EXPECT_EQ(num_allocations_, 1u);
  EXPECT_EQ(num_attach_requests_, 1u);
}

TEST_F(ProducerRingBufferArbiterTest, AttachReplyAfterDestructionIsIgnored) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  arbiter_.reset();
  AttachReader();
}

TEST_F(ProducerRingBufferArbiterTest, DisconnectBeforeRingBufferIsFinal) {
  arbiter_->Disconnect();
  SetupInstance(kInstance, V2Config());
  EXPECT_EQ(num_allocations_, 0u);
  EXPECT_EQ(reader_state(), ReaderState::kDetached);
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
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

}  // namespace
}  // namespace perfetto::tracing_v2
