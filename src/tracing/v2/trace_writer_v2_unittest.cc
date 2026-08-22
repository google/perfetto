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

#include "perfetto/base/time.h"

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/public/pb_msg.h"
#include "perfetto/public/protos/trace/test_event.pzc.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"
#include "src/shared_lib/stream_writer.h"
#include "src/tracing/v2/chunk_reader.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace tracing_v2 {
namespace {

class FakeDelegate : public TraceWriterV2::Delegate {
 public:
  ~FakeDelegate() override = default;

  void OnPacketsCommitted() override { ++commit_notifications; }

  void OnWriterFlush(WriterID writer_id,
                     uint32_t pos,
                     std::function<void()> callback) override {
    last_flushed_writer = writer_id;
    last_flush_pos = pos;
    if (callback)
      callback();
  }

  void OnWriterDestroyed(WriterID writer_id, uint32_t pos) override {
    destroyed_writer = writer_id;
    destruction_pos = pos;
  }

  uint32_t commit_notifications = 0;
  WriterID last_flushed_writer = 0;
  uint32_t last_flush_pos = 0;
  WriterID destroyed_writer = 0;
  uint32_t destruction_pos = 0;
};

ChunkHeader MakeHeader(WriterID writer_id, BufferID target_buffer) {
  ChunkHeader header;
  header.writer_id = writer_id;
  header.target_buffer = target_buffer;
  return header;
}

TraceWriterV2::InitArgs MakeArgs(SharedRingBuffer* ring,
                                 std::shared_ptr<FakeDelegate> delegate,
                                 WriterID writer_id = 1,
                                 BufferID target_buffer = 2) {
  TraceWriterV2::InitArgs args;
  args.ring_buffer = ring;
  args.delegate = delegate;
  args.writer_id = writer_id;
  args.target_buffer = target_buffer;
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kDrop;
  return args;
}

std::vector<protos::gen::TracePacket> DrainPackets(ChunkReader* reader) {
  std::vector<protos::gen::TracePacket> packets;
  reader->Drain([&packets](const ChunkReader::Packet& packet) {
    protos::gen::TracePacket decoded;
    EXPECT_TRUE(decoded.ParseFromArray(packet.data, packet.size));
    packets.emplace_back(std::move(decoded));
  });
  return packets;
}

TEST(TraceWriterV2Test, WritesAndUngroupsGeneratedPacket) {
  SharedRingBuffer ring(8);
  ChunkReader reader(&ring);
  auto delegate = std::make_shared<FakeDelegate>();
  {
    TraceWriterV2 writer(MakeArgs(&ring, delegate));
    auto packet = writer.NewTracePacket();
    packet->set_timestamp(1234);
    auto* payload = packet->set_for_testing()->set_payload();
    payload->add_str("alpha");
    payload->add_nested()->set_single_string("nested");
  }

  const std::vector<protos::gen::TracePacket> packets = DrainPackets(&reader);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 1234u);
  ASSERT_TRUE(packets[0].has_for_testing());
  EXPECT_EQ(packets[0].for_testing().payload().str()[0], "alpha");
  EXPECT_EQ(packets[0].for_testing().payload().nested()[0].single_string(),
            "nested");
  EXPECT_GT(delegate->commit_notifications, 0u);
  EXPECT_EQ(delegate->destroyed_writer, 1);
}

TEST(TraceWriterV2Test, PacksBackToBackPacketsIntoOneChunk) {
  SharedRingBuffer ring(8);
  ChunkReader reader(&ring);
  auto delegate = std::make_shared<FakeDelegate>();
  TraceWriterV2 writer(MakeArgs(&ring, delegate));
  for (uint64_t timestamp : {1u, 2u, 3u})
    writer.NewTracePacket()->set_timestamp(timestamp);

  EXPECT_EQ(ring.write_pos(), 1u);
  const std::vector<protos::gen::TracePacket> packets = DrainPackets(&reader);
  ASSERT_EQ(packets.size(), 3u);
  EXPECT_EQ(packets[0].timestamp(), 1u);
  EXPECT_EQ(packets[1].timestamp(), 2u);
  EXPECT_EQ(packets[2].timestamp(), 3u);
}

TEST(TraceWriterV2Test, FragmentsPacketAtArbitraryProtoByteOffsets) {
  SharedRingBuffer ring(16);
  ChunkReader reader(&ring);
  auto delegate = std::make_shared<FakeDelegate>();
  TraceWriterV2 writer(MakeArgs(&ring, delegate));
  const std::string value(1000, 'x');
  writer.NewTracePacket()->set_for_testing()->set_str(value);

  EXPECT_GT(ring.write_pos(), 1u);
  const std::vector<protos::gen::TracePacket> packets = DrainPackets(&reader);
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].for_testing().str(), value);
}

TEST(TraceWriterV2Test, DropsWithoutBlockingAndReportsRecovery) {
  SharedRingBuffer ring(2);
  auto delegate = std::make_shared<FakeDelegate>();

  SharedRingBuffer::Chunk blocker1 =
      ring.TryAcquireChunkForWriting(MakeHeader(1, 1));
  SharedRingBuffer::Chunk blocker2 =
      ring.TryAcquireChunkForWriting(MakeHeader(2, 1));
  ASSERT_TRUE(blocker1.is_valid());
  ASSERT_TRUE(blocker2.is_valid());
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&blocker1, /*added_flags=*/0));
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&blocker2, /*added_flags=*/0));

  TraceWriterV2 writer(MakeArgs(&ring, delegate, /*writer_id=*/3));
  writer.NewTracePacket()->set_timestamp(1);
  EXPECT_EQ(writer.drop_count(), 1u);

  ChunkHeader ignored_header;
  uint8_t ignored_payload[kChunkPayloadSize]{};
  uint32_t ignored_payload_size = 0;
  ASSERT_EQ(ring.TryReadChunk(&ignored_header, ignored_payload,
                              &ignored_payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);
  ASSERT_EQ(ring.TryReadChunk(&ignored_header, ignored_payload,
                              &ignored_payload_size),
            SharedRingBuffer::ReadResult::kChunkRead);

  writer.NewTracePacket()->set_timestamp(2);
  ChunkReader reader(&ring);
  uint32_t loss_reasons = 0;
  reader.Drain([&loss_reasons](const ChunkReader::Packet& packet) {
    loss_reasons = packet.previous_packet_dropped;
  });
  // The ring was full, so the reason has to say so and not just "something was
  // lost".
  EXPECT_EQ(loss_reasons, static_cast<uint32_t>(
                              protos::pbzero::TracePacket::DATA_LOSS_PRESENT |
                              protos::pbzero::TracePacket::DATA_LOSS_SMB_FULL));
}

// kStallThenDrop waits for the reader, then gives up rather than blocking
// forever. Nothing drains the ring here, so the deadline is what ends it.
TEST(TraceWriterV2Test, StallThenDropGivesUpAtTheDeadline) {
  if (!kHasFutex)
    GTEST_SKIP() << "This test blocks on the Linux/Android futex";
  SharedRingBuffer ring(2);
  auto delegate = std::make_shared<FakeDelegate>();

  SharedRingBuffer::Chunk blocker1 =
      ring.TryAcquireChunkForWriting(MakeHeader(1, 1));
  SharedRingBuffer::Chunk blocker2 =
      ring.TryAcquireChunkForWriting(MakeHeader(2, 1));
  ASSERT_TRUE(blocker1.is_valid() && blocker2.is_valid());

  TraceWriterV2::InitArgs args = MakeArgs(&ring, delegate, /*writer_id=*/3);
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kStallThenDrop;
  args.stall_then_drop_timeout_ms = 1;
  TraceWriterV2 writer(args);

  const int64_t started_ms = base::GetWallTimeMs().count();
  writer.NewTracePacket()->set_timestamp(1);
  writer.FinishTracePacket();
  EXPECT_EQ(writer.drop_count(), 1u);
  // Should have waited at all, but nothing like the 30s fatal deadline.
  EXPECT_LT(base::GetWallTimeMs().count() - started_ms, 5000);

  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&blocker1, /*added_flags=*/0));
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&blocker2, /*added_flags=*/0));
}

// kStall means wait for the consumer, and with the consumer on its own
// sequence that is all a writer has to do: it waits, and does not silently
// become a drop. The packet only gets written because this thread frees a
// chunk.
TEST(TraceWriterV2Test, StalledWriterResumesWhenAnotherThreadDrains) {
  if (!kHasFutex)
    GTEST_SKIP() << "This test blocks on the Linux/Android futex";
  SharedRingBuffer ring(2);
  auto delegate = std::make_shared<FakeDelegate>();

  // Fill the ring. Both chunks are empty, so draining them yields no packets
  // and anything the reader below sees came from the stalled writer.
  SharedRingBuffer::Chunk blocker1 =
      ring.TryAcquireChunkForWriting(MakeHeader(1, 1));
  SharedRingBuffer::Chunk blocker2 =
      ring.TryAcquireChunkForWriting(MakeHeader(2, 1));
  ASSERT_TRUE(blocker1.is_valid() && blocker2.is_valid());
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&blocker1, /*added_flags=*/0));
  ASSERT_TRUE(ring.ReleaseChunkAsComplete(&blocker2, /*added_flags=*/0));

  TraceWriterV2::InitArgs args = MakeArgs(&ring, delegate, /*writer_id=*/3);
  args.buffer_exhausted_policy = BufferExhaustedPolicy::kStall;

  std::atomic<uint64_t> drops{0};
  std::thread producer([&args, &drops] {
    TraceWriterV2 writer(args);
    writer.NewTracePacket()->set_timestamp(7);
    writer.FinishTracePacket();
    drops.store(writer.drop_count(), std::memory_order_release);
  });

  // Stands in for the relay sequence: nothing else can free a chunk, so the
  // writer above cannot finish until this loop runs.
  ChunkReader reader(&ring);
  std::vector<protos::gen::TracePacket> packets;
  const int64_t deadline_ms = base::GetWallTimeMs().count() + 30000;
  while (packets.empty()) {
    ASSERT_LT(base::GetWallTimeMs().count(), deadline_ms);
    packets = DrainPackets(&reader);
  }
  producer.join();

  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].timestamp(), 7u);
  EXPECT_EQ(drops.load(std::memory_order_acquire), 0u);
}

TEST(TraceWriterV2Test, FlushCarriesRingWritePos) {
  SharedRingBuffer ring(4);
  auto delegate = std::make_shared<FakeDelegate>();
  TraceWriterV2 writer(MakeArgs(&ring, delegate, /*writer_id=*/9));
  writer.NewTracePacket()->set_timestamp(1);
  bool callback_called = false;
  writer.Flush([&callback_called] { callback_called = true; });

  EXPECT_TRUE(callback_called);
  EXPECT_EQ(delegate->last_flushed_writer, 9);
  EXPECT_EQ(delegate->last_flush_pos, ring.write_pos());

  ChunkReader reader(&ring);
  DrainPackets(&reader);
}

TEST(TraceWriterV2Test, CWriterUsesTheSameGroupEncodingPath) {
  SharedRingBuffer ring(8);
  ChunkReader reader(&ring);
  auto delegate = std::make_shared<FakeDelegate>();
  TraceWriterV2 writer(MakeArgs(&ring, delegate));

  auto packet = writer.NewTracePacket();
  protozero::ScatteredStreamWriter* stream_writer = packet.TakeStreamWriter();
  PerfettoPbMsgWriter c_writer{};
  c_writer.writer.impl =
      reinterpret_cast<PerfettoStreamWriterImpl*>(stream_writer);
  UpdateStreamWriter(*stream_writer, &c_writer.writer);

  perfetto_protos_TracePacket root{};
  perfetto_protos_TestEvent nested{};
  PerfettoPbMsgInit(&root.msg, &c_writer);
  ASSERT_TRUE(PerfettoPbMsgNestedMessagesAreGroups(&root.msg));
  perfetto_protos_TracePacket_set_timestamp(&root, 42);
  perfetto_protos_TracePacket_begin_for_testing(&root, &nested);
  const std::string value(1000, 'c');
  perfetto_protos_TestEvent_set_str(&nested, value.data(), value.size());
  perfetto_protos_TracePacket_end_for_testing(&root, &nested);
  PerfettoPbMsgFinalize(&root.msg);
  stream_writer->set_write_ptr(c_writer.writer.write_ptr);
  writer.FinishTracePacket();

  EXPECT_GT(ring.write_pos(), 1u);
  protos::gen::TracePacket decoded;
  reader.Drain([&decoded](const ChunkReader::Packet& packet_bytes) {
    EXPECT_TRUE(decoded.ParseFromArray(packet_bytes.data, packet_bytes.size));
  });
  EXPECT_EQ(decoded.timestamp(), 42u);
  EXPECT_EQ(decoded.for_testing().str(), value);
}

}  // namespace
}  // namespace tracing_v2
}  // namespace perfetto
