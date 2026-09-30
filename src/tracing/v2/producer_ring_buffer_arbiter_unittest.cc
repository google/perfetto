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

#include "src/tracing/v2/producer_ring_buffer_arbiter.h"

#include <string>
#include <vector>

#include "src/tracing/v2/producer_ring_buffer_test_fixture.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {

namespace test {

// Reaches the merge flag of a ProducerRingBufferArbiter.
class ProducerRingBufferArbiterTestPeer {
 public:
  // Acts like a writer that set the merge flag in PostDrainTask() but did
  // not post its drain task yet.
  static void SetDrainTaskPending(ProducerRingBufferArbiter* arbiter) {
    arbiter->drain_task_pending_.store(1);
  }
};

}  // namespace test

namespace {

using ::testing::_;
using State = ProducerRingBufferArbiter::State;
class ProducerRingBufferArbiterTest : public ProducerRingBufferTest {
 protected:
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
  protocol_abi_versions_ = {ProtocolAbiVersion::kV1};
  // Even if the service permits v2 in the config, the common set must too.
  SetupInstance(kInstance, V2Config());
  EXPECT_FALSE(CreateWriter());
  EXPECT_EQ(num_allocations_, 0u);
  EXPECT_EQ(num_attach_requests_, 0u);
}

TEST_F(ProducerRingBufferArbiterTest, V2OnlyConnectionCanSelectV2) {
  protocol_abi_versions_ = {ProtocolAbiVersion::kV2};
  SetupV2InstanceWithReader(/*num_chunks=*/4);
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
  EXPECT_EQ(arbiter_->state(), State::kNoRingBuffer);
}

TEST_F(ProducerRingBufferArbiterTest, InstanceUsesV1WithZeroProbability) {
  SetupInstance(kInstance, V2Config(/*probability_percent=*/0));
  EXPECT_FALSE(CreateWriter());
  EXPECT_EQ(num_allocations_, 0u);
}

TEST_F(ProducerRingBufferArbiterTest, UnknownInstanceUsesV1) {
  SetupV2InstanceWithReader(/*num_chunks=*/4);
  EXPECT_FALSE(CreateWriter(BufferExhaustedPolicy::kDrop, kInstance + 1));
}

TEST_F(ProducerRingBufferArbiterTest, StoppedInstanceUsesV1) {
  SetupV2InstanceWithReader(/*num_chunks=*/4);
  arbiter_->OnInstanceStopped(kInstance);
  EXPECT_FALSE(CreateWriter());
}

// --- The ring buffer and its attach request ---

TEST_F(ProducerRingBufferArbiterTest, FirstV2InstanceAttachesRingBuffer) {
  EXPECT_CALL(endpoint_, AttachV2RingBuffer(_, kChunkSize, _));
  SetupV2Instance(/*num_chunks=*/4);
  EXPECT_EQ(num_allocations_, 1u);
  EXPECT_EQ(service_memory_->size(), BudgetFor(4));
  EXPECT_FALSE(arbiter_->IsReaderAttached());

  AcceptAttach();
  EXPECT_EQ(arbiter_->state(), State::kAttached);
  EXPECT_TRUE(arbiter_->IsReaderAttached());

  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_NE(writer->writer_id(), 0u);
  EXPECT_TRUE(PacketReachesRingBuffer(writer.get()));
}

TEST_F(ProducerRingBufferArbiterTest, InstancesShareOneRingBuffer) {
  SetupV2InstanceWithReader(/*num_chunks=*/4);
  SetupInstance(kInstance + 1, V2Config());
  EXPECT_EQ(num_allocations_, 1u);
  EXPECT_EQ(num_attach_requests_, 1u);

  auto writer = CreateWriter(BufferExhaustedPolicy::kDrop, kInstance + 1);
  ASSERT_TRUE(writer);
  EXPECT_TRUE(PacketReachesRingBuffer(writer.get()));
}

TEST_F(ProducerRingBufferArbiterTest, NoRingBufferIfNoLayoutFitsTheBudget) {
  // The budget holds the header and one chunk, below kMinChunksPerRing.
  SetupInstance(kInstance, V2Config(), /*num_chunks=*/1);
  EXPECT_EQ(num_allocations_, 0u);
  EXPECT_EQ(arbiter_->state(), State::kNoRingBuffer);

  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
}

TEST_F(ProducerRingBufferArbiterTest, AllocationFailureRetriesOnNextInstance) {
  fail_allocation_ = true;
  SetupInstance(kInstance, V2Config());
  EXPECT_EQ(num_allocations_, 1u);
  EXPECT_EQ(num_attach_requests_, 0u);
  EXPECT_EQ(arbiter_->state(), State::kNoRingBuffer);

  // No v1 fallback for a v2 instance.
  auto null_writer = CreateWriter();
  ASSERT_TRUE(null_writer);
  EXPECT_EQ(null_writer->writer_id(), 0u);

  // The next v2 instance tries again. The first instance then gets ring
  // buffer writers too.
  fail_allocation_ = false;
  SetupInstance(kInstance + 1, V2Config());
  EXPECT_EQ(num_allocations_, 2u);
  EXPECT_EQ(num_attach_requests_, 1u);
  AcceptAttach();

  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_TRUE(PacketReachesRingBuffer(writer.get()));
}

TEST_F(ProducerRingBufferArbiterTest, RejectedAttachGivesNullTraceWriters) {
  SetupV2Instance(/*num_chunks=*/4);
  auto pending_writer = CreateWriter();
  EXPECT_NE(pending_writer->writer_id(), 0u);

  RejectAttach();
  EXPECT_EQ(arbiter_->state(), State::kDetached);
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);

  // Existing writers keep the mapping. A later instance does not try again.
  WritePacket(pending_writer.get(), "kept mapping");
  SetupInstance(kInstance + 1, V2Config());
  EXPECT_EQ(num_allocations_, 1u);
  EXPECT_EQ(num_attach_requests_, 1u);
}

TEST_F(ProducerRingBufferArbiterTest, AttachReplyAfterDisconnectIsIgnored) {
  SetupV2Instance(/*num_chunks=*/4);
  arbiter_->Disconnect();
  AcceptAttach();
  EXPECT_EQ(arbiter_->state(), State::kDetached);
  EXPECT_FALSE(arbiter_->IsReaderAttached());
}

TEST_F(ProducerRingBufferArbiterTest, AttachReplyAfterDestructionIsIgnored) {
  SetupV2Instance(/*num_chunks=*/4);
  arbiter_.reset();
  AcceptAttach();
}

TEST_F(ProducerRingBufferArbiterTest, DisconnectBeforeRingBufferIsFinal) {
  arbiter_->Disconnect();
  SetupInstance(kInstance, V2Config());
  EXPECT_EQ(num_allocations_, 0u);
  EXPECT_EQ(arbiter_->state(), State::kDetached);
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
}

// --- Writers ---

TEST_F(ProducerRingBufferArbiterTest, StalledWriterDrainsOnEndpointThread) {
  SetupV2InstanceWithReader(/*num_chunks=*/4);
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
  SetupV2Instance(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);
  const std::string payload(200, 'o');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_GT(writer->drop_count(), 0u);
  EXPECT_EQ(num_drain_requests_, 0u);
}

TEST_F(ProducerRingBufferArbiterTest, NullTraceWriterAfterDisconnect) {
  SetupV2Instance(/*num_chunks=*/4);
  arbiter_->Disconnect();
  auto disconnected_writer = CreateWriter();
  ASSERT_TRUE(disconnected_writer);
  EXPECT_EQ(disconnected_writer->writer_id(), 0u);
  WritePacket(disconnected_writer.get(), "discarded");
}

TEST_F(ProducerRingBufferArbiterTest, NoWriterIdAfterSmbArbiterShutdown) {
  SetupV2Instance(/*num_chunks=*/4);
  ASSERT_TRUE(smb_arbiter_->TryShutdown());
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
}

// --- Drain requests and flush ---

// A publication asks for a drain once a quarter of the positions wait for
// the reader. Requests from later publications merge into the pending task.
TEST_F(ProducerRingBufferArbiterTest,
       DrainRequestWaitsForAQuarterOfTheRingBuffer) {
  SetupV2InstanceWithReader(/*num_chunks=*/8);
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
  SetupV2InstanceWithReader(/*num_chunks=*/4);
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
  SetupV2InstanceWithReader(/*num_chunks=*/4);
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
  SetupV2Instance(/*num_chunks=*/8);
  auto writer = CreateWriter();
  WritePacket(writer.get(), "p");
  bool flushed = false;
  writer->Flush([&] { flushed = true; });
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
  EXPECT_EQ(num_drain_requests_, 1u);
  EXPECT_EQ(ReadPackets().size(), 0u);

  AcceptAttach();
  writer->Flush();
  task_runner_.RunUntilIdle();
  EXPECT_EQ(ReadPackets().size(), 1u);
}

TEST_F(ProducerRingBufferArbiterTest, FlushWithoutRingBufferRunsCallback) {
  bool flushed = false;
  arbiter_->Flush([&] { flushed = true; });
  EXPECT_TRUE(flushed);
  EXPECT_EQ(num_drain_requests_, 0u);
}

TEST_F(ProducerRingBufferArbiterTest, FlushAfterDisconnectRunsCallback) {
  SetupV2Instance(/*num_chunks=*/4);
  auto writer = CreateWriter();
  arbiter_->Disconnect();
  bool flushed = false;
  writer->Flush([&] { flushed = true; });
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
}

TEST_F(ProducerRingBufferArbiterTest, FlushRunsIfArbiterIsDestroyedFirst) {
  SetupV2Instance(/*num_chunks=*/4);
  bool flushed = false;
  arbiter_->Flush([&] { flushed = true; });
  arbiter_.reset();
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
  EXPECT_EQ(num_drain_requests_, 0u);
}

}  // namespace
}  // namespace perfetto::tracing_v2
