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

#include "src/tracing/service/service_ring_buffer_drainer.h"

#include <string.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/service/clock.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/perfetto/tracing_v2_ring_buffer_dump.gen.h"
#include "protos/perfetto/trace/perfetto/tracing_v2_ring_buffer_dump.pbzero.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::tracing_v2 {

namespace test {

// Reaches the reader of a ServiceRingBufferDrainer.
class ServiceRingBufferDrainerTestPeer {
 public:
  static SharedRingBufferReader* reader(ServiceRingBufferDrainer* drainer) {
    return &drainer->reader_;
  }
  static bool retry_scheduled(const ServiceRingBufferDrainer& drainer) {
    return drainer.retry_scheduled_;
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
class FakeDelegate : public ServiceRingBufferDrainer::Delegate {
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
  void OnRingBufferProtocolError() override { ++protocol_errors; }
  void OnRingBufferChunkRejected(
      const SharedRingBufferReader::ChunkRejection& rejection) override {
    rejections.push_back(rejection);
  }

  std::unique_ptr<TraceBufferV2> buffer =
      TraceBufferV2::Create(64 * 1024, TraceBuffer::kOverwrite);
  uint64_t chunks_discarded = 0;
  uint32_t protocol_errors = 0;
  std::vector<SharedRingBufferReader::ChunkRejection> rejections;
};

struct ReadPacket {
  std::string str;
  ProducerID producer_id = 0;
  WriterID writer_id = 0;
  bool previous_packet_dropped = false;
};

class ServiceRingBufferDrainerTest : public testing::Test {
 protected:
  ServiceRingBufferDrainerTest() {
    auto memory = std::make_unique<InProcessSharedMemory>(
        sizeof(RingBufferHeader) + kNumChunks * kChunkSize);
    memset(memory->start(), 0, memory->size());
    // The writer view and the drainer's view share the same bytes, as the
    // producer and the service do.
    ring_buffer_ = std::make_unique<SharedRingBuffer>(
        static_cast<uint8_t*>(memory->start()), memory->size(), kChunkSize);
    drainer_ = std::make_unique<ServiceRingBufferDrainer>(
        std::move(memory), kChunkSize, kProducer, ClientIdentity(1000, 1001),
        &delegate_, &clock_, &task_runner_);
  }

  // Serializes a dump of the ring buffer within |max_bytes|.
  std::optional<protos::gen::TracingV2RingBufferDump> WriteDump(
      bool include_chunk_bytes,
      std::optional<uint32_t> only_chunk_pos,
      size_t max_bytes) {
    protozero::HeapBuffered<protos::pbzero::TracingV2RingBufferDump> raw;
    if (!drainer_->WriteDump(raw.get(), include_chunk_bytes, only_chunk_pos,
                             max_bytes)) {
      return std::nullopt;
    }
    const std::string serialized = raw.SerializeAsString();
    EXPECT_LE(serialized.size(), max_bytes);
    protos::gen::TracingV2RingBufferDump dump;
    EXPECT_TRUE(dump.ParseFromString(serialized));
    return dump;
  }

  // Each delivered chunk has exactly one admission outcome.
  void ExpectEveryDeliveredChunkAccounted() {
    const auto& stats = drainer_->stats();
    EXPECT_EQ(drainer_->reader_stats().chunks_read,
              stats.chunks_admitted + stats.invalid_writer_chunks +
                  stats.invalid_destination_chunks +
                  stats.trace_buffer_rejected_buffer_full +
                  stats.trace_buffer_rejected_format_conflict +
                  stats.trace_buffer_rejected_protovm +
                  stats.trace_buffer_rejected_invalid);
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
  tracing_service::ClockImpl clock_;
  FakeDelegate delegate_;
  std::unique_ptr<SharedRingBuffer> ring_buffer_;
  std::unique_ptr<ServiceRingBufferDrainer> drainer_;
};

TEST_F(ServiceRingBufferDrainerTest, DrainsIntoDestination) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("first")));
  ASSERT_TRUE(WriteFragment(&writer, Packet("second")));
  writer.FinishCurrentChunk();

  drainer_->Drain();

  EXPECT_EQ(ReadPos(), 1u);
  EXPECT_EQ(delegate_.chunks_discarded, 0u);
  EXPECT_EQ(drainer_->stats().chunks_admitted, 1u);
  // The fragment payloads only, without the size directory.
  EXPECT_EQ(drainer_->stats().admitted_payload_bytes,
            Packet("first").size() + Packet("second").size());
  ExpectEveryDeliveredChunkAccounted();
  // The pass consumed the only position and caught up with write_pos.
  EXPECT_EQ(drainer_->stats().drain_passes, 1u);
  EXPECT_EQ(drainer_->stats().drain_passes_without_progress, 0u);
  EXPECT_GT(drainer_->last_drain().time_ns, 0);
  EXPECT_EQ(drainer_->last_drain().progress_time_ns,
            drainer_->last_drain().time_ns);
  EXPECT_EQ(drainer_->last_drain().positions_consumed, 1u);
  EXPECT_EQ(drainer_->last_drain().result,
            SharedRingBufferReader::ConsumeResult::kNoData);
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "first");
  EXPECT_EQ(packets[1].str, "second");
  for (const ReadPacket& packet : packets) {
    // The drainer stamps its own trusted producer ID.
    EXPECT_EQ(packet.producer_id, kProducer);
    EXPECT_EQ(packet.writer_id, kWriter);
    EXPECT_FALSE(packet.previous_packet_dropped);
  }
}

TEST_F(ServiceRingBufferDrainerTest, DiscardsInvalidWriterIds) {
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

  drainer_->Drain();

  EXPECT_EQ(ReadPos(), 2u);
  EXPECT_EQ(delegate_.chunks_discarded, 2u);
  EXPECT_EQ(drainer_->stats().invalid_writer_chunks, 2u);
  ExpectEveryDeliveredChunkAccounted();
  EXPECT_TRUE(ReadPackets().empty());
}

TEST_F(ServiceRingBufferDrainerTest, ForbiddenDestinationRecordsLoss) {
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

  drainer_->Drain();

  // The service drops the chunk before any trace buffer sees it. The
  // writer's sequence in the allowed buffer shows the gap.
  EXPECT_EQ(delegate_.chunks_discarded, 1u);
  EXPECT_EQ(drainer_->stats().chunks_admitted, 2u);
  EXPECT_EQ(drainer_->stats().invalid_destination_chunks, 1u);
  ExpectEveryDeliveredChunkAccounted();
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "before");
  EXPECT_FALSE(packets[0].previous_packet_dropped);
  EXPECT_EQ(packets[1].str, "after");
  EXPECT_TRUE(packets[1].previous_packet_dropped);
}

TEST_F(ServiceRingBufferDrainerTest, ProducerLossIsNotAServiceDiscard) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("before")));
  writer.FinishCurrentChunk();
  drainer_->Drain();

  // The writer flags its next chunk. The reader drops that chunk's fragments.
  writer.RecordDataLoss();
  ASSERT_TRUE(WriteFragment(&writer, Packet("lost")));
  writer.FinishCurrentChunk();
  drainer_->Drain();

  ASSERT_TRUE(WriteFragment(&writer, Packet("after")));
  writer.FinishCurrentChunk();
  drainer_->Drain();

  EXPECT_EQ(delegate_.chunks_discarded, 0u);
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "before");
  EXPECT_EQ(packets[1].str, "after");
  EXPECT_TRUE(packets[1].previous_packet_dropped);
}

TEST_F(ServiceRingBufferDrainerTest, MalformedAndUnknownChunksAreDiscards) {
  using Reason = SharedRingBufferReader::ChunkRejection::Reason;
  const uint32_t kWords[] = {
      // 255 size entries do not fit in a 256-byte chunk. The zero bytes decode
      // as 250 empty fragments, then the directory reaches the payload start.
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                        255, kWriter),
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kReservedRouting, 0,
                        1, kWriter),
      // A second chunk of the first reason. Only the first one is kept.
      MakeDataStateWord(ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0,
                        255, kWriter),
  };
  for (uint32_t word : kWords) {
    const uint32_t chunk_pos = ring_buffer_->TryReserveWritePos().chunk_pos;
    Internals::SetChunkStateWord(
        ring_buffer_.get(), ChunkIndex::FromPosition(chunk_pos, kNumChunks),
        word);
  }

  drainer_->Drain();

  EXPECT_EQ(ReadPos(), 3u);
  EXPECT_EQ(delegate_.chunks_discarded, 3u);
  EXPECT_EQ(delegate_.buffer->stats().abi_violations(), 0u);
  // Rejected chunks never reach OnChunkRead().
  EXPECT_EQ(drainer_->reader_stats().chunks_read, 0u);
  EXPECT_EQ(drainer_->reader_stats().malformed_chunks, 2u);
  EXPECT_EQ(drainer_->reader_stats().unsupported_format_chunks, 1u);

  // Each record keeps the word that the reader decoded, although the
  // Complete -> Free transition replaced it.
  ASSERT_EQ(delegate_.rejections.size(), 3u);
  EXPECT_EQ(delegate_.rejections[0].state_word, kWords[0]);
  EXPECT_EQ(ChunkStateOf(ring_buffer_->LoadChunkStateWordAcquire(
                ChunkIndex::FromIndex(0))),
            ChunkState::kFree);

  const auto& first = drainer_->first_rejections();
  const auto& invalid =
      first[static_cast<size_t>(Reason::kInvalidSizeEncoding)];
  ASSERT_TRUE(invalid);
  EXPECT_EQ(invalid->rejection.chunk_pos, 0u);
  EXPECT_EQ(invalid->rejection.state_word, kWords[0]);
  EXPECT_EQ(invalid->rejection.fragment_index, 250u);
  EXPECT_EQ(invalid->rejection.payload_bytes, 0u);
  EXPECT_EQ(invalid->rejection.directory_bytes, 250u);
  EXPECT_EQ(invalid->time_ns, drainer_->last_drain().time_ns);
  const auto& unsupported =
      first[static_cast<size_t>(Reason::kUnsupportedFormat)];
  ASSERT_TRUE(unsupported);
  EXPECT_EQ(unsupported->rejection.chunk_pos, 1u);
  EXPECT_EQ(unsupported->rejection.state_word, kWords[1]);
  EXPECT_FALSE(first[static_cast<size_t>(Reason::kFragmentTooLarge)]);
  EXPECT_FALSE(first[static_cast<size_t>(Reason::kPayloadOverlapsDirectory)]);
}

// Every trace buffer rejection reaches its own counter, and the totals still
// account for every delivered chunk.
TEST_F(ServiceRingBufferDrainerTest, TraceBufferRejectionsAreCounted) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  auto write_and_drain = [&](const std::string& value) {
    ASSERT_TRUE(WriteFragment(&writer, Packet(value)));
    writer.FinishCurrentChunk();
    drainer_->Drain();
  };

  // A ProtoVM of the producer on the buffer.
  delegate_.buffer->MaybeSetUpProtoVm("vm", "", 1024, kProducer);
  write_and_drain("protovm");
  EXPECT_EQ(drainer_->stats().trace_buffer_rejected_protovm, 1u);

  // The writer ID already has a v1 sequence in the buffer.
  delegate_.buffer = TraceBufferV2::Create(64 * 1024, TraceBuffer::kOverwrite);
  const uint8_t v1_payload[] = {0x01, 0x00};
  delegate_.buffer->CopyChunkUntrusted(
      kProducer, ClientIdentity(1000, 1001), kWriter, /*chunk_id=*/0,
      /*num_fragments=*/1, /*chunk_flags=*/0, /*chunk_complete=*/true,
      v1_payload, sizeof(v1_payload));
  write_and_drain("format conflict");
  EXPECT_EQ(drainer_->stats().trace_buffer_rejected_format_conflict, 1u);

  // A full DISCARD buffer.
  delegate_.buffer = TraceBufferV2::Create(4096, TraceBuffer::kDiscard);
  const std::string large(200, 'f');
  for (int i = 0;
       i < 64 && drainer_->stats().trace_buffer_rejected_buffer_full == 0;
       ++i) {
    write_and_drain(large);
  }
  EXPECT_GT(drainer_->stats().trace_buffer_rejected_buffer_full, 0u);
  EXPECT_GT(drainer_->stats().chunks_admitted, 0u);

  EXPECT_EQ(drainer_->stats().invalid_writer_chunks, 0u);
  EXPECT_EQ(drainer_->stats().invalid_destination_chunks, 0u);
  ExpectEveryDeliveredChunkAccounted();
}

TEST_F(ServiceRingBufferDrainerTest, ProtocolErrorIsAnAbiViolation) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("corrupt")));
  writer.FinishCurrentChunk();
  Internals::SetChunkStateWord(ring_buffer_.get(), ChunkIndex::FromIndex(0),
                               static_cast<uint32_t>(ChunkState::kReserved5));

  drainer_->Drain();
  EXPECT_EQ(delegate_.buffer->stats().abi_violations(), 1u);
  EXPECT_EQ(delegate_.protocol_errors, 1u);
  EXPECT_EQ(drainer_->protocol_error_time_ns(), drainer_->last_drain().time_ns);
  EXPECT_EQ(drainer_->last_drain().result,
            SharedRingBufferReader::ConsumeResult::kProtocolError);
  ASSERT_TRUE(drainer_->protocol_error());
  EXPECT_STREQ(drainer_->protocol_error()->reason, "reserved chunk state");
  EXPECT_EQ(drainer_->protocol_error()->state_word,
            static_cast<uint32_t>(ChunkState::kReserved5));
  EXPECT_EQ(drainer_->protocol_error()->read_pos, 0u);
  EXPECT_EQ(drainer_->protocol_error()->write_pos, 1u);

  // The reader stays stopped. Later drains read and count nothing.
  ASSERT_TRUE(WriteFragment(&writer, Packet("ignored")));
  writer.FinishCurrentChunk();
  drainer_->Drain();
  task_runner_.RunUntilIdle();
  EXPECT_EQ(ReadPos(), 0u);
  EXPECT_EQ(delegate_.buffer->stats().abi_violations(), 1u);
  EXPECT_EQ(delegate_.protocol_errors, 1u);
  EXPECT_EQ(drainer_->stats().drain_passes, 1u);
  EXPECT_TRUE(ReadPackets().empty());
}

TEST_F(ServiceRingBufferDrainerTest, WriteDumpCopiesHeaderAndStateWords) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("first")));
  writer.FinishCurrentChunk();
  drainer_->Drain();
  // An open fragment keeps position 1 BeingWritten, so the dump shows a writer
  // that holds it.
  ASSERT_EQ(writer.BeginFragment(/*min_size=*/4, false).result,
            SharedRingBufferWriter::BeginFragmentResult::kSuccess);

  const size_t bound = drainer_->DumpSizeBound(
      /*include_chunk_bytes=*/false, /*only_chunk_pos=*/std::nullopt);
  const auto dump = WriteDump(/*include_chunk_bytes=*/false,
                              /*only_chunk_pos=*/std::nullopt, bound);
  ASSERT_TRUE(dump);

  EXPECT_EQ(dump->chunk_size_bytes(), kChunkSize);
  EXPECT_EQ(dump->num_chunks(), kNumChunks);
  EXPECT_EQ(dump->reader_read_pos(), 1u);
  EXPECT_EQ(dump->read_pos(), 1u);
  EXPECT_EQ(dump->write_pos(), 2u);
  EXPECT_EQ(dump->num_writers_waiting(), 0u);
  ASSERT_EQ(dump->chunk_state_words().size(), kNumChunks);
  EXPECT_EQ(ChunkStateOf(dump->chunk_state_words()[1]),
            ChunkState::kBeingWritten);
  EXPECT_FALSE(dump->has_chunk_bytes());
  EXPECT_FALSE(dump->has_chunks());

  // The state words are the required part. A smaller budget writes nothing.
  EXPECT_FALSE(WriteDump(/*include_chunk_bytes=*/false,
                         /*only_chunk_pos=*/std::nullopt, bound - 1));
}

TEST_F(ServiceRingBufferDrainerTest, WriteDumpCopiesAllChunkBytesThatFit) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("first")));
  writer.FinishCurrentChunk();

  const size_t bound =
      drainer_->DumpSizeBound(/*include_chunk_bytes=*/true, std::nullopt);
  const auto dump =
      WriteDump(/*include_chunk_bytes=*/true, std::nullopt, bound);
  ASSERT_TRUE(dump);
  EXPECT_EQ(dump->chunk_bytes(),
            protos::gen::TracingV2RingBufferDump::CHUNK_BYTES_ALL);
  ASSERT_EQ(dump->chunks().size(), size_t{kNumChunks} * kChunkSize);
  EXPECT_TRUE(dump->chunk_byte_indexes().empty());
  // Each chunk starts with its state word.
  uint32_t word = 0;
  memcpy(&word, dump->chunks().data(), sizeof(word));
  EXPECT_EQ(word, dump->chunk_state_words()[0]);
}

// When not all chunk bytes fit, the dump keeps the chunk at the reader
// position, then the chunks that are not Free, and lists them.
TEST_F(ServiceRingBufferDrainerTest, WriteDumpSelectsChunksWithinBudget) {
  SharedRingBufferWriter first =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&first, Packet("drained")));
  first.FinishCurrentChunk();
  drainer_->Drain();
  // Positions 1 and 2 are Complete. Chunks 3 and 0 are Free.
  SharedRingBufferWriter second =
      MakeWriter(ring_buffer_.get(), kWriter + 1, kBuffer);
  ASSERT_TRUE(WriteFragment(&first, Packet("complete")));
  first.FinishCurrentChunk();
  ASSERT_TRUE(WriteFragment(&second, Packet("held")));

  // Room for the state words and two chunks.
  const size_t required = drainer_->DumpSizeBound(false, std::nullopt);
  const size_t two_chunks = required + 2 * 7 + 2 * (kChunkSize + 5);
  auto dump = WriteDump(/*include_chunk_bytes=*/true, std::nullopt, two_chunks);
  ASSERT_TRUE(dump);
  EXPECT_EQ(dump->chunk_bytes(),
            protos::gen::TracingV2RingBufferDump::CHUNK_BYTES_SELECTED);
  EXPECT_EQ(dump->chunk_byte_indexes(), (std::vector<uint32_t>{1, 2}));
  EXPECT_EQ(dump->chunks().size(), 2u * kChunkSize);
  EXPECT_EQ(dump->chunk_bytes_omitted(), kNumChunks - 2);
  // The state words still cover every chunk.
  EXPECT_EQ(dump->chunk_state_words().size(), kNumChunks);

  // Room for the state words only.
  dump = WriteDump(/*include_chunk_bytes=*/true, std::nullopt, required);
  ASSERT_TRUE(dump);
  EXPECT_EQ(dump->chunk_bytes(),
            protos::gen::TracingV2RingBufferDump::CHUNK_BYTES_OMITTED);
  EXPECT_FALSE(dump->has_chunks());
  EXPECT_EQ(dump->chunk_bytes_omitted(), kNumChunks);
}

// A rejected chunk record keeps the facts of the failed decode.
TEST_F(ServiceRingBufferDrainerTest, RejectionRecordsDecoderFacts) {
  using Reason = SharedRingBufferReader::ChunkRejection::Reason;
  // Two fragments. Size entry 0 says 100 bytes. Size entry 1 has a
  // continuation bit in every byte, so it is longer than four bytes.
  const uint32_t chunk_pos = ring_buffer_->TryReserveWritePos().chunk_pos;
  uint8_t* chunk = ring_buffer_->chunk_at(ChunkIndex::FromIndex(0));
  chunk[kChunkSize - 1] = 100;
  memset(&chunk[kChunkSize - 6], 0x80, 5);
  const uint32_t word = MakeDataStateWord(
      ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0, 2, kWriter);
  Internals::SetChunkStateWord(ring_buffer_.get(), ChunkIndex::FromIndex(0),
                               word);

  drainer_->Drain();

  ASSERT_EQ(delegate_.rejections.size(), 1u);
  const auto& rejection = delegate_.rejections[0];
  EXPECT_EQ(rejection.reason, Reason::kInvalidSizeEncoding);
  EXPECT_EQ(rejection.chunk_pos, chunk_pos);
  EXPECT_EQ(rejection.state_word, word);
  EXPECT_EQ(rejection.fragment_index, 1u);
  EXPECT_EQ(rejection.payload_bytes, 100u);
  EXPECT_EQ(rejection.directory_bytes, 1u);
}

TEST_F(ServiceRingBufferDrainerTest, LostRacesPostOneDelayedRetry) {
  SharedRingBufferWriter writer =
      MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
  ASSERT_TRUE(WriteFragment(&writer, Packet("first")));
  writer.FinishCurrentChunk();
  ASSERT_TRUE(WriteFragment(&writer, Packet("second")));
  writer.FinishCurrentChunk();

  // At position 1, flip the chunk between Complete and BeingWritten before
  // each reader transition. The reader loses every attempt at that position.
  SharedRingBufferReader* reader =
      test::ServiceRingBufferDrainerTestPeer::reader(drainer_.get());
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

  drainer_->Drain();
  drainer_->Drain();
  EXPECT_EQ(ReadPos(), 1u);
  // Both drains stopped early. The second one found the retry pending.
  EXPECT_TRUE(
      test::ServiceRingBufferDrainerTestPeer::retry_scheduled(*drainer_));

  // The retry runs after the retry delay and consumes the position.
  Internals::SetBeforeStateTransitionCallback(reader, {});
  task_runner_.AdvanceTimeAndRunUntilIdle(1);
  EXPECT_EQ(ReadPos(), 2u);
  EXPECT_FALSE(
      test::ServiceRingBufferDrainerTestPeer::retry_scheduled(*drainer_));

  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "first");
  EXPECT_EQ(packets[1].str, "second");
}

TEST_F(ServiceRingBufferDrainerTest, DestructionCancelsRetry) {
  {
    SharedRingBufferWriter writer =
        MakeWriter(ring_buffer_.get(), kWriter, kBuffer);
    ASSERT_TRUE(WriteFragment(&writer, Packet("first")));
    writer.FinishCurrentChunk();
  }
  SharedRingBufferReader* reader =
      test::ServiceRingBufferDrainerTestPeer::reader(drainer_.get());
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
  drainer_->Drain();
  EXPECT_EQ(ReadPos(), 0u);

  // The drainer holds the only reference to the mapping, so the writer view is
  // invalid after this.
  // The pending retry must not run. ASan reports it if it does.
  ring_buffer_.reset();
  drainer_.reset();
  task_runner_.AdvanceTimeAndRunUntilIdle(1);
}

}  // namespace
}  // namespace perfetto::tracing_v2
