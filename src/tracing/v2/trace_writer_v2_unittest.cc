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

#include "src/tracing/v2/trace_writer_v2.h"

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/chunk_reader.h"
#include "src/tracing/v2/proto_rewriter.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

constexpr WriterID kWriterA = 3;
constexpr BufferID kBufferA = 11;

class CountingDelegate : public TraceWriterV2::Delegate {
 public:
  void OnPacketsCommitted() override { ++commits; }
  uint32_t commits = 0;
};

// Drains the ring from inside the notification. That is what the bridge does
// asynchronously on the relay sequence; doing it synchronously here is what
// makes a test about notification timing deterministic.
class DrainingDelegate : public TraceWriterV2::Delegate {
 public:
  void OnPacketsCommitted() override {
    ++commits;
    if (reader)
      reader->Drain(1u << 20);
  }
  ChunkReader* reader = nullptr;
  uint32_t commits = 0;
};

// Puts the writer's fragments back together into packets and rewrites them to
// canonical protobuf, which is what the relay will do in the next change. The
// only inputs are the two continuation flags and the fragment order, so this
// doubles as a check that the writer sets them.
class PacketReassembler : public ChunkReader::Delegate {
 public:
  void OnChunkRead(const ChunkReader::ChunkContents& contents) override {
    State& state = per_writer_[contents.writer_id];
    if ((contents.payload_flags & kFlagDataLoss) != 0)
      ++data_loss_reports;

    for (uint32_t i = 0; i < contents.num_fragments; ++i) {
      const bool first = i == 0;
      const bool last = i + 1 == contents.num_fragments;
      const bool continues_from_prev =
          first && (contents.payload_flags & kFlagContinuesFromPrevChunk) != 0;
      const bool continues_on_next =
          last && (contents.payload_flags & kFlagContinuesOnNextChunk) != 0;

      if (!continues_from_prev) {
        // A fragment that does not continue anything starts a packet, so
        // anything still pending was never finished.
        EXPECT_TRUE(state.pending.empty())
            << "writer " << contents.writer_id << " left a packet unfinished";
        state.pending.clear();
      }
      const ChunkReader::Fragment& fragment = contents.fragments[i];
      state.pending.insert(state.pending.end(), fragment.data,
                           fragment.data + fragment.size);
      if (!continues_on_next) {
        packets.push_back(std::move(state.pending));
        state.pending.clear();
      }
    }
    target_buffers[contents.writer_id] = contents.target_buffer;
  }

  // Rewrites and decodes every reassembled packet.
  std::vector<protos::gen::TracePacket> Decode() const {
    std::vector<protos::gen::TracePacket> out;
    for (const std::vector<uint8_t>& packet : packets) {
      std::vector<uint8_t> canonical;
      EXPECT_TRUE(RewriteToLengthDelimitedProto(packet.data(),
                                                packet.data() + packet.size(),
                                                1024 * 1024, &canonical));
      protos::gen::TracePacket decoded;
      EXPECT_TRUE(decoded.ParseFromArray(canonical.data(), canonical.size()));
      out.push_back(std::move(decoded));
    }
    return out;
  }

  std::vector<std::vector<uint8_t>> packets;
  std::map<WriterID, BufferID> target_buffers;
  uint32_t data_loss_reports = 0;

 private:
  struct State {
    std::vector<uint8_t> pending;
  };
  std::map<WriterID, State> per_writer_;
};

struct Fixture {
  explicit Fixture(uint32_t num_chunks = 8,
                   uint32_t chunk_size = 256,
                   BufferExhaustedPolicy policy = BufferExhaustedPolicy::kDrop)
      : ring(SharedRingBuffer::Create(num_chunks, chunk_size)),
        delegate(std::make_shared<CountingDelegate>()),
        reader(ring.get(), &reassembler) {
    TraceWriterV2::InitArgs args;
    args.ring_buffer = ring.get();
    args.delegate = delegate;
    args.writer_id = kWriterA;
    args.target_buffer = kBufferA;
    args.buffer_exhausted_policy = policy;
    writer = std::make_unique<TraceWriterV2>(args);
  }

  std::unique_ptr<SharedRingBuffer> ring;
  std::shared_ptr<CountingDelegate> delegate;
  PacketReassembler reassembler;
  ChunkReader reader;
  std::unique_ptr<TraceWriterV2> writer;
};

TEST(TraceWriterV2Test, WritesAPacketThatRewritesToCanonicalProtobuf) {
  Fixture f;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(4242);
    packet->set_trusted_packet_sequence_id(7);
  }
  f.writer->Flush();
  f.reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 4242u);
  EXPECT_EQ(packets[0].trusted_packet_sequence_id(), 7u);
  EXPECT_EQ(f.reassembler.target_buffers[kWriterA], kBufferA);
  EXPECT_EQ(f.writer->writer_id(), kWriterA);
  EXPECT_GT(f.delegate->commits, 0u);
}

TEST(TraceWriterV2Test, NestedMessagesUseThePrivateFramingOnTheWire) {
  Fixture f;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(1);
    auto* test_event = packet->set_for_testing();
    test_event->set_str("outer");
    auto* payload = test_event->set_payload();
    payload->set_single_string("inner");
    payload->add_str("first");
    payload->add_str("second");
  }
  f.writer->Flush();
  f.reader.Drain(64);

  // On the wire the nested messages are opened with wire-type-3 tags and closed
  // with the bare terminator, so the raw fragment is not parseable protobuf.
  ASSERT_EQ(f.reassembler.packets.size(), 1u);
  const std::vector<uint8_t>& raw = f.reassembler.packets[0];
  EXPECT_NE(std::find(raw.begin(), raw.end(),
                      protozero::proto_utils::kNestedMessageTerminator),
            raw.end());
  protos::gen::TracePacket not_decoded;
  EXPECT_FALSE(not_decoded.ParseFromArray(raw.data(), raw.size()));

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  ASSERT_TRUE(packets[0].has_for_testing());
  EXPECT_EQ(packets[0].for_testing().str(), "outer");
  EXPECT_EQ(packets[0].for_testing().payload().single_string(), "inner");
  ASSERT_EQ(packets[0].for_testing().payload().str().size(), 2u);
  EXPECT_EQ(packets[0].for_testing().payload().str()[0], "first");
  EXPECT_EQ(packets[0].for_testing().payload().str()[1], "second");
}

TEST(TraceWriterV2Test, PacketSpanningSeveralChunksReconstructsExactly) {
  // 256-byte chunks give about 249 payload bytes, so a 2 KiB string has to be
  // split across many of them.
  Fixture f(/*num_chunks=*/64, /*chunk_size=*/256);
  const std::string payload(2048, 'x');
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(99);
    packet->set_for_testing()->set_str(payload);
  }
  f.writer->Flush();
  f.reader.Drain(1024);

  // The reassembler asserted the continuation flags line up; here we only have
  // to see one packet come out whole.
  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 99u);
  EXPECT_EQ(packets[0].for_testing().str(), payload);
  EXPECT_GT(f.reader.num_chunks_emitted(), 1u);
}

// A packet bigger than the entire ring. Every chunk the writer fills has to be
// drained before it can have the next one, so the writer has to say something
// happened at each fragment boundary rather than only when the packet ends.
// Without that the third chunk request finds a full ring: under kDrop the tail
// of the packet is thrown away, and under kStall the writer waits for a reader
// that was never told to run.
//
// kDrop deliberately, so that the failure is a wrong answer in milliseconds
// rather than a 30-second stall.
TEST(TraceWriterV2Test, PacketLargerThanTheWholeRingSurvives) {
  auto ring = SharedRingBuffer::Create(/*num_chunks=*/2, /*chunk_size=*/256);
  ASSERT_NE(ring, nullptr);
  PacketReassembler reassembler;
  ChunkReader reader(ring.get(), &reassembler);
  auto delegate = std::make_shared<DrainingDelegate>();
  delegate->reader = &reader;

  TraceWriterV2::InitArgs args;
  args.ring_buffer = ring.get();
  args.delegate = delegate;
  args.writer_id = kWriterA;
  args.target_buffer = kBufferA;
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kDrop;
  TraceWriterV2 writer(args);

  // Two 256-byte chunks hold about 500 payload bytes between them, so this
  // needs the ring around eight times over.
  const std::string payload(4096, 'y');
  {
    auto packet = writer.NewTracePacket();
    packet->set_timestamp(77);
    packet->set_for_testing()->set_str(payload);
  }
  writer.Flush();
  reader.Drain(1u << 20);

  const std::vector<protos::gen::TracePacket> packets = reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 77u);
  EXPECT_EQ(packets[0].for_testing().str(), payload);
  EXPECT_EQ(writer.drop_count(), 0u);
  EXPECT_EQ(reassembler.data_loss_reports, 0u);
  // The packet crossed the ring several times over, so the reader was told
  // about it many times, not once at the end.
  EXPECT_GT(delegate->commits, 8u);
}

// A packet is not finished when the ring runs out from under it, and a writer
// that burned positions has to say so straight away rather than when the packet
// ends: those positions count against every writer on the ring, so one of them
// holding a packet open must not be what keeps the reader from being told.
TEST(TraceWriterV2Test, AnAcquisitionThatBurnedPositionsNotifiesImmediately) {
  auto ring = SharedRingBuffer::Create(/*num_chunks=*/2, /*chunk_size=*/256);
  ASSERT_NE(ring, nullptr);
  PacketReassembler reassembler;
  ChunkReader reader(ring.get(), &reassembler);
  auto delegate = std::make_shared<CountingDelegate>();

  // Both chunks pinned by a writer that stopped mid-rewrite, so no claim the
  // writer below makes can succeed.
  const uint32_t acquired = MakeDataBearingWord(
      ChunkState::kAcquired, ChunkFormat::kTargetBuffer, 0, 0, kWriterA + 1);
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    ASSERT_TRUE(ring->TryClaim(i, acquired));
    uint32_t observed = acquired;
    ASSERT_TRUE(ring->TryMarkForRewrite(i, &observed));
  }

  TraceWriterV2::InitArgs args;
  args.ring_buffer = ring.get();
  args.delegate = delegate;
  args.writer_id = kWriterA;
  args.target_buffer = kBufferA;
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kDrop;
  TraceWriterV2 writer(args);

  TraceWriter::TracePacketHandle packet = writer.NewTracePacket();
  packet->set_timestamp(1);
  // Still open, and the reader has already been told.
  EXPECT_GT(delegate->commits, 0u);
  EXPECT_GT(ring->LoadWritePosRelaxed(), 0u);
}

// The other way a writer can find itself with nowhere to write: not a full ring
// but chunks pinned by somebody who stopped mid-rewrite. Every position this
// writer reserves maps onto one of them and cannot be claimed, so its own
// failed claims are what fill the ring.
//
// A stalling writer must not sleep on that. The positions it burned are ones
// only the reader can resolve, and the reader has not been told there is
// anything to do - so it has to come back and say so, and the next attempt has
// to make progress. With the drain wired to the notification, as the bridge
// wires it, this converges in a couple of rounds; without it the writer parks
// for the full stall timeout and aborts.
TEST(TraceWriterV2Test, StallingWriterRecoversFromChunksItCouldNotClaim) {
  auto ring = SharedRingBuffer::Create(/*num_chunks=*/2, /*chunk_size=*/256);
  ASSERT_NE(ring, nullptr);
  PacketReassembler reassembler;
  ChunkReader reader(ring.get(), &reassembler);
  auto delegate = std::make_shared<DrainingDelegate>();
  delegate->reader = &reader;

  // Both physical chunks pinned by another writer that stopped between the
  // reader's scrape and its own acknowledgement.
  constexpr WriterID kPinningWriter = kWriterA + 1;
  const uint32_t acquired = MakeDataBearingWord(
      ChunkState::kAcquired, ChunkFormat::kTargetBuffer, 0, 0, kPinningWriter);
  const uint32_t pinned = WithState(acquired, ChunkState::kRewriteRequested);
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    ASSERT_TRUE(ring->TryClaim(i, acquired));
    uint32_t observed = acquired;
    ASSERT_TRUE(ring->TryMarkForRewrite(i, &observed));
  }

  TraceWriterV2::InitArgs args;
  args.ring_buffer = ring.get();
  args.delegate = delegate;
  args.writer_id = kWriterA;
  args.target_buffer = kBufferA;
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kStall;
  TraceWriterV2 writer(args);

  // Round one. Nothing can be claimed, so this packet is lost - but the attempt
  // returns instead of blocking, and it tells the reader about the holes it
  // made, which is what lets the reader move its cursor past them.
  writer.NewTracePacket()->set_timestamp(1);
  writer.FinishTracePacket();
  EXPECT_GT(writer.drop_count(), 0u);
  EXPECT_GT(delegate->commits, 0u);

  // The pinned writer comes back and hands both chunks over.
  for (uint32_t i = 0; i < ring->num_chunks(); ++i)
    ASSERT_TRUE(ring->TryAcknowledge(i, pinned));

  // Round two. The chunks are Acknowledged, which only the reader may leave, so
  // these claims fail too - and this is the round that would deadlock: the ring
  // is now full of this writer's own holes. It returns, notifies, and the
  // reader reclaims both chunks.
  writer.NewTracePacket()->set_timestamp(2);
  writer.FinishTracePacket();

  // Round three lands.
  writer.NewTracePacket()->set_timestamp(3);
  writer.Flush();
  reader.Drain(1u << 20);

  const std::vector<protos::gen::TracePacket> packets = reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 3u);
  // The lost packets are accounted for on the chunk that carried this one.
  EXPECT_EQ(reassembler.data_loss_reports, 1u);
}

TEST(TraceWriterV2Test, ManySmallPacketsShareChunksAndKeepTheirOrder) {
  Fixture f(/*num_chunks=*/64, /*chunk_size=*/512);
  for (uint32_t i = 0; i < 200; ++i) {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(i);
  }
  f.writer->Flush();
  f.reader.Drain(1024);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 200u);
  for (uint32_t i = 0; i < 200; ++i)
    EXPECT_EQ(packets[i].timestamp(), i) << i;
  // Several packets shared a chunk: 200 packets did not need 200 positions.
  EXPECT_LT(f.ring->LoadWritePosRelaxed(), 200u);
}

TEST(TraceWriterV2Test, PacketsFromTwoWritersKeepTheirOwnBuffersAndIds) {
  Fixture f(/*num_chunks=*/16, /*chunk_size=*/256);
  TraceWriterV2::InitArgs args;
  args.ring_buffer = f.ring.get();
  args.delegate = f.delegate;
  args.writer_id = 9;
  args.target_buffer = 22;
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kDrop;
  TraceWriterV2 other(args);

  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(1);
  }
  f.writer->Flush();
  {
    auto packet = other.NewTracePacket();
    packet->set_timestamp(2);
  }
  other.Flush();
  f.reader.Drain(64);

  EXPECT_EQ(f.reassembler.target_buffers[kWriterA], kBufferA);
  EXPECT_EQ(f.reassembler.target_buffers[9], 22);
  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].timestamp(), 1u);
  EXPECT_EQ(packets[1].timestamp(), 2u);
}

// The reader takes the chunk while a packet is still being written. The writer
// relocates only the part it had not published, and the packet still comes out
// once and whole.
TEST(TraceWriterV2Test, ScrapeDuringAnOpenPacketProducesOneCanonicalPacket) {
  Fixture f(/*num_chunks=*/16, /*chunk_size=*/512);

  // A first packet so the chunk has a published prefix for the reader to take.
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(1);
  }

  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(2);
    // The reader arrives while this packet is still being written and takes
    // the prefix the writer has already published, which is the first packet.
    ASSERT_EQ(StateOf(f.ring->LoadStateAcquire(0)), ChunkState::kAcquired);
    EXPECT_EQ(f.reader.ReadOne(), ChunkReader::ReadOutcome::kEmitted);
    EXPECT_EQ(f.reader.num_scrapes(), 1u);
    packet->set_trusted_packet_sequence_id(5);
  }
  f.writer->Flush();
  f.reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].timestamp(), 1u);
  EXPECT_EQ(packets[1].timestamp(), 2u);
  EXPECT_EQ(packets[1].trusted_packet_sequence_id(), 5u);
}

TEST(TraceWriterV2Test, DropModeCountsDropsAndReportsTheGapAfterwards) {
  // Two chunks, both taken by other writers, so this writer gets nothing.
  Fixture f(/*num_chunks=*/2, /*chunk_size=*/256);
  ASSERT_EQ(f.ring->ReservePosition().outcome,
            SharedRingBuffer::ReserveOutcome::kReserved);
  ASSERT_EQ(f.ring->ReservePosition().outcome,
            SharedRingBuffer::ReserveOutcome::kReserved);

  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(1);
  }
  f.writer->Flush();
  EXPECT_EQ(f.writer->drop_count(), 1u);

  // Once the reader resolves those holes the writer gets in again, and the
  // chunk it publishes reports the gap.
  f.reader.Drain(64);
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(2);
  }
  f.writer->Flush();
  f.reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 2u);
  EXPECT_EQ(f.reassembler.data_loss_reports, 1u);
}

TEST(TraceWriterV2Test, DestructorPublishesTheLastPacket) {
  Fixture f;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(77);
  }
  f.writer.reset();
  f.reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 77u);
}

TEST(TraceWriterV2Test, FlushRunsItsCallbackInline) {
  Fixture f;
  bool called = false;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(1);
  }
  f.writer->Flush([&] { called = true; });
  EXPECT_TRUE(called);
}

}  // namespace
}  // namespace perfetto::tracing_v2
