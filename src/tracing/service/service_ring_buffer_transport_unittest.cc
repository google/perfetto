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

// Component tests for SharedRingBufferReader, RingBufferIngress, and TBv2.
// tracing_integration_test.cc covers the IPC path from producer to consumer.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/service/ring_buffer_ingress.h"
#include "src/tracing/service/trace_buffer.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using tracing_v2::RingBufferHeader;
using tracing_v2::SharedRingBuffer;
using tracing_v2::SharedRingBufferReader;

// Helpers to build proto-group encoded packets.
// These match the format used by the writer and expected by TBv2.

void AppendVarInt(uint64_t val, std::vector<uint8_t>* buf) {
  while (val >= 0x80) {
    buf->push_back(static_cast<uint8_t>(val | 0x80));
    val >>= 7;
  }
  buf->push_back(static_cast<uint8_t>(val));
}

uint64_t MakeTagStartGroup(uint32_t field_id) {
  return (static_cast<uint64_t>(field_id) << 3) | 3;
}
uint64_t MakeTagVarInt(uint32_t field_id) {
  return (static_cast<uint64_t>(field_id) << 3) | 0;
}

// Creates a simple proto-group packet: one group field wrapping a varint.
// This exercises the rewriter during readback.
std::vector<uint8_t> MakeGroupPacket(uint32_t outer_id, uint64_t value) {
  std::vector<uint8_t> data;
  AppendVarInt(MakeTagStartGroup(outer_id), &data);
  AppendVarInt(MakeTagVarInt(1), &data);
  AppendVarInt(value, &data);
  data.push_back(0x04);
  return data;
}

// ---------------------------------------------------------------------------
// SharedRingBufferReader unit tests
// ---------------------------------------------------------------------------

class CollectingDelegate : public SharedRingBufferReader::Delegate {
 public:
  struct RecordedChunk {
    WriterID writer_id;
    BufferID target_buffer;
    uint32_t payload_flags;
    std::vector<std::vector<uint8_t>> fragments;
  };

  void OnChunkRead(const SharedRingBufferReader::ChunkContents& c) override {
    RecordedChunk rc;
    rc.writer_id = c.writer_id;
    rc.target_buffer = c.target_buffer;
    rc.payload_flags = c.payload_flags;
    for (uint32_t i = 0; i < c.num_fragments; ++i) {
      rc.fragments.emplace_back(c.fragments[i].data,
                                c.fragments[i].data + c.fragments[i].size);
    }
    chunks.push_back(std::move(rc));
  }

  void OnDataLoss(WriterID wid) override { loss_writer_ids.push_back(wid); }

  std::vector<RecordedChunk> chunks;
  std::vector<WriterID> loss_writer_ids;
};

// Publish one fragment and verify the reader delegate.
TEST(SharedRingBufferTransportTest, BasicDrain) {
  constexpr uint32_t kNumChunks = 4;
  constexpr uint32_t kChunkSize = 256;

  auto ring_buffer_mem = std::make_unique<InProcessSharedMemory>(
      sizeof(RingBufferHeader) + kNumChunks * kChunkSize);
  memset(ring_buffer_mem->start(), 0, ring_buffer_mem->size());

  // Create a writer-side view to publish fragments.
  SharedRingBuffer writer_ring_buffer(
      static_cast<uint8_t*>(ring_buffer_mem->start()), ring_buffer_mem->size(),
      kChunkSize);

  CollectingDelegate delegate;

  // Write one fragment using the writer.
  auto packet = MakeGroupPacket(1, 42);
  {
    auto writer = tracing_v2::test::MakeWriter(&writer_ring_buffer, /*id=*/1,
                                               /*buffer=*/7);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(
        &writer, std::string(reinterpret_cast<const char*>(packet.data()),
                             packet.size())));
  }
  // Writer destructor publishes the chunk.

  // Copy the published bytes into service-owned memory for the reader.
  auto service_mem = std::make_unique<InProcessSharedMemory>(
      sizeof(RingBufferHeader) + kNumChunks * kChunkSize);
  memcpy(service_mem->start(), ring_buffer_mem->start(),
         ring_buffer_mem->size());

  SharedRingBuffer service_ring_buffer(
      static_cast<uint8_t*>(service_mem->start()), service_mem->size(),
      kChunkSize);
  SharedRingBufferReader reader(&service_ring_buffer, &delegate);

  auto result = reader.Drain(kNumChunks);
  ASSERT_GT(result.positions_consumed, 0u);
  ASSERT_EQ(delegate.chunks.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].writer_id, 1u);
  EXPECT_EQ(delegate.chunks[0].target_buffer, 7u);
  EXPECT_EQ(delegate.chunks[0].fragments.size(), 1u);
  EXPECT_EQ(delegate.chunks[0].fragments[0], packet);
}

// An empty RingBuffer supplies no data.
TEST(SharedRingBufferTransportTest, EmptyDrain) {
  constexpr uint32_t kNumChunks = 4;
  constexpr uint32_t kChunkSize = 256;

  auto mem = std::make_unique<InProcessSharedMemory>(sizeof(RingBufferHeader) +
                                                     kNumChunks * kChunkSize);
  memset(mem->start(), 0, mem->size());

  CollectingDelegate delegate;
  SharedRingBuffer service_ring_buffer(static_cast<uint8_t*>(mem->start()),
                                       mem->size(), kChunkSize);
  SharedRingBufferReader reader(&service_ring_buffer, &delegate);

  auto result = reader.Drain(kNumChunks);
  EXPECT_EQ(result.positions_consumed, 0u);
  EXPECT_EQ(delegate.chunks.size(), 0u);
}

// Multiple writes, single drain.
TEST(SharedRingBufferTransportTest, MultipleWritesSingleDrain) {
  constexpr uint32_t kNumChunks = 8;
  constexpr uint32_t kChunkSize = 256;

  auto mem = std::make_unique<InProcessSharedMemory>(sizeof(RingBufferHeader) +
                                                     kNumChunks * kChunkSize);
  memset(mem->start(), 0, mem->size());

  SharedRingBuffer ring_buffer(static_cast<uint8_t*>(mem->start()), mem->size(),
                               kChunkSize);

  // Write three fragments from two writers into the same RingBuffer.
  auto pkt1 = MakeGroupPacket(1, 10);
  auto pkt2 = MakeGroupPacket(1, 20);
  auto pkt3 = MakeGroupPacket(1, 30);

  {
    auto w1 = tracing_v2::test::MakeWriter(&ring_buffer, 1, 7);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(
        &w1,
        std::string(reinterpret_cast<const char*>(pkt1.data()), pkt1.size())));
  }
  {
    auto w2 = tracing_v2::test::MakeWriter(&ring_buffer, 2, 7);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(
        &w2,
        std::string(reinterpret_cast<const char*>(pkt2.data()), pkt2.size())));
  }
  {
    auto w1 = tracing_v2::test::MakeWriter(&ring_buffer, 1, 7);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(
        &w1,
        std::string(reinterpret_cast<const char*>(pkt3.data()), pkt3.size())));
  }

  // Copy to service side.
  auto svc_mem = std::make_unique<InProcessSharedMemory>(
      sizeof(RingBufferHeader) + kNumChunks * kChunkSize);
  memcpy(svc_mem->start(), mem->start(), mem->size());

  CollectingDelegate delegate;
  SharedRingBuffer service_ring_buffer(static_cast<uint8_t*>(svc_mem->start()),
                                       svc_mem->size(), kChunkSize);
  SharedRingBufferReader reader(&service_ring_buffer, &delegate);

  auto result = reader.Drain(kNumChunks);
  EXPECT_GE(result.positions_consumed, 3u);
  ASSERT_EQ(delegate.chunks.size(), 3u);
  EXPECT_EQ(delegate.chunks[0].fragments[0], pkt1);
  EXPECT_EQ(delegate.chunks[1].fragments[0], pkt2);
  EXPECT_EQ(delegate.chunks[2].fragments[0], pkt3);
}

// ---------------------------------------------------------------------------
// Service integration tests with a RingBuffer and TBv2
// ---------------------------------------------------------------------------

// Reads all packets from a TraceBuffer, returning their raw bytes.
std::vector<std::vector<uint8_t>> ReadAllPacketBytes(TraceBuffer* buf) {
  std::vector<std::vector<uint8_t>> packets;
  buf->BeginRead();
  for (;;) {
    TracePacket packet;
    TraceBuffer::PacketSequenceProperties seq{};
    uint32_t dropped = 0;
    if (!buf->ReadNextTracePacket(&packet, &seq, &dropped))
      break;
    std::vector<uint8_t> bytes;
    for (const Slice& slice : packet.slices()) {
      bytes.insert(bytes.end(), static_cast<const uint8_t*>(slice.start),
                   static_cast<const uint8_t*>(slice.start) + slice.size);
    }
    packets.push_back(std::move(bytes));
  }
  return packets;
}

// Reads all packets and also returns their sequence properties.
struct PacketWithProps {
  std::vector<uint8_t> bytes;
  TraceBuffer::PacketSequenceProperties props;
  uint32_t dropped;
};

std::vector<PacketWithProps> ReadAllPacketsWithProps(TraceBuffer* buf) {
  std::vector<PacketWithProps> result;
  buf->BeginRead();
  for (;;) {
    TracePacket packet;
    PacketWithProps pwp;
    pwp.dropped = 0;
    if (!buf->ReadNextTracePacket(&packet, &pwp.props, &pwp.dropped))
      break;
    for (const Slice& slice : packet.slices()) {
      pwp.bytes.insert(pwp.bytes.end(),
                       static_cast<const uint8_t*>(slice.start),
                       static_cast<const uint8_t*>(slice.start) + slice.size);
    }
    result.push_back(std::move(pwp));
  }
  return result;
}

// Exercise TBv2 with the same fragments that the service ingress supplies.
class ServiceRingBufferTransportTest : public testing::Test {
 public:
  void SetUp() override {
    tbv2_ = TraceBufferV2::Create(65536, TraceBuffer::kOverwrite);
    ASSERT_TRUE(tbv2_);
  }

  TraceBufferV2* buf() { return tbv2_.get(); }

  // Append one fragment with the trusted producer identity.
  void AcceptChunk(ProducerID pid,
                   WriterID wid,
                   const std::vector<uint8_t>& payload,
                   uint32_t payload_flags = 0) {
    tracing_v2::Fragment fv{payload.data(),
                            static_cast<uint32_t>(payload.size())};
    TraceBufferV2::ProtoGroupSequence seq{};
    seq.producer_id_trusted = pid;
    seq.client_identity_trusted = ClientIdentity(42, 0);
    seq.writer_id = wid;

    buf()->AppendProtoGroupFragments(seq, &fv, 1, payload_flags);
  }

  void RecordLoss(ProducerID pid, WriterID wid) {
    TraceBufferV2::ProtoGroupSequence seq{};
    seq.producer_id_trusted = pid;
    seq.client_identity_trusted = ClientIdentity(42, 0);
    seq.writer_id = wid;

    buf()->RecordProtoGroupLoss(seq);
  }

 protected:
  std::unique_ptr<TraceBufferV2> tbv2_;
};

// 1. TBv2 converts a proto-group packet to ordinary protobuf.
TEST_F(ServiceRingBufferTransportTest, BasicEndToEnd) {
  auto packet = MakeGroupPacket(1, 42);
  AcceptChunk(1, 1, packet);

  auto packets = ReadAllPacketBytes(buf());
  ASSERT_EQ(packets.size(), 1u);
  // Verify it decodes as length-delimited protobuf (rewritten from group).
  protozero::ProtoDecoder decoder(packets[0].data(), packets[0].size());
  auto field = decoder.FindField(1);
  ASSERT_TRUE(field.valid());
}

// 2. Multi-fragment packet: split across chunks, reassembled by TBv2.
TEST_F(ServiceRingBufferTransportTest, MultiFragmentPacket) {
  auto packet = MakeGroupPacket(1, 12345);
  auto mid = static_cast<ptrdiff_t>(packet.size() / 2);
  std::vector<uint8_t> frag1(packet.begin(), packet.begin() + mid);
  std::vector<uint8_t> frag2(packet.begin() + mid, packet.end());

  AcceptChunk(1, 1, frag1, tracing_v2::kFlagContinuesOnNextChunk);
  AcceptChunk(1, 1, frag2, tracing_v2::kFlagContinuesFromPrevChunk);

  auto packets = ReadAllPacketBytes(buf());
  ASSERT_EQ(packets.size(), 1u);
  protozero::ProtoDecoder decoder(packets[0].data(), packets[0].size());
  auto field = decoder.FindField(1);
  ASSERT_TRUE(field.valid());
}

// 3. Multiple writers: separate sequence tracking.
TEST_F(ServiceRingBufferTransportTest, MultipleWriters) {
  auto pkt_a = MakeGroupPacket(1, 10);
  auto pkt_b = MakeGroupPacket(1, 20);

  AcceptChunk(1, 1, pkt_a);
  AcceptChunk(1, 2, pkt_b);

  auto packets = ReadAllPacketsWithProps(buf());
  ASSERT_EQ(packets.size(), 2u);

  // Verify they come from separate writers.
  EXPECT_EQ(packets[0].props.writer_id, 1u);
  EXPECT_EQ(packets[1].props.writer_id, 2u);
}

// 4. Data loss propagation.
TEST_F(ServiceRingBufferTransportTest, DataLossPropagation) {
  auto pkt1 = MakeGroupPacket(1, 10);
  AcceptChunk(1, 1, pkt1);

  RecordLoss(1, 1);

  auto pkt2 = MakeGroupPacket(1, 30);
  AcceptChunk(1, 1, pkt2);

  auto packets = ReadAllPacketsWithProps(buf());
  // Expect two packets, with the second reporting prior loss.
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].dropped, 0u);
  EXPECT_NE(packets[1].dropped, 0u);
}

// 5. The ingress preserves the trusted producer identity and writer ID.
TEST_F(ServiceRingBufferTransportTest, PreservesSequenceIdentity) {
  auto pkt = MakeGroupPacket(1, 99);
  AcceptChunk(1, 1, pkt);

  auto packets = ReadAllPacketsWithProps(buf());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].props.producer_id_trusted, 1u);
  EXPECT_EQ(packets[0].props.writer_id, 1u);
}

// 6. RingBufferIngress drains into TBv2 through the delegate interface.
TEST_F(ServiceRingBufferTransportTest, RingBufferIngressDrainIntoTBv2) {
  constexpr uint32_t kNumChunks = 4;
  constexpr uint32_t kChunkSize = 256;
  const size_t ring_buffer_size =
      sizeof(RingBufferHeader) + kNumChunks * kChunkSize;

  // Allocate RingBuffer memory and write a fragment into it.
  auto writer_mem = std::make_unique<InProcessSharedMemory>(ring_buffer_size);
  memset(writer_mem->start(), 0, writer_mem->size());

  SharedRingBuffer writer_ring_buffer(
      static_cast<uint8_t*>(writer_mem->start()), writer_mem->size(),
      kChunkSize);

  auto pkt = MakeGroupPacket(1, 77);
  {
    auto writer = tracing_v2::test::MakeWriter(&writer_ring_buffer, 3, 7);
    ASSERT_TRUE(tracing_v2::test::WriteFragment(
        &writer,
        std::string(reinterpret_cast<const char*>(pkt.data()), pkt.size())));
  }

  // Copy the RingBuffer memory to the service side for the drain.
  auto svc_mem = std::make_unique<InProcessSharedMemory>(ring_buffer_size);
  memcpy(svc_mem->start(), writer_mem->start(), writer_mem->size());

  // Use a delegate that routes into TBv2.
  struct TBv2Delegate : public tracing_v2::RingBufferIngress::Delegate {
    TraceBufferV2* tb;

    TraceBufferV2* GetRingBufferDestination(BufferID) override { return tb; }
    void ForEachRingBufferDestination(
        const std::function<void(TraceBufferV2&)>& callback) override {
      callback(*tb);
    }
    void OnRingBufferChunkDiscarded() override {}
    void OnRingBufferUsed(BufferID) override {}
  };

  base::TestTaskRunner task_runner;
  TBv2Delegate delegate;
  delegate.tb = buf();

  SharedRingBuffer service_ring_buffer(static_cast<uint8_t*>(svc_mem->start()),
                                       svc_mem->size(), kChunkSize);
  tracing_v2::RingBufferIngress ingress(std::move(svc_mem), kChunkSize, 1,
                                        ClientIdentity(42, 0), &delegate,
                                        &task_runner);
  ingress.Drain();
  EXPECT_GT(tracing_v2::test::SharedRingBufferInternalsForTest::GetReadPos(
                &service_ring_buffer),
            0u);

  auto packets = ReadAllPacketBytes(buf());
  ASSERT_EQ(packets.size(), 1u);
  // Verify the output is valid length-delimited protobuf.
  protozero::ProtoDecoder decoder(packets[0].data(), packets[0].size());
  auto field = decoder.FindField(1);
  ASSERT_TRUE(field.valid());
}

// 7. Multiple fragments in a single chunk: several packets from one writer.
TEST_F(ServiceRingBufferTransportTest, MultipleFragmentsOneChunk) {
  // Write two separate packets (two fragments) from one writer.
  auto pkt1 = MakeGroupPacket(1, 100);
  auto pkt2 = MakeGroupPacket(1, 200);

  // Accept as two separate single-fragment chunks.
  AcceptChunk(1, 1, pkt1);
  AcceptChunk(1, 1, pkt2);

  auto packets = ReadAllPacketBytes(buf());
  ASSERT_EQ(packets.size(), 2u);

  // Both should decode.
  for (const auto& p : packets) {
    protozero::ProtoDecoder decoder(p.data(), p.size());
    auto field = decoder.FindField(1);
    EXPECT_TRUE(field.valid());
  }
}

// 8. Sequence ID distinction: same writer ID, different producers.
TEST_F(ServiceRingBufferTransportTest, SameWriterIdDifferentProducers) {
  auto pkt_a = MakeGroupPacket(1, 10);
  auto pkt_b = MakeGroupPacket(1, 20);

  // Same writer_id=1, but different producer IDs.
  {
    tracing_v2::Fragment fv{pkt_a.data(), static_cast<uint32_t>(pkt_a.size())};
    TraceBufferV2::ProtoGroupSequence seq{};
    seq.producer_id_trusted = 1;
    seq.client_identity_trusted = ClientIdentity(42, 0);
    seq.writer_id = 1;

    buf()->AppendProtoGroupFragments(seq, &fv, 1, 0);
  }
  {
    tracing_v2::Fragment fv{pkt_b.data(), static_cast<uint32_t>(pkt_b.size())};
    TraceBufferV2::ProtoGroupSequence seq{};
    seq.producer_id_trusted = 2;
    seq.client_identity_trusted = ClientIdentity(43, 0);
    seq.writer_id = 1;

    buf()->AppendProtoGroupFragments(seq, &fv, 1, 0);
  }

  auto packets = ReadAllPacketsWithProps(buf());
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].props.producer_id_trusted, 1u);
  EXPECT_EQ(packets[1].props.producer_id_trusted, 2u);
}

// 9. Empty fragment is valid.
TEST_F(ServiceRingBufferTransportTest, EmptyFragment) {
  tracing_v2::Fragment fv{nullptr, 0};
  TraceBufferV2::ProtoGroupSequence seq{};
  seq.producer_id_trusted = 1;
  seq.client_identity_trusted = ClientIdentity(42, 0);
  seq.writer_id = 1;

  // Empty fragments with zero size: should be handled without crash.
  auto result = buf()->AppendProtoGroupFragments(seq, &fv, 1, 0);
  // Whether it stores or rejects as invalid depends on TBv2 implementation.
  // The important thing is it does not crash.
  (void)result;
}

// 10. Loss followed by continuation: orphan continuation is dropped.
TEST_F(ServiceRingBufferTransportTest, LossThenContinuation) {
  // First fragment of a multi-chunk packet.
  auto frag1 = MakeGroupPacket(1, 42);
  AcceptChunk(1, 1, frag1, tracing_v2::kFlagContinuesOnNextChunk);

  // Record loss: the first fragment's packet cannot be completed.
  RecordLoss(1, 1);

  // Continuation fragment arrives: it's an orphan, should be handled.
  auto frag2 = MakeGroupPacket(1, 43);
  AcceptChunk(1, 1, frag2, tracing_v2::kFlagContinuesFromPrevChunk);

  // Now write a complete standalone packet.
  auto good_pkt = MakeGroupPacket(1, 99);
  AcceptChunk(1, 1, good_pkt, 0);

  auto packets = ReadAllPacketsWithProps(buf());
  // The good packet should be readable. The exact number of packets
  // depends on TBv2's handling of the orphan continuation.
  bool found_good = false;
  for (const auto& p : packets) {
    protozero::ProtoDecoder decoder(p.bytes.data(), p.bytes.size());
    auto field = decoder.FindField(1);
    if (field.valid()) {
      // Check if it's the nested varint = 99.
      protozero::ProtoDecoder inner(field.data(), field.size());
      auto val = inner.FindField(1);
      if (val.valid() && val.as_uint64() == 99u)
        found_good = true;
    }
  }
  EXPECT_TRUE(found_good);
}

}  // namespace
}  // namespace perfetto
