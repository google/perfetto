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

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/protozero/proto_utils.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/proto_rewriter.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

using Internals = test::SharedRingBufferInternalsForTest;

constexpr WriterID kWriterA = 3;
constexpr BufferID kBufferA = 11;

// Test stand-in for downstream acknowledgement, without a service.
class CountingDelegate : public TraceWriterV2::Delegate {
 public:
  explicit CountingDelegate(SharedRingBuffer& ring) : ring_buffer_(ring) {}

  SharedRingBuffer& ring_buffer() override { return ring_buffer_; }
  void NotifyReader() override { ++notifications; }
  void Flush(WriterID, std::function<void()> callback) override {
    ++flushes;
    if (callback)
      callback();
  }
  void OnWriterDestroyed(WriterID writer_id) override {
    destroyed_writers.push_back(writer_id);
  }
  uint32_t notifications = 0;
  uint32_t flushes = 0;
  std::vector<WriterID> destroyed_writers;

 private:
  SharedRingBuffer& ring_buffer_;
};

// Drains the ring synchronously from the notification to make tests about
// notification timing deterministic.
class DrainingDelegate : public TraceWriterV2::Delegate {
 public:
  explicit DrainingDelegate(SharedRingBuffer& ring) : ring_buffer_(ring) {}

  SharedRingBuffer& ring_buffer() override { return ring_buffer_; }
  void NotifyReader() override {
    ++notifications;
    if (reader)
      reader->Drain(1u << 20);
  }
  void Flush(WriterID, std::function<void()> callback) override {
    NotifyReader();
    if (callback)
      callback();
  }
  void OnWriterDestroyed(WriterID) override {}
  SharedRingBufferReader* reader = nullptr;
  uint32_t notifications = 0;

 private:
  SharedRingBuffer& ring_buffer_;
};

// Puts the writer's fragments back together into packets and rewrites them to
// canonical protobuf. The only inputs are the two continuation flags and the
// fragment order, so this checks that the writer sets them correctly.
class PacketReassembler : public SharedRingBufferReader::Delegate {
 public:
  void OnChunkRead(
      const SharedRingBufferReader::ChunkContents& contents) override {
    std::vector<uint8_t>& pending = per_writer_[contents.writer_id];
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
        EXPECT_TRUE(pending.empty())
            << "writer " << contents.writer_id << " left a packet unfinished";
        pending.clear();
      }
      const SharedRingBufferReader::Fragment& fragment = contents.fragments[i];
      pending.insert(pending.end(), fragment.data,
                     fragment.data + fragment.size);
      if (!continues_on_next) {
        packets.push_back(std::move(pending));
        pending.clear();
      }
    }
    target_buffers[contents.writer_id] = contents.target_buffer;
  }

  void OnDataLoss(WriterID) override { ++data_loss_reports; }

  // Rewrites and decodes every reassembled packet.
  std::vector<protos::gen::TracePacket> Decode() const {
    std::vector<protos::gen::TracePacket> out;
    for (const std::vector<uint8_t>& packet : packets) {
      std::vector<uint8_t> canonical;
      EXPECT_EQ(RewriteProtoGroupToLengthDelimited(
                    packet.data(), packet.data() + packet.size(), &canonical,
                    1024 * 1024),
                RewriteResult::kSuccess);
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
  std::map<WriterID, std::vector<uint8_t>> per_writer_;
};

struct Fixture {
  explicit Fixture(uint32_t num_chunks = 8,
                   uint32_t chunk_size = 256,
                   BufferExhaustedPolicy policy = BufferExhaustedPolicy::kDrop)
      : ring(num_chunks, chunk_size),
        delegate(std::make_shared<CountingDelegate>(*ring.get())),
        reader(ring.get(), &reassembler) {
    TraceWriterV2::InitArgs args;
    args.delegate = delegate;
    args.writer_id = kWriterA;
    args.target_buffer = kBufferA;
    args.buffer_exhausted_policy = policy;
    writer = std::make_unique<TraceWriterV2>(args);
  }

  test::SharedRingBufferForTesting ring;
  std::shared_ptr<CountingDelegate> delegate;
  PacketReassembler reassembler;
  SharedRingBufferReader reader;
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
  EXPECT_GT(f.delegate->notifications, 0u);
}

TEST(TraceWriterV2Test, MessageHandleDestructionPublishesThePacket) {
  Fixture f;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(4242);
  }

  EXPECT_GT(f.delegate->notifications, 0u);
  f.reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 4242u);
}

TEST(TraceWriterV2Test, DirectFinalizationThenFlushClosesFragmentOnce) {
  Fixture f;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(42);
    packet->Finalize();
    // Message finalization alone has not closed the ring fragment.
    EXPECT_EQ(f.delegate->notifications, 0u);
    f.writer->Flush();
    EXPECT_EQ(f.delegate->notifications, 1u);
    f.reader.Drain(64);
  }
  // Handle destruction must not publish the same fragment again.
  f.writer->Flush();
  f.reader.Drain(64);
  EXPECT_EQ(f.delegate->notifications, 1u);
  const auto packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 42u);
  EXPECT_EQ(f.writer->drop_count(), 0u);
}

TEST(TraceWriterV2Test, DirectFinalizationThenNewPacket) {
  Fixture f;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(1);
    packet->Finalize();
  }
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(2);
  }
  f.writer->Flush();
  f.reader.Drain(64);
  const auto packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].timestamp(), 1u);
  EXPECT_EQ(packets[1].timestamp(), 2u);
  EXPECT_EQ(f.delegate->notifications, 2u);
}

TEST(TraceWriterV2Test, RawStreamRequiresExplicitCompletion) {
  Fixture f;
  auto packet = f.writer->NewTracePacket();
  auto* stream = packet.TakeStreamWriter();
  stream->WriteByte(0x40);  // TracePacket.timestamp = 7.
  stream->WriteByte(0x07);
  EXPECT_EQ(f.delegate->notifications, 0u);
  f.writer->FinishTracePacket();
  f.writer->Flush();
  f.reader.Drain(64);
  const auto packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 7u);
  EXPECT_EQ(f.delegate->notifications, 1u);
}

TEST(TraceWriterV2Test, WrittenIncludesOpenAndDroppedPacketBytes) {
  test::SharedRingBufferForTesting ring(/*num_chunks=*/1, /*chunk_size=*/256);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);

  TraceWriterV2::InitArgs args;
  args.delegate = std::make_shared<CountingDelegate>(*ring.get());
  args.writer_id = kWriterA;
  args.target_buffer = kBufferA;
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kDrop;
  TraceWriterV2 writer(args);

  EXPECT_EQ(writer.written(), 0u);
  {
    auto packet = writer.NewTracePacket();
    const uint64_t before_field = writer.written();
    packet->set_timestamp(42);
    EXPECT_GT(writer.written(), before_field);
  }
  EXPECT_GT(writer.written(), 0u);
  EXPECT_EQ(writer.drop_count(), 1u);
}

TEST(TraceWriterV2Test, MarksOnlyTheFirstPacketOnSequence) {
  Fixture f;
  for (uint32_t timestamp : {1u, 2u}) {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(timestamp);
  }
  f.writer->Flush();
  f.reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_TRUE(packets[0].first_packet_on_sequence());
  EXPECT_FALSE(packets[1].first_packet_on_sequence());
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

  // Proto-group packets are not directly parseable as protobuf. They become
  // ordinary protobuf after rewriting.
  ASSERT_EQ(f.reassembler.packets.size(), 1u);
  const std::vector<uint8_t>& raw = f.reassembler.packets[0];
  EXPECT_NE(std::find(raw.begin(), raw.end(),
                      protozero::proto_utils::kProtoGroupEndByte),
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
  EXPECT_GT(f.reader.GetStats().chunks_read, 1u);
}

TEST(TraceWriterV2Test, FinalizingAPacketCanCrossAChunkBoundary) {
  Fixture f(/*num_chunks=*/8, /*chunk_size=*/256);

  // An empty chunk holds a 248-byte fragment. The three-byte first-packet
  // marker, two-byte opening tag for TracePacket.for_testing, string tag,
  // two-byte length and payload fill it exactly. Finalize() has to write the
  // nested-message close byte into another chunk.
  const std::string payload(240, 'z');
  auto packet = f.writer->NewTracePacket();
  packet->set_for_testing()->set_str(payload);
  ASSERT_EQ(f.ring->LoadWritePos(), 1u);
  f.writer->FinishTracePacket();
  EXPECT_EQ(f.ring->LoadWritePos(), 2u);
  f.writer->Flush();
  f.reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), payload);
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
  test::SharedRingBufferForTesting ring(/*num_chunks=*/2, /*chunk_size=*/256);
  PacketReassembler reassembler;
  SharedRingBufferReader reader(ring.get(), &reassembler);
  auto delegate = std::make_shared<DrainingDelegate>(*ring.get());
  delegate->reader = &reader;

  TraceWriterV2::InitArgs args;
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
  EXPECT_GT(delegate->notifications, 8u);
}

// A packet is not finished when the ring runs out from under it, and a writer
// that burned positions has to say so straight away rather than when the packet
// ends: those positions count against every writer on the ring, so one of them
// holding a packet open must not be what keeps the reader from being told.
TEST(TraceWriterV2Test, AnAcquisitionThatBurnedPositionsNotifiesImmediately) {
  test::SharedRingBufferForTesting ring(/*num_chunks=*/2, /*chunk_size=*/256);
  PacketReassembler reassembler;
  SharedRingBufferReader reader(ring.get(), &reassembler);
  auto delegate = std::make_shared<CountingDelegate>(*ring.get());

  // Both chunks pinned by a writer that stopped mid-rewrite, so no claim the
  // writer below makes can succeed.
  const uint32_t being_written =
      MakeDataStateWord(ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer,
                        0, 0, kWriterA + 1);
  for (uint32_t chunk_pos = 0; chunk_pos < ring->num_chunks(); ++chunk_pos) {
    ASSERT_TRUE(ring->TryAcquireChunkForWriting(chunk_pos, being_written));
    const auto chunk_idx =
        ChunkIndex::FromPosition(chunk_pos, ring->num_chunks());
    uint32_t observed = being_written;
    ASSERT_TRUE(ring->TryRequestRewrite(chunk_idx, &observed));
  }

  TraceWriterV2::InitArgs args;
  args.delegate = delegate;
  args.writer_id = kWriterA;
  args.target_buffer = kBufferA;
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kDrop;
  TraceWriterV2 writer(args);

  TraceWriter::TracePacketHandle packet = writer.NewTracePacket();
  packet->set_timestamp(1);
  // Still open, and the reader has already been told.
  EXPECT_GT(delegate->notifications, 0u);
  EXPECT_GT(ring->LoadWritePos(), 0u);
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
  EXPECT_LT(f.ring->LoadWritePos(), 200u);
}

TEST(TraceWriterV2Test, PacketsFromTwoWritersKeepTheirOwnBuffersAndIds) {
  Fixture f(/*num_chunks=*/16, /*chunk_size=*/256);
  TraceWriterV2::InitArgs args;
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
    ASSERT_EQ(
        ChunkStateOf(f.ring->LoadChunkStateWord(ChunkIndex::FromIndex(0))),
        ChunkState::kBeingWritten);
    EXPECT_EQ(Internals::ConsumeNextPosition(&f.reader),
              SharedRingBufferReader::ConsumeResult::kChunkRead);
    EXPECT_EQ(f.reader.GetStats().rewrite_requests, 1u);
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
  ASSERT_EQ(f.ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(f.ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);

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

TEST(TraceWriterV2Test, APartiallyDroppedPacketIsNotResumed) {
  test::SharedRingBufferForTesting ring(/*num_chunks=*/2, /*chunk_size=*/256);
  PacketReassembler reassembler;
  SharedRingBufferReader reader(ring.get(), &reassembler);
  auto delegate = std::make_shared<CountingDelegate>(*ring.get());

  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);

  TraceWriterV2::InitArgs args;
  args.delegate = delegate;
  args.writer_id = kWriterA;
  args.target_buffer = kBufferA;
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kDrop;
  TraceWriterV2 writer(args);

  {
    auto packet = writer.NewTracePacket();

    // The packet started in the drop buffer. Make ring capacity available
    // before the packet asks for its second buffer.
    reader.Drain(64);
    packet->set_for_testing()->set_str(std::string(2048, 'x'));
  }
  reader.Drain(64);
  EXPECT_TRUE(reassembler.packets.empty());

  {
    auto packet = writer.NewTracePacket();
    packet->set_timestamp(2);
  }
  writer.Flush();
  reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 2u);
  EXPECT_EQ(writer.drop_count(), 1u);
  EXPECT_EQ(reassembler.data_loss_reports, 1u);
}

TEST(TraceWriterV2Test, DropBufferSupportsContiguousStreamReservations) {
  Fixture f(/*num_chunks=*/1, /*chunk_size=*/4096);
  ASSERT_EQ(f.ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);

  auto packet = f.writer->NewTracePacket();
  auto* stream = packet.TakeStreamWriter();
  const uint64_t before = f.writer->written();
  // Direct stream users can reserve more than a scalar field. Each reservation
  // must fit in a fresh delegate buffer, including when the packet is dropped.
  for (size_t i = 0; i < 3; ++i) {
    uint8_t* reserved = stream->ReserveBytes(2048);
    ASSERT_GE(reserved, stream->cur_range().begin);
    ASSERT_LE(reserved + 2048, stream->cur_range().end);
    std::fill_n(reserved, 2048, 0);
  }
  EXPECT_EQ(f.writer->written() - before, 3u * 2048u);
  f.writer->FinishTracePacket();
  EXPECT_EQ(f.writer->drop_count(), 1u);

  f.reader.Drain(64);
  EXPECT_TRUE(f.reassembler.packets.empty());
  {
    auto recovered = f.writer->NewTracePacket();
    recovered->set_timestamp(42);
  }
  f.writer->Flush();
  f.reader.Drain(64);

  const auto packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 42u);
  EXPECT_EQ(f.writer->drop_count(), 1u);
  EXPECT_EQ(f.reassembler.data_loss_reports, 1u);
}

TEST(TraceWriterV2Test, DestructorPublishesTheLastPacket) {
  Fixture f;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(77);
  }
  f.writer.reset();
  ASSERT_EQ(f.delegate->destroyed_writers.size(), 1u);
  EXPECT_EQ(f.delegate->destroyed_writers[0], kWriterA);
  f.reader.Drain(64);

  const std::vector<protos::gen::TracePacket> packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 77u);
}

TEST(TraceWriterV2Test, RetainsDelegateUntilDestruction) {
  Fixture f;
  std::weak_ptr<CountingDelegate> delegate = f.delegate;
  f.delegate.reset();
  ASSERT_FALSE(delegate.expired());

  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(77);
  }
  f.writer->Flush();
  f.writer.reset();
  EXPECT_TRUE(delegate.expired());
  f.reader.Drain(64);

  const auto packets = f.reassembler.Decode();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 77u);
}

TEST(TraceWriterV2Test, FlushIsForwardedToTheDelegate) {
  Fixture f;
  bool called = false;
  {
    auto packet = f.writer->NewTracePacket();
    packet->set_timestamp(1);
  }
  f.writer->Flush([&] { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(f.delegate->flushes, 1u);
}

#if defined(GTEST_HAS_DEATH_TEST)

TEST(TraceWriterV2DeathTest, RawStreamRemainsUnfinishedWithoutPacketEnd) {
  Fixture f;
  auto packet = f.writer->NewTracePacket();
  auto* stream = packet.TakeStreamWriter();
  stream->WriteByte(0x40);
  stream->WriteByte(0x07);
  EXPECT_DEATH(f.writer->Flush(), "packet_open_");
  EXPECT_DEATH(f.writer->NewTracePacket(), "packet_open_");
  f.writer->FinishTracePacket();
}

TEST(TraceWriterV2DeathTest, StaleStreamWriteIsDiagnosed) {
  Fixture f;
  auto packet = f.writer->NewTracePacket();
  auto* stream = packet.TakeStreamWriter();
  f.writer->FinishTracePacket();
  f.writer->Flush();  // Clears the stream's cached range.
#if PERFETTO_DCHECK_IS_ON()
  EXPECT_DEATH(stream->WriteByte(0), "write outside an open packet");
#else
  stream->WriteByte(0);
  EXPECT_EQ(f.writer->drop_count(), 1u);
  auto next = f.writer->NewTracePacket();
  next->set_timestamp(42);
#endif
}

TEST(TraceWriterV2DeathTest, NewPacketRequiresThePreviousHandleToClose) {
  Fixture f;
  auto packet = f.writer->NewTracePacket();
  packet->set_timestamp(1);

  EXPECT_DEATH({ f.writer->NewTracePacket(); }, "");
}

TEST(TraceWriterV2DeathTest, FlushRequiresThePacketHandleToClose) {
  Fixture f;
  auto packet = f.writer->NewTracePacket();
  packet->set_timestamp(1);

  EXPECT_DEATH({ f.writer->Flush(); }, "");
}

#endif  // defined(GTEST_HAS_DEATH_TEST)

}  // namespace
}  // namespace perfetto::tracing_v2
