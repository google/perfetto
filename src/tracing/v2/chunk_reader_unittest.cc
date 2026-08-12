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

#include "src/tracing/v2/chunk_reader.h"

#include <string.h>

#include <algorithm>
#include <initializer_list>
#include <vector>

#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace tracing_v2 {
namespace {

struct PacketCopy {
  WriterID writer_id = 0;
  BufferID target_buffer = 0;
  uint32_t previous_packet_dropped = 0;
  std::vector<uint8_t> data;
};

ChunkHeader MakeHeader(WriterID writer_id,
                       BufferID target_buffer,
                       uint8_t flags = 0) {
  ChunkHeader header;
  header.writer_id = writer_id;
  header.target_buffer = target_buffer;
  header.flags = flags;
  return header;
}

void PublishRecords(SharedRingBuffer* ring,
                    const ChunkHeader& header,
                    std::initializer_list<std::vector<uint8_t>> records) {
  SharedRingBuffer::Chunk chunk = ring->TryAcquireChunkForWriting(header);
  ASSERT_TRUE(chunk.is_valid());
  size_t offset = 0;
  for (const auto& record : records) {
    ASSERT_LE(record.size(), kMaxFragmentSize);
    ASSERT_LE(offset + kFragmentHeaderSize + record.size(),
              static_cast<size_t>(kChunkPayloadSize));
    chunk.payload_begin()[offset++] = static_cast<uint8_t>(record.size());
    std::copy(record.begin(), record.end(), chunk.payload_begin() + offset);
    offset += record.size();
    chunk.AddFragment(static_cast<uint32_t>(offset));
  }
  ASSERT_TRUE(ring->ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
}

std::vector<PacketCopy> Drain(ChunkReader* reader) {
  std::vector<PacketCopy> packets;
  reader->Drain([&packets](const ChunkReader::Packet& packet) {
    packets.push_back(PacketCopy{
        packet.writer_id, packet.target_buffer, packet.previous_packet_dropped,
        std::vector<uint8_t>(packet.data, packet.data + packet.size)});
  });
  return packets;
}

TEST(ChunkReaderTest, EmitsMultipleExplicitlySizedPacketsIncludingEmpty) {
  SharedRingBuffer ring(8);
  ChunkReader reader(&ring);
  PublishRecords(&ring, MakeHeader(7, 3), {{0x08, 0x01}, {}, {0x10, 0x02}});

  const std::vector<PacketCopy> packets = Drain(&reader);
  ASSERT_EQ(packets.size(), 3u);
  EXPECT_EQ(packets[0].data, std::vector<uint8_t>({0x08, 0x01}));
  EXPECT_TRUE(packets[1].data.empty());
  EXPECT_EQ(packets[2].data, std::vector<uint8_t>({0x10, 0x02}));
  EXPECT_EQ(packets[0].writer_id, 7);
  EXPECT_EQ(packets[0].target_buffer, 3);
}

TEST(ChunkReaderTest, ReassemblesPacketAcrossThreeChunks) {
  SharedRingBuffer ring(8);
  ChunkReader reader(&ring);
  PublishRecords(&ring, MakeHeader(1, 4, kFlagContinuesOnNextChunk),
                 {{0x0a, 0x05}});
  PublishRecords(&ring,
                 MakeHeader(1, 4,
                            static_cast<uint8_t>(kFlagContinuesFromPrevChunk |
                                                 kFlagContinuesOnNextChunk)),
                 {{'h', 'e'}});
  PublishRecords(&ring, MakeHeader(1, 4, kFlagContinuesFromPrevChunk),
                 {{'l', 'l', 'o'}});

  const std::vector<PacketCopy> packets = Drain(&reader);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].data,
            std::vector<uint8_t>({0x0a, 0x05, 'h', 'e', 'l', 'l', 'o'}));
}

TEST(ChunkReaderTest, OrphanFragmentDoesNotHideLaterPacketInSameChunk) {
  SharedRingBuffer ring(4);
  ChunkReader reader(&ring);
  PublishRecords(&ring, MakeHeader(2, 5, kFlagContinuesFromPrevChunk),
                 {{0xff}, {0x08, 0x07}});

  const std::vector<PacketCopy> packets = Drain(&reader);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].data, std::vector<uint8_t>({0x08, 0x07}));
  EXPECT_TRUE(packets[0].previous_packet_dropped);
  EXPECT_EQ(reader.stats().orphan_fragments, 1u);
}

TEST(ChunkReaderTest, DataLossFlagIsMergedIntoNextPacket) {
  SharedRingBuffer ring(4);
  ChunkReader reader(&ring);
  PublishRecords(&ring, MakeHeader(3, 6, kFlagDataLoss), {{0x08, 0x01}});

  const std::vector<PacketCopy> packets = Drain(&reader);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_TRUE(packets[0].previous_packet_dropped);
}

// previous_packet_dropped is a bitmask whose reasons are defined to be ORed.
// A producer that set it itself must not have its reason replaced by ours, and
// exactly one occurrence may reach the trace.
TEST(ChunkReaderTest, MergesLossReasonsIntoOneCanonicalField) {
  SharedRingBuffer ring(4);
  ChunkReader reader(&ring);

  // A packet carrying previous_packet_dropped = DATA_LOSS_READ_GAP. Field 42,
  // varint: tag (42 << 3) | 0 = 336.
  const std::vector<uint8_t> packet_with_loss = {
      0xd0, 0x02,
      static_cast<uint8_t>(protos::pbzero::TracePacket::DATA_LOSS_READ_GAP)};
  // kFlagDataLoss on the chunk says the writer could not get one, i.e. the
  // ring was full.
  PublishRecords(&ring, MakeHeader(1, 2, kFlagDataLoss), {packet_with_loss});

  const std::vector<PacketCopy> packets = Drain(&reader);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(
      packets[0].previous_packet_dropped,
      static_cast<uint32_t>(protos::pbzero::TracePacket::DATA_LOSS_PRESENT |
                            protos::pbzero::TracePacket::DATA_LOSS_SMB_FULL |
                            protos::pbzero::TracePacket::DATA_LOSS_READ_GAP));
  // The producer's occurrence was hoisted out, so the caller emits exactly one.
  EXPECT_TRUE(packets[0].data.empty());
}

// A record reaching past the payload capacity is rejected inside
// SharedRingBuffer::TryReadChunk() - the walk that turns the fragment count
// into a byte boundary - so the reassembly layer never sees the chunk. What
// it does see is the writer's broken continuation chain, reported through the
// ordinary loss marking. The ring-level rejection has its own tests in
// shared_ring_buffer_unittest.cc.
TEST(ChunkReaderTest, SkipsAChunkTheRingRejectedAndMarksTheChainBroken) {
  SharedRingBuffer ring(4);
  ChunkReader reader(&ring);
  // First half of a packet, in a valid chunk.
  PublishRecords(&ring, MakeHeader(4, 7, kFlagContinuesOnNextChunk),
                 {{0x0a, 0x02}});
  // The continuation chunk carries a record claiming bytes past the capacity.
  SharedRingBuffer::Chunk chunk = ring.TryAcquireChunkForWriting(
      MakeHeader(4, 7, kFlagContinuesFromPrevChunk));
  ASSERT_TRUE(chunk.is_valid());
  chunk.payload_begin()[0] = 0xff;
  chunk.AddFragment(2);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));
  // The writer moves on to a self-contained packet.
  PublishRecords(&ring, MakeHeader(4, 7), {{0x08, 0x07}});

  const std::vector<PacketCopy> packets = Drain(&reader);
  EXPECT_EQ(ring.stats().malformed_chunks.load(), 1u);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].data, std::vector<uint8_t>({0x08, 0x07}));
  // The half-assembled packet was lost and the next one says so.
  EXPECT_TRUE(packets[0].previous_packet_dropped);
}

// Nothing zeroes a freed slot, so a chunk that fills only part of one is read
// against a payload whose tail still holds the previous lap's bytes. The
// counted records are the only thing that separates the two.
TEST(ChunkReaderTest, IgnoresBytesPastTheCountedRecords) {
  SharedRingBuffer ring(4);
  ChunkReader reader(&ring);
  SharedRingBuffer::Chunk chunk =
      ring.TryAcquireChunkForWriting(MakeHeader(5, 8));
  ASSERT_TRUE(chunk.is_valid());
  // Record-shaped garbage everywhere, then one real record in front of it.
  memset(chunk.payload_begin(), 0x01, kChunkPayloadSize);
  chunk.payload_begin()[0] = 2;
  chunk.payload_begin()[1] = 0x08;
  chunk.payload_begin()[2] = 0x01;
  chunk.AddFragment(3);
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&chunk, /*added_flags=*/0));

  const std::vector<PacketCopy> packets = Drain(&reader);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].data, std::vector<uint8_t>({0x08, 0x01}));
}

}  // namespace
}  // namespace tracing_v2
}  // namespace perfetto
