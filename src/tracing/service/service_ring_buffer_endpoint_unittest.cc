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

#include "src/tracing/service/service_ring_buffer_endpoint.h"

#include <string.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::tracing_v2 {

namespace test {

// Reaches the reader of a ServiceRingBufferEndpoint.
class ServiceRingBufferEndpointTestPeer {
 public:
  static SharedRingBufferReader* reader(ServiceRingBufferEndpoint* endpoint) {
    return &endpoint->reader_;
  }
  static bool retry_scheduled(const ServiceRingBufferEndpoint& endpoint) {
    return endpoint.retry_scheduled_;
  }
};

}  // namespace test

namespace {

using Internals = test::SharedRingBufferInternalsForTest;
using test::MakeWriter;
using test::WriteFragment;

constexpr uint32_t kNumChunks = 4;
constexpr uint32_t kChunkSize = 256;
constexpr ProducerID kProducer = 42;
constexpr WriterID kWriter = 3;
constexpr BufferID kBuffer = 7;
constexpr BufferID kForbiddenBuffer = 9;

// A TracePacket with for_testing.str = |value|, in the proto-group encoding
// that ring buffer writers emit. TraceBufferV2 rewrites it on readback.
std::string Packet(const std::string& value) {
  return std::string("\xa3\x38\x0a") + static_cast<char>(value.size()) + value +
         '\x04';
}

// Stands in for ProducerEndpointImpl. It authorizes one destination.
class FakeDelegate : public ServiceRingBufferEndpoint::Delegate {
 public:
  TraceBufferV2* GetRingBufferDestination(BufferID id) override {
    return id == kBuffer ? buffer.get() : nullptr;
  }
  void ForEachRingBufferDestination(
      const std::function<void(TraceBufferV2&)>& callback) override {
    callback(*buffer);
  }
  void OnRingBufferChunksDiscarded(uint64_t count) override {
    chunks_discarded += count;
  }

  std::unique_ptr<TraceBufferV2> buffer =
      TraceBufferV2::Create(64 * 1024, TraceBuffer::kOverwrite);
  uint64_t chunks_discarded = 0;
};

struct ReadPacket {
  std::string str;
  ProducerID producer_id = 0;
  WriterID writer_id = 0;
  bool previous_packet_dropped = false;
};

class ServiceRingBufferEndpointTest : public testing::Test {
 protected:
  ServiceRingBufferEndpointTest() {
    auto memory = std::make_unique<InProcessSharedMemory>(
        sizeof(RingBufferHeader) + kNumChunks * kChunkSize);
    memset(memory->start(), 0, memory->size());
    // The writer view and the endpoint's view share the same bytes, as the
    // producer and the service do.
    ring_buffer_ = std::make_unique<SharedRingBuffer>(
        static_cast<uint8_t*>(memory->start()), memory->size(), kChunkSize);
    endpoint_ = std::make_unique<ServiceRingBufferEndpoint>(
        std::move(memory), kChunkSize, kProducer, ClientIdentity(1000, 1001),
        &delegate_, &task_runner_);
  }

  std::vector<ReadPacket> ReadPackets() {
    std::vector<ReadPacket> packets;
    TraceBufferV2* buffer = delegate_.buffer.get();
    buffer->BeginRead();
    for (;;) {
      TracePacket packet;
      TraceBuffer::PacketSequenceProperties sequence{};
      uint32_t dropped = 0;
      if (!buffer->ReadNextTracePacket(&packet, &sequence, &dropped))
        break;
      protos::gen::TracePacket decoded;
      EXPECT_TRUE(decoded.ParseFromString(packet.GetRawBytesForTesting()));
      packets.push_back({decoded.for_testing().str(),
                         sequence.producer_id_trusted, sequence.writer_id,
                         dropped != 0});
    }
    return packets;
  }

  uint32_t ReadPos() { return Internals::GetReadPos(ring_buffer_.get()); }

  base::TestTaskRunner task_runner_;
  FakeDelegate delegate_;
  std::unique_ptr<SharedRingBuffer> ring_buffer_;
  std::unique_ptr<ServiceRingBufferEndpoint> endpoint_;
};

TEST_F(ServiceRingBufferEndpointTest, DrainsIntoDestination) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("first")));
  ASSERT_TRUE(WriteFragment(&writer, Packet("second")));
  writer.FinishCurrentChunk();

  endpoint_->Drain();

  EXPECT_EQ(ReadPos(), 1u);
  EXPECT_EQ(delegate_.chunks_discarded, 0u);
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "first");
  EXPECT_EQ(packets[1].str, "second");
  for (const ReadPacket& packet : packets) {
    // The endpoint stamps its own trusted producer ID.
    EXPECT_EQ(packet.producer_id, kProducer);
    EXPECT_EQ(packet.writer_id, kWriter);
    EXPECT_FALSE(packet.previous_packet_dropped);
  }
}

TEST_F(ServiceRingBufferEndpointTest, DiscardsInvalidWriterIds) {
  // Writer ID 0 and IDs above kMaxWriterID fit in the state word, but no
  // producer can own them.
  for (WriterID writer_id :
       {WriterID{0}, static_cast<WriterID>(kMaxWriterID + 1)}) {
    const uint32_t chunk_pos = ring_buffer_->TryReserveWritePos().chunk_pos;
    Internals::SetChunkStateWord(
        ring_buffer_.get(), ChunkIndex::FromPosition(chunk_pos, kNumChunks),
        MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                          1, writer_id));
  }

  endpoint_->Drain();

  EXPECT_EQ(ReadPos(), 2u);
  EXPECT_EQ(delegate_.chunks_discarded, 2u);
  EXPECT_TRUE(ReadPackets().empty());
}

TEST_F(ServiceRingBufferEndpointTest, ForbiddenDestinationRecordsLoss) {
  SharedRingBufferWriter allowed =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&allowed, Packet("before")));
  allowed.FinishCurrentChunk();
  SharedRingBufferWriter forbidden =
      MakeWriter(ring_buffer_.get(), kWriter, kForbiddenBuffer);
  ASSERT_TRUE(WriteFragment(&forbidden, Packet("forbidden")));
  forbidden.FinishCurrentChunk();
  ASSERT_TRUE(WriteFragment(&allowed, Packet("after")));
  allowed.FinishCurrentChunk();

  endpoint_->Drain();

  // The service drops the chunk before any trace buffer sees it. The
  // writer's sequence in the allowed buffer shows the gap.
  EXPECT_EQ(delegate_.chunks_discarded, 1u);
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "before");
  EXPECT_FALSE(packets[0].previous_packet_dropped);
  EXPECT_EQ(packets[1].str, "after");
  EXPECT_TRUE(packets[1].previous_packet_dropped);
}

TEST_F(ServiceRingBufferEndpointTest, ProducerLossIsNotAServiceDiscard) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("before")));
  writer.FinishCurrentChunk();
  endpoint_->Drain();

  // The writer flags its next chunk. The reader drops that chunk's fragments.
  writer.RecordDataLoss();
  ASSERT_TRUE(WriteFragment(&writer, Packet("lost")));
  writer.FinishCurrentChunk();
  endpoint_->Drain();

  ASSERT_TRUE(WriteFragment(&writer, Packet("after")));
  writer.FinishCurrentChunk();
  endpoint_->Drain();

  EXPECT_EQ(delegate_.chunks_discarded, 0u);
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "before");
  EXPECT_EQ(packets[1].str, "after");
  EXPECT_TRUE(packets[1].previous_packet_dropped);
}

TEST_F(ServiceRingBufferEndpointTest, MalformedAndUnknownChunksAreDiscards) {
  const struct {
    ChunkFormat format;
    uint32_t num_fragments;
  } kChunks[] = {
      // 255 one-byte sizes do not fit in a 256-byte chunk.
      {ChunkFormat::kTargetBuffer, 255},
      {ChunkFormat::kReservedRouting, 1},
  };
  for (const auto& chunk : kChunks) {
    const uint32_t chunk_pos = ring_buffer_->TryReserveWritePos().chunk_pos;
    Internals::SetChunkStateWord(
        ring_buffer_.get(), ChunkIndex::FromPosition(chunk_pos, kNumChunks),
        MakeDataStateWord(ChunkState::kComplete, chunk.format, 0,
                          chunk.num_fragments, kWriter));
  }

  endpoint_->Drain();

  EXPECT_EQ(ReadPos(), 2u);
  EXPECT_EQ(delegate_.chunks_discarded, 2u);
  EXPECT_EQ(delegate_.buffer->stats().abi_violations(), 0u);
}

TEST_F(ServiceRingBufferEndpointTest, ProtocolErrorIsAnAbiViolation) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("corrupt")));
  writer.FinishCurrentChunk();
  Internals::SetChunkStateWord(ring_buffer_.get(), ChunkIndex::FromIndex(0),
                               static_cast<uint32_t>(ChunkState::kReserved5));

  endpoint_->Drain();
  EXPECT_EQ(delegate_.buffer->stats().abi_violations(), 1u);

  // The reader stays stopped. Later drains read and count nothing.
  ASSERT_TRUE(WriteFragment(&writer, Packet("ignored")));
  writer.FinishCurrentChunk();
  endpoint_->Drain();
  task_runner_.RunUntilIdle();
  EXPECT_EQ(ReadPos(), 0u);
  EXPECT_EQ(delegate_.buffer->stats().abi_violations(), 1u);
  EXPECT_TRUE(ReadPackets().empty());
}

TEST_F(ServiceRingBufferEndpointTest, LostRacesPostOneDelayedRetry) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("first")));
  writer.FinishCurrentChunk();
  ASSERT_TRUE(WriteFragment(&writer, Packet("second")));
  writer.FinishCurrentChunk();

  // At position 1, flip the chunk between Complete and BeingWritten before
  // each reader transition. The reader loses every attempt at that position.
  SharedRingBufferReader* reader =
      test::ServiceRingBufferEndpointTestPeer::reader(endpoint_.get());
  Internals::SetBeforeStateTransitionCallback(reader, [&] {
    if (reader->read_pos() == 0)
      return;
    const ChunkIndex chunk_idx = ChunkIndex::FromIndex(1);
    uint32_t word = ring_buffer_->LoadChunkStateWordAcquire(chunk_idx);
    if (ChunkStateOf(word) == ChunkState::kComplete) {
      ASSERT_TRUE(ring_buffer_->TryReacquireChunkForWriting(chunk_idx, word));
    } else {
      ASSERT_TRUE(ring_buffer_->TryReleaseChunkAsComplete(
          chunk_idx, ReplaceChunkState(word, ChunkState::kComplete), &word));
    }
  });

  endpoint_->Drain();
  endpoint_->Drain();
  EXPECT_EQ(ReadPos(), 1u);
  // Both drains stopped early. The second one found the retry pending.
  EXPECT_TRUE(
      test::ServiceRingBufferEndpointTestPeer::retry_scheduled(*endpoint_));

  // The retry runs after the retry delay and consumes the position.
  Internals::SetBeforeStateTransitionCallback(reader, {});
  task_runner_.AdvanceTimeAndRunUntilIdle(1);
  EXPECT_EQ(ReadPos(), 2u);
  EXPECT_FALSE(
      test::ServiceRingBufferEndpointTestPeer::retry_scheduled(*endpoint_));

  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "first");
  EXPECT_EQ(packets[1].str, "second");
}

TEST_F(ServiceRingBufferEndpointTest, DestructionCancelsRetry) {
  {
    SharedRingBufferWriter writer =
        MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
    ASSERT_TRUE(WriteFragment(&writer, Packet("first")));
    writer.FinishCurrentChunk();
  }
  SharedRingBufferReader* reader =
      test::ServiceRingBufferEndpointTestPeer::reader(endpoint_.get());
  Internals::SetBeforeStateTransitionCallback(reader, [&] {
    const ChunkIndex chunk_idx = ChunkIndex::FromIndex(0);
    uint32_t word = ring_buffer_->LoadChunkStateWordAcquire(chunk_idx);
    if (ChunkStateOf(word) == ChunkState::kComplete) {
      ASSERT_TRUE(ring_buffer_->TryReacquireChunkForWriting(chunk_idx, word));
    } else {
      ASSERT_TRUE(ring_buffer_->TryReleaseChunkAsComplete(
          chunk_idx, ReplaceChunkState(word, ChunkState::kComplete), &word));
    }
  });
  endpoint_->Drain();
  EXPECT_EQ(ReadPos(), 0u);

  // The endpoint owns the mapping, so the writer view is invalid after this.
  // The pending retry must not run. ASan reports it if it does.
  ring_buffer_.reset();
  endpoint_.reset();
  task_runner_.AdvanceTimeAndRunUntilIdle(1);
}

}  // namespace
}  // namespace perfetto::tracing_v2
