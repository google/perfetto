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

#include "src/tracing/service/tracing_v2_ingress.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using ::testing::ElementsAreArray;

// Ignores NotifyReader(); the test drives ingress Drain() explicitly.
class NoopWriterDelegate : public tracing_v2::SharedRingBufferWriter::Delegate {
 public:
  void NotifyReader() override {}
};

class TracingV2IngressTest : public testing::Test {
 protected:
  static constexpr uint32_t kNumChunks = 8;
  static constexpr uint32_t kChunkSize = 4096;

  TracingV2IngressTest() : ring_(kNumChunks, kChunkSize) {
    buffer_ = TraceBufferV2::Create(4096 * 16);
    PERFETTO_CHECK(buffer_);
  }

  // Builds an ingress routing every target buffer id in |allowed| to buffer_.
  std::unique_ptr<TracingV2Ingress> MakeIngress(
      std::vector<BufferID> allowed = {1}) {
    return std::make_unique<TracingV2Ingress>(
        ring_.get(), ProducerID(1), ClientIdentity(),
        [this, allowed](BufferID target) -> TraceBufferV2* {
          for (BufferID b : allowed) {
            if (b == target)
              return buffer_.get();
          }
          return nullptr;
        });
  }

  // Writes one whole-packet fragment carrying |bytes| via a ring writer.
  static void WriteWholePacket(tracing_v2::SharedRingBufferWriter* writer,
                               const std::vector<uint8_t>& bytes) {
    auto range = writer->BeginFragment(static_cast<uint32_t>(bytes.size()),
                                       /*continues_from_prev=*/false);
    ASSERT_EQ(
        range.result,
        tracing_v2::SharedRingBufferWriter::BeginFragmentResult::kSuccess);
    memcpy(range.begin, bytes.data(), bytes.size());
    ASSERT_EQ(writer->EndFragment(static_cast<uint32_t>(bytes.size()),
                                  /*continues_on_next=*/false),
              tracing_v2::SharedRingBufferWriter::EndFragmentResult::kSuccess);
  }

  // Reads one packet from buffer_, returning its concatenated bytes and, if
  // requested, its previous-packet-dropped flags and stored writer id.
  bool ReadPacketBytes(std::vector<uint8_t>* out,
                       uint32_t* dropped = nullptr,
                       WriterID* writer_id = nullptr) {
    TracePacket packet;
    TraceBuffer::PacketSequenceProperties psp{};
    uint32_t d = 0;
    if (!buffer_->ReadNextTracePacket(&packet, &psp, &d))
      return false;
    if (dropped)
      *dropped = d;
    if (writer_id)
      *writer_id = psp.writer_id;
    out->clear();
    for (const Slice& s : packet.slices()) {
      const auto* p = static_cast<const uint8_t*>(s.start);
      out->insert(out->end(), p, p + s.size);
    }
    return true;
  }

  tracing_v2::test::SharedRingBufferForTesting ring_;
  std::unique_ptr<TraceBufferV2> buffer_;
  NoopWriterDelegate writer_delegate_;
};

TEST_F(TracingV2IngressTest, WholePacketReachesBuffer) {
  auto ingress = MakeIngress();
  tracing_v2::SharedRingBufferWriter writer(
      ring_.get(), WriterID(1), BufferID(1), BufferExhaustedPolicy::kDrop,
      &writer_delegate_);
  const std::vector<uint8_t> pkt = {0x08, 0x2a};
  WriteWholePacket(&writer, pkt);

  EXPECT_FALSE(ingress->Drain());  // Nothing more to read.
  EXPECT_FALSE(ingress->has_protocol_error());

  buffer_->BeginRead();
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(ReadPacketBytes(&bytes));
  EXPECT_THAT(bytes, ElementsAreArray(pkt));
  EXPECT_FALSE(ReadPacketBytes(&bytes));
}

TEST_F(TracingV2IngressTest, NestedProtoGroupCanonicalizedEndToEnd) {
  auto ingress = MakeIngress();
  tracing_v2::SharedRingBufferWriter writer(
      ring_.get(), WriterID(1), BufferID(1), BufferExhaustedPolicy::kDrop,
      &writer_delegate_);
  // field 1 { field 2: 7 } in the append-only ProtoGroup encoding.
  WriteWholePacket(&writer, {0x0b, 0x10, 0x07, 0x04});

  ingress->Drain();

  buffer_->BeginRead();
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(ReadPacketBytes(&bytes));
  // Canonical length-delimited form (four-byte redundant length).
  EXPECT_THAT(bytes, ElementsAreArray<uint8_t>(
                         {0x0a, 0x82, 0x80, 0x80, 0x00, 0x10, 0x07}));
}

TEST_F(TracingV2IngressTest, MultipleWritersReadBackDisjoint) {
  auto ingress = MakeIngress();
  tracing_v2::SharedRingBufferWriter w1(ring_.get(), WriterID(1), BufferID(1),
                                        BufferExhaustedPolicy::kDrop,
                                        &writer_delegate_);
  tracing_v2::SharedRingBufferWriter w2(ring_.get(), WriterID(2), BufferID(1),
                                        BufferExhaustedPolicy::kDrop,
                                        &writer_delegate_);
  WriteWholePacket(&w1, {0x08, 0x01});
  WriteWholePacket(&w2, {0x08, 0x02});

  ingress->Drain();

  // Each writer forms its own stored sequence (v2 bit set), and its bytes stay
  // with it.
  const WriterID kW1 = static_cast<WriterID>(1 | (1u << 15));
  const WriterID kW2 = static_cast<WriterID>(2 | (1u << 15));
  buffer_->BeginRead();
  std::map<WriterID, std::vector<uint8_t>> by_writer;
  std::vector<uint8_t> bytes;
  WriterID wid = 0;
  while (ReadPacketBytes(&bytes, nullptr, &wid))
    by_writer[wid] = bytes;
  ASSERT_EQ(by_writer.size(), 2u);
  EXPECT_THAT(by_writer[kW1], ElementsAreArray<uint8_t>({0x08, 0x01}));
  EXPECT_THAT(by_writer[kW2], ElementsAreArray<uint8_t>({0x08, 0x02}));
}

TEST_F(TracingV2IngressTest, InvalidWireWriterIdDropped) {
  // The reader delegates a chunk whose (untrusted) writer id is out of the
  // public 15-bit range. Ingress must drop it without crashing or storing.
  auto ingress = MakeIngress();
  auto* delegate =
      static_cast<tracing_v2::SharedRingBufferReader::Delegate*>(ingress.get());
  const uint8_t frag[] = {0x08, 0x2a};
  tracing_v2::SharedRingBufferReader::Fragment f{frag, sizeof(frag)};
  tracing_v2::SharedRingBufferReader::ChunkContents c;
  c.writer_id = static_cast<WriterID>(0x8001);  // Bit 15 set: invalid wire id.
  c.target_buffer = BufferID(1);
  c.fragments = &f;
  c.num_fragments = 1;
  delegate->OnChunkRead(c);
  delegate->OnDataLoss(static_cast<WriterID>(0x8001));  // Also ignored.

  buffer_->BeginRead();
  std::vector<uint8_t> bytes;
  EXPECT_FALSE(ReadPacketBytes(&bytes));  // Nothing stored.
}

TEST_F(TracingV2IngressTest, UnpermittedTargetDropsChunk) {
  // The resolver permits only buffer 1, but the writer targets buffer 2.
  auto ingress = MakeIngress(/*allowed=*/{1});
  tracing_v2::SharedRingBufferWriter writer(
      ring_.get(), WriterID(1), BufferID(2), BufferExhaustedPolicy::kDrop,
      &writer_delegate_);
  WriteWholePacket(&writer, {0x08, 0x2a});

  ingress->Drain();
  EXPECT_FALSE(ingress->has_protocol_error());

  buffer_->BeginRead();
  std::vector<uint8_t> bytes;
  EXPECT_FALSE(ReadPacketBytes(&bytes));  // Dropped, nothing landed.
}

TEST_F(TracingV2IngressTest, PacketFragmentedAcrossChunksReassembled) {
  auto ingress = MakeIngress();
  tracing_v2::SharedRingBufferWriter writer(
      ring_.get(), WriterID(1), BufferID(1), BufferExhaustedPolicy::kDrop,
      &writer_delegate_);
  // Split one packet {08 2a 10 07} across two chunks via continuation flags.
  {
    auto range = writer.BeginFragment(2, /*continues_from_prev=*/false);
    ASSERT_EQ(
        range.result,
        tracing_v2::SharedRingBufferWriter::BeginFragmentResult::kSuccess);
    const uint8_t a[] = {0x08, 0x2a};
    memcpy(range.begin, a, sizeof(a));
    ASSERT_EQ(writer.EndFragment(2, /*continues_on_next=*/true),
              tracing_v2::SharedRingBufferWriter::EndFragmentResult::kSuccess);
  }
  {
    auto range = writer.BeginFragment(2, /*continues_from_prev=*/true);
    ASSERT_EQ(
        range.result,
        tracing_v2::SharedRingBufferWriter::BeginFragmentResult::kSuccess);
    const uint8_t b[] = {0x10, 0x07};
    memcpy(range.begin, b, sizeof(b));
    ASSERT_EQ(writer.EndFragment(2, /*continues_on_next=*/false),
              tracing_v2::SharedRingBufferWriter::EndFragmentResult::kSuccess);
  }

  ingress->Drain();

  buffer_->BeginRead();
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(ReadPacketBytes(&bytes));
  EXPECT_THAT(bytes, ElementsAreArray<uint8_t>({0x08, 0x2a, 0x10, 0x07}));
  EXPECT_FALSE(ReadPacketBytes(&bytes));
}

// A chunk denied by the target resolver must keep the writer's loss pending, so
// it surfaces on the next packet stored to a permitted buffer.
TEST_F(TracingV2IngressTest, FirstDeniedTargetReportsLossOnNextPacket) {
  bool allowed = false;
  TracingV2Ingress ingress(
      ring_.get(), ProducerID(1), ClientIdentity(),
      [this, &allowed](BufferID) { return allowed ? buffer_.get() : nullptr; });
  tracing_v2::SharedRingBufferWriter writer(
      ring_.get(), WriterID(1), BufferID(1), BufferExhaustedPolicy::kDrop,
      &writer_delegate_);
  WriteWholePacket(&writer, {0x08, 0x01});  // Denied (allowed == false).
  ingress.Drain();
  allowed = true;
  WriteWholePacket(&writer, {0x08, 0x02});  // Accepted.
  ingress.Drain();

  buffer_->BeginRead();
  std::vector<uint8_t> bytes;
  uint32_t dropped = 0;
  ASSERT_TRUE(ReadPacketBytes(&bytes, &dropped));
  EXPECT_THAT(bytes, ElementsAreArray<uint8_t>({0x08, 0x02}));
  EXPECT_NE(dropped, 0u);
}

// A chunk that TBv2 drops (larger than the destination buffer) must keep the
// pending loss, which then surfaces on the next stored packet.
TEST_F(TracingV2IngressTest, DroppedAdmissionKeepsPendingLoss) {
  buffer_ = TraceBufferV2::Create(4096);  // Small destination.
  tracing_v2::test::SharedRingBufferForTesting large_ring(8, 16384);
  TracingV2Ingress ingress(large_ring.get(), ProducerID(1), ClientIdentity(),
                           [this](BufferID) { return buffer_.get(); });
  auto* delegate =
      static_cast<tracing_v2::SharedRingBufferReader::Delegate*>(&ingress);
  delegate->OnDataLoss(WriterID(1));  // Loss before any stored chunk.

  tracing_v2::SharedRingBufferWriter writer(
      large_ring.get(), WriterID(1), BufferID(1), BufferExhaustedPolicy::kDrop,
      &writer_delegate_);
  // Valid flat protobuf, field 1 with 8192 bytes: fits the 16 KiB ring chunk
  // but not the 4 KiB destination, so TBv2 drops it.
  std::vector<uint8_t> large = {0x0a, 0x80, 0x40};
  large.insert(large.end(), 8192, 0x00);
  WriteWholePacket(&writer, large);
  ingress.Drain();
  ASSERT_EQ(buffer_->stats().chunks_discarded(), 1u);

  const std::vector<uint8_t> good = {0x08, 0x2a};
  WriteWholePacket(&writer, good);
  ingress.Drain();

  buffer_->BeginRead();
  std::vector<uint8_t> bytes;
  uint32_t dropped = 0;
  ASSERT_TRUE(ReadPacketBytes(&bytes, &dropped));
  EXPECT_THAT(bytes, ElementsAreArray(good));
  EXPECT_NE(dropped, 0u);
}

// A loss reported after the writer's earlier sequence state was reclaimed by GC
// must still surface on the next stored packet.
TEST_F(TracingV2IngressTest, LossAfterSequenceGcSurfaces) {
  auto ingress = MakeIngress();
  std::vector<uint8_t> bytes;
  // Fully consume enough other writers to reclaim writer 1's sequence state.
  for (uint16_t id = 1; id <= 1200; ++id) {
    tracing_v2::SharedRingBufferWriter writer(
        ring_.get(), WriterID(id), BufferID(1), BufferExhaustedPolicy::kDrop,
        &writer_delegate_);
    WriteWholePacket(&writer, {0x08, 0x01});
    ingress->Drain();
    buffer_->BeginRead();
    ASSERT_TRUE(ReadPacketBytes(&bytes));
    ASSERT_FALSE(ReadPacketBytes(&bytes));
  }
  auto* delegate =
      static_cast<tracing_v2::SharedRingBufferReader::Delegate*>(ingress.get());
  delegate->OnDataLoss(WriterID(1));
  tracing_v2::SharedRingBufferWriter writer(
      ring_.get(), WriterID(1), BufferID(1), BufferExhaustedPolicy::kDrop,
      &writer_delegate_);
  WriteWholePacket(&writer, {0x08, 0x2a});
  ingress->Drain();

  buffer_->BeginRead();
  uint32_t dropped = 0;
  ASSERT_TRUE(ReadPacketBytes(&bytes, &dropped));
  EXPECT_THAT(bytes, ElementsAreArray<uint8_t>({0x08, 0x2a}));
  EXPECT_NE(dropped, 0u);
}

}  // namespace
}  // namespace perfetto
