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
#include <utility>
#include <vector>

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

class CountingDelegate : public TraceWriterV2::Delegate {
 public:
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
};

// Drains the ring from inside the notification. That is what the bridge does
// asynchronously on the relay sequence; doing it synchronously here is what
// makes a test about notification timing deterministic.
class DrainingDelegate : public TraceWriterV2::Delegate {
 public:
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
};

// Puts the writer's fragments back together into packets and rewrites them to
// canonical protobuf, which is what the relay will do in the next change. The
// only inputs are the two continuation flags and the fragment order, so this
// doubles as a check that the writer sets them.
class PacketReassembler : public SharedRingBufferReader::Delegate {
 public:
  void OnChunkRead(
      const SharedRingBufferReader::ChunkContents& contents) override {
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
      const SharedRingBufferReader::Fragment& fragment = contents.fragments[i];
      state.pending.insert(state.pending.end(), fragment.data,
                           fragment.data + fragment.size);
      if (!continues_on_next) {
        packets.push_back(std::move(state.pending));
        state.pending.clear();
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
  struct State {
    std::vector<uint8_t> pending;
  };
  std::map<WriterID, State> per_writer_;
};

struct Fixture {
  explicit Fixture(uint32_t num_chunks = 8,
                   uint32_t chunk_size = 256,
                   BufferExhaustedPolicy policy = BufferExhaustedPolicy::kDrop)
      : ring(num_chunks, chunk_size),
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

TEST(TraceWriterV2Test, WrittenIncludesOpenAndDroppedPacketBytes) {
  test::SharedRingBufferForTesting ring(/*num_chunks=*/1, /*chunk_size=*/256);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);

  TraceWriterV2::InitArgs args;
  args.ring_buffer = ring.get();
  args.delegate = std::make_shared<CountingDelegate>();
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

  // An empty chunk holds a 248-byte fragment. The two-byte opening tag for
  // TracePacket.for_testing, the string tag, its two-byte length and this
  // payload fill it exactly. Finalize() has to write the nested-message close
  // byte into another chunk.
  const std::string payload(243, 'z');
  auto packet = f.writer->NewTracePacket();
  packet->set_for_testing()->set_str(payload);
  f.writer->FinishTracePacket();
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
  auto delegate = std::make_shared<CountingDelegate>();

  // Both chunks pinned by a writer that stopped mid-rewrite, so no claim the
  // writer below makes can succeed.
  const uint32_t being_written =
      MakeDataStateWord(ChunkState::kBeingWritten, ChunkFormat::kTargetBuffer,
                        0, 0, kWriterA + 1);
  for (uint32_t i = 0; i < ring->num_chunks(); ++i) {
    ASSERT_TRUE(ring->TryAcquireChunkForWriting(i, being_written));
    uint32_t observed = being_written;
    ASSERT_TRUE(ring->TryRequestRewrite(i, &observed));
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
    ASSERT_EQ(ChunkStateOf(f.ring->LoadChunkStateWord(0)),
              ChunkState::kBeingWritten);
    EXPECT_EQ(Internals::ResolveNextPosition(&f.reader),
              SharedRingBufferReader::ResolveResult::kChunkRead);
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
  auto delegate = std::make_shared<CountingDelegate>();

  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);
  ASSERT_EQ(ring->TryReserveWritePos().result,
            SharedRingBuffer::ReserveResult::kReserved);

  TraceWriterV2::InitArgs args;
  args.ring_buffer = ring.get();
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
