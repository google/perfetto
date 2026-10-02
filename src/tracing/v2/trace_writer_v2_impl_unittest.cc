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

// Tests for TraceWriterV2Impl, the TraceWriter that writes packets into the
// tracing v2 ring buffer. They cover what this class adds on top of
// SharedRingBufferWriter:
// - proto group encoding, also for packets that cross chunks,
// - closing and publishing packets, also on Flush(),
// - drop mode, loss reports and the drop_count() and written() counters,
// - publishing the last chunk before the WriterID is released.

#include "src/tracing/v2/trace_writer_v2_impl.h"

#include <cstdint>
#include <string>

#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/tracing/v2/producer_ring_buffer_test.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::tracing_v2 {
namespace {

using TraceWriterV2ImplTest = ProducerRingBufferTest;

// --- Packet encoding ---

TEST_F(TraceWriterV2ImplTest, PacketsRoundTripThroughTraceBuffer) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/8);
  // 600 bytes need three 256-byte chunks, so the nested message crosses chunk
  // boundaries. A length-delimited message would need a patch here, which
  // TraceWriterV2Impl does not support. So this also checks the proto group
  // encoding.
  const std::string large(600, 'x');
  {
    auto writer = CreateWriter();
    WritePacket(writer.get(), "small");
    WritePacket(writer.get(), large);
    EXPECT_EQ(writer->drop_count(), 0u);
  }
  task_runner_.RunUntilIdle();

  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "small");
  EXPECT_TRUE(packets[0].packet.first_packet_on_sequence());
  EXPECT_EQ(packets[1].packet.for_testing().str(), large);
  EXPECT_FALSE(packets[1].packet.first_packet_on_sequence());
  EXPECT_EQ(packets[1].previous_dropped, 0u);
}

// --- Packet closing ---

// A caller can finalize the message without the handle. The last fragment
// then stays open. NewTracePacket() and Flush() must publish it.
TEST_F(TraceWriterV2ImplTest, NextPacketOrFlushPublishesFinalizedPacket) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/8);
  auto writer = CreateWriter();
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str("closed by next packet");
    packet->Finalize();
  }
  WritePacket(writer.get(), "next");
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str("closed by flush");
    packet->Finalize();
  }
  writer->Flush();
  task_runner_.RunUntilIdle();

  // The writer is still alive, so its destructor did not publish the data.
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 3u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "closed by next packet");
  EXPECT_EQ(packets[1].packet.for_testing().str(), "next");
  EXPECT_EQ(packets[2].packet.for_testing().str(), "closed by flush");
}

// A raw stream caller writes without the message and then calls
// FinishTracePacket(). That call must publish the bytes, also across chunks.
TEST_F(TraceWriterV2ImplTest, FinishTracePacketPublishesRawStreamPacket) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/8);
  // 600 bytes need three 256-byte chunks.
  const std::string large(600, 'r');
  protos::gen::TracePacket raw_packet;
  raw_packet.mutable_for_testing()->set_str(large);
  const std::string raw_bytes = raw_packet.SerializeAsString();

  auto writer = CreateWriter();
  {
    auto packet = writer->NewTracePacket();
    protozero::ScatteredStreamWriter* stream = packet.TakeStreamWriter();
    stream->WriteBytes(reinterpret_cast<const uint8_t*>(raw_bytes.data()),
                       raw_bytes.size());
  }
  writer->FinishTracePacket();
  writer->Flush();
  task_runner_.RunUntilIdle();

  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), large);
  EXPECT_EQ(writer->drop_count(), 0u);
}

// --- Flush ---

// Flush() releases the cached chunk. The next packet must use a new
// reservation, so a later flush can deliver it.
TEST_F(TraceWriterV2ImplTest, FlushReleasesCachedChunk) {
  // Without a reader, nothing moves the positions.
  CreateRingBufferArbiter(/*num_chunks=*/8);
  auto writer = CreateWriter();
  WritePacket(writer.get(), "a");
  WritePacket(writer.get(), "b");
  EXPECT_EQ(reader_ring_buffer_->LoadWritePosRelaxed(), 1u);

  writer->Flush();
  task_runner_.RunUntilIdle();
  WritePacket(writer.get(), "c");
  EXPECT_EQ(reader_ring_buffer_->LoadWritePosRelaxed(), 2u);

  writer.reset();
  AttachReader();
  Drain();
  EXPECT_EQ(ReadPackets().size(), 3u);
}

// --- Buffer exhaustion ---

TEST_F(TraceWriterV2ImplTest, DropWhenFullThenReportLoss) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kDrop);

  // Nothing drains until the tasks run. Each 200-byte packet needs its own
  // chunk, so the fifth packet finds the ring buffer full.
  const std::string payload(200, 'd');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  ASSERT_GT(writer->drop_count(), 0u);
  task_runner_.RunUntilIdle();
  ReadPackets();

  // The first chunk after the loss carries kFlagDataLoss. The reader discards
  // all of its fragments, so this packet is lost too.
  WritePacket(writer.get(), "in flagged chunk");
  writer->Flush();
  WritePacket(writer.get(), "recovered");
  writer.reset();
  task_runner_.RunUntilIdle();

  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "recovered");
  EXPECT_NE(packets[0].previous_dropped, 0u);
}

// --- Counters ---

// Consecutive dropped packets count as one drop, as in TraceWriterImpl.
// A new drop after a packet that fits counts again.
TEST_F(TraceWriterV2ImplTest, DropCountCountsEachLossOnce) {
  // Without a reader, kDrop does not wait and nothing frees space.
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kDrop);
  // Each 200-byte packet needs its own chunk.
  const std::string payload(200, 'd');
  for (int i = 0; i < 4; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 0u);

  // The first packet loses its tail. The next two packets get no chunk.
  for (int i = 0; i < 3; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 1u);

  // Free the ring buffer. The next packet fits and ends the drop.
  AttachReader();
  Drain();
  WritePacket(writer.get(), "fits");
  EXPECT_EQ(writer->drop_count(), 1u);

  for (int i = 0; i < 8 && writer->drop_count() == 1; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 2u);
}

// written() counts the packet bytes in all fragments. It does not count
// chunk headers or size entries.
TEST_F(TraceWriterV2ImplTest, WrittenCountsPacketBytes) {
  CreateRingBufferArbiter(/*num_chunks=*/8);
  auto writer = CreateWriter();
  WritePacket(writer.get(), "first");
  const uint64_t written_before = writer->written();

  // Finalize() returns the size of the packet. The packet crosses chunks.
  uint32_t packet_size = 0;
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str(std::string(600, 'w'));
    packet_size = packet->Finalize();
  }
  EXPECT_GT(packet_size, 600u);
  EXPECT_EQ(writer->written() - written_before, packet_size);
}

// --- Destruction ---

TEST_F(TraceWriterV2ImplTest, DestructionPublishesThenReleasesWriterId) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto writer = CreateWriter();
  EXPECT_NE(writer->writer_id(), 0u);
  WritePacket(writer.get(), "last");

  // The live WriterID keeps the SMB arbiter, and so the endpoint, alive.
  EXPECT_FALSE(smb_arbiter_->TryShutdown());
  writer.reset();
  EXPECT_TRUE(smb_arbiter_->TryShutdown());

  // Attach a reader and drain to check that the destructor published the
  // packet before releasing the ID.
  AttachReader();
  Drain();
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "last");
}

}  // namespace
}  // namespace perfetto::tracing_v2
