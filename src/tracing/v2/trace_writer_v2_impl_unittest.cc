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
// - the drain threshold after each publication,
// - the BufferExhaustedPolicy: stalls, drops, deadlines and the drain
//   requests that they send, also for relocations,
// - drop mode, loss reports and the drop_count() and written() counters,
// - publishing the last chunk before the WriterID is released.

#include "src/tracing/v2/trace_writer_v2_impl.h"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/base/time.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/tracing/v2/producer_ring_buffer_test.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::tracing_v2 {

namespace test {

// Gives tests access to the private state of TraceWriterV2Impl.
class TraceWriterV2ImplTestPeer {
 public:
  // Replaces the clock of the stall deadlines.
  static void SetClock(TraceWriter* writer,
                       TraceWriterV2Impl::ClockFunction clock) {
    static_cast<TraceWriterV2Impl*>(writer)->get_time_ms_ = clock;
  }

  static SharedRingBufferWriter* GetRingBufferWriter(TraceWriter* writer) {
    return &static_cast<TraceWriterV2Impl*>(writer)->ring_buffer_writer_;
  }
};

}  // namespace test

namespace {

using test::PinAllChunks;
using test::TraceWriterV2ImplTestPeer;
using ::testing::UnorderedElementsAre;
using Internals = test::SharedRingBufferInternalsForTest;

// The same value as kStallTimeoutMs in trace_writer_v2_impl.cc.
constexpr base::TimeMillis kStallTimeout{30000};

// The fake time of the stall deadlines. Tests advance it in the endpoint's
// DrainV2RingBuffer(), which a stalled writer calls before each wait. So the
// deadline decisions do not depend on how long the real waits take.
base::TimeMillis g_fake_now{};

base::TimeMillis FakeNow() {
  return g_fake_now;
}

class TraceWriterV2ImplTest : public ProducerRingBufferTest {
 protected:
  void SetUp() override {
    ProducerRingBufferTest::SetUp();
    g_fake_now = base::TimeMillis(1000000);
  }

  // Makes |writer| use g_fake_now for its stall deadlines.
  static void UseFakeClock(TraceWriter* writer) {
    TraceWriterV2ImplTestPeer::SetClock(writer, &FakeNow);
  }

  // Leaves a packet open whose relocation finds no free chunk. Needs a ring
  // buffer of 2 chunks:
  // 1. |writer| publishes a packet in chunk 0. Another writer fills chunk 1.
  // 2. |writer| opens a packet in its cached chunk 0.
  // 3. The reader copies chunk 0 and requests a rewrite.
  // Closing the open fragment then needs a new chunk:
  // - Chunk 1 waits for the reader.
  // - The claim of chunk 0 fails until the reader releases it.
  TraceWriter::TracePacketHandle OpenPacketInScrapedChunk(TraceWriter* writer) {
    WritePacket(writer, std::string(200, 'a'));
    {
      auto other = CreateWriter();
      WritePacket(other.get(), std::string(200, 'b'));
    }
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str("relocated");
    reader_->Drain(/*max_positions=*/1);
    EXPECT_EQ(reader_->GetStats().rewrite_requests, 1u);
    return packet;
  }

  static std::vector<std::string> StringsOf(
      const std::vector<ReadPacket>& packets) {
    std::vector<std::string> strs;
    for (const ReadPacket& read : packets)
      strs.push_back(read.packet.for_testing().str());
    return strs;
  }
};

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

// --- Drain threshold ---

// With 8 chunks, the threshold is 2 outstanding positions. Publications below
// it stay buffered. Once it is reached, further requests share the pending
// drain task.
TEST_F(TraceWriterV2ImplTest, DrainRequestsAreCoalescedAtOccupancyThreshold) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/8);
  auto writer = CreateWriter();
  // Two of these packets do not fit one 256-byte chunk.
  const std::string large(200, 'x');

  WritePacket(writer.get(), large);
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 0u);  // 1 of 8 positions waits.

  WritePacket(writer.get(), large);  // 2 of 8: asks for a drain.
  WritePacket(writer.get(), large);  // Merged into the pending task.
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
  EXPECT_EQ(ReadPackets().size(), 3u);
}

// The threshold counts the outstanding positions of all writers. One small
// packet from each of two writers reaches it together.
TEST_F(TraceWriterV2ImplTest, DrainThresholdCountsAllWriters) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/8);
  auto first = CreateWriter();
  auto second = CreateWriter();

  WritePacket(first.get(), "first");  // 1 of 8 positions.
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 0u);

  WritePacket(second.get(), "second");  // 2 of 8: asks for a drain.
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
  EXPECT_EQ(ReadPackets().size(), 2u);
}

// A publication after a relocation also checks the threshold.
TEST_F(TraceWriterV2ImplTest, RelocatedPublicationAppliesDrainThreshold) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/8);
  auto writer = CreateWriter();
  // Stays alive until the end. Its destructor would flush.
  auto other = CreateWriter();
  WritePacket(writer.get(), "first");  // Position 0: 1 of 8.
  {
    // Reuses the cached chunk at position 0.
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str("relocated");
    // The reader scrapes position 0 and requests a rewrite: 0 of 8.
    reader_->Drain(/*max_positions=*/1);
    WritePacket(other.get(), "other");  // Position 1: 1 of 8.
  }  // The relocation publishes at position 2: 2 of 8.
  EXPECT_EQ(reader_->GetStats().rewrite_requests, 1u);
  EXPECT_EQ(writer->drop_count(), 0u);

  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
  EXPECT_THAT(StringsOf(ReadPackets()),
              UnorderedElementsAre("first", "other", "relocated"));
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
  // kDrop does not wait, so it sends no request from this thread.
  EXPECT_EQ(num_drain_requests_, 0u);
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

// Failed claims leave reserved positions that only the reader can move past.
// A writer that drops after them must still ask for a drain, so the positions
// do not block later reservations.
TEST_F(TraceWriterV2ImplTest, DropAfterFailedClaimsRequestsDrain) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  // Every claim fails on a pinned chunk.
  ASSERT_TRUE(PinAllChunks(reader_ring_buffer_.get(), /*owner=*/99));

  auto writer = CreateWriter(BufferExhaustedPolicy::kDrop);
  WritePacket(writer.get(), "dropped");
  EXPECT_EQ(writer->drop_count(), 1u);
  EXPECT_EQ(reader_ring_buffer_->LoadWritePosRelaxed(), 4u);
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
}

TEST_F(TraceWriterV2ImplTest, WriterDoesNotWaitBeforeReaderAttached) {
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);
  const std::string payload(200, 'o');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_GT(writer->drop_count(), 0u);
  EXPECT_EQ(num_drain_requests_, 0u);
}

TEST_F(TraceWriterV2ImplTest, StalledWriterDrainsOnEndpointThread) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);

  // No task runs here. The writer runs on the endpoint thread, so each wait
  // for space sends the drain request at once, through
  // RequestDrain(kUrgent).
  const std::string payload(200, 's');
  for (int i = 0; i < 12; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 0u);
  EXPECT_GT(num_drain_requests_, 0u);
  writer.reset();
  task_runner_.RunUntilIdle();

  EXPECT_EQ(ReadPackets().size(), 12u);
}

// On another thread, a stalled writer posts its drain request. The endpoint
// thread runs the task, the reader frees space, and the writer continues.
TEST_F(TraceWriterV2ImplTest, StalledWriterOnOtherThreadDrainsThroughTasks) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  constexpr size_t kNumPackets = 12;
  auto writer_done = task_runner_.CreateCheckpoint("writer_done");
  uint64_t drop_count = 0;
  std::thread writer_thread([&] {
    auto writer = CreateWriter(BufferExhaustedPolicy::kStall);
    const std::string payload(200, 't');
    for (size_t i = 0; i < kNumPackets; ++i)
      WritePacket(writer.get(), payload);
    drop_count = writer->drop_count();
    writer.reset();
    task_runner_.PostTask(writer_done);
  });

  // This thread is the endpoint thread. It runs the drain tasks. The timeout
  // only detects a writer that stops making progress.
  task_runner_.RunUntilCheckpoint("writer_done", /*timeout_ms=*/30000);
  writer_thread.join();
  task_runner_.RunUntilIdle();
  Drain();

  EXPECT_EQ(drop_count, 0u);
  EXPECT_GT(num_drain_requests_, 0u);
  EXPECT_EQ(ReadPackets().size(), kNumPackets);
}

// If the reader detaches during a stall, the next check drops the packet.
// Without that check, kStall would wait 30 s and then crash.
TEST_F(TraceWriterV2ImplTest, StallEndsWhenReaderDetaches) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  // The service rejects the ring buffer instead of draining it.
  ON_CALL(endpoint_, DrainV2RingBuffer()).WillByDefault([this] {
    ++num_drain_requests_;
    arbiter_->Disconnect();
  });
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);

  // Each 200-byte packet needs its own chunk. No task runs, so the fifth
  // packet finds the ring buffer full and stalls on the endpoint thread.
  const std::string payload(200, 's');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 1u);
  // One stall, one direct drain request. The writer then saw kDetached.
  EXPECT_EQ(num_drain_requests_, 1u);
}

// After a drop, kStallThenDrop does not stall until a chunk carries the loss.
// Otherwise each dropped packet could cost another full timeout.
TEST_F(TraceWriterV2ImplTest, StallThenDropDoesNotStallWithPendingLoss) {
  // Without a reader, the writer drops without a stall.
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStallThenDrop);
  // Each 200-byte packet needs its own chunk.
  const std::string payload(200, 'd');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  ASSERT_EQ(writer->drop_count(), 1u);

  // The reader attaches, but the ring buffer stays full until the tasks run.
  // A stall would send a drain request at once, from this thread.
  AttachReader();
  WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 1u);
  EXPECT_EQ(num_drain_requests_, 0u);

  // The posted drain empties the ring buffer. The next chunk carries the loss.
  task_runner_.RunUntilIdle();
  WritePacket(writer.get(), "carries the loss");
  const uint32_t drain_requests_before = num_drain_requests_;

  // Without a pending loss, a full ring buffer stalls again. The stall drains
  // the ring buffer at once, so no packet is dropped.
  for (int i = 0; i < 8; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 1u);
  EXPECT_GT(num_drain_requests_, drain_requests_before);
}

// A rewrite of BeingWritten(0) moves the chunk's unreported loss flag to the
// relocated fragment. kStallThenDrop must see that loss too, and drop.
TEST_F(TraceWriterV2ImplTest, StallThenDropSeesLossInheritedByRelocation) {
  // Without a reader, the first full ring buffer drops without a stall.
  CreateRingBufferArbiter(/*num_chunks=*/2);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStallThenDrop);
  const std::string payload(200, 'd');
  for (int i = 0; i < 4 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  ASSERT_EQ(writer->drop_count(), 1u);

  // The posted drain empties the ring buffer.
  AttachReader();
  task_runner_.RunUntilIdle();
  const uint32_t drain_requests_before = num_drain_requests_;

  {
    // The claim puts the pending loss into the new chunk, as BeingWritten(0).
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str("open");
    {
      auto other = CreateWriter();
      WritePacket(other.get(), payload);  // Fills the other chunk.
    }
    // The reader takes no fragment, so the flag moves with the relocation.
    reader_->Drain(/*max_positions=*/1);
  }  // Closing the fragment needs a new chunk. None is free.

  EXPECT_EQ(writer->drop_count(), 2u);
  // A stall would have sent a request at once, from this thread.
  EXPECT_EQ(num_drain_requests_, drain_requests_before);
}

// --- Stall deadline ---
//
// These tests use the fake clock g_fake_now. Each one checks the exact number
// of drain requests, which does not change with spurious wakes or slow waits.

// Failed retries keep the deadline of their acquisition. Each request moves
// the clock by a third of the timeout. So the writer drops after the third
// request. If a retry restarted the deadline, the writer would keep waiting.
// The fourth request then frees space, so that error shows as a delivered
// packet, not as a hang.
TEST_F(TraceWriterV2ImplTest, StallDeadlineCoversAllRetries) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  ON_CALL(endpoint_, DrainV2RingBuffer()).WillByDefault([this] {
    if (++num_drain_requests_ <= 3) {
      g_fake_now += kStallTimeout / 3;
    } else {
      Drain();
    }
  });
  auto writer = CreateWriter(BufferExhaustedPolicy::kStallThenDrop);
  UseFakeClock(writer.get());

  // Each 200-byte packet needs its own chunk. No task runs, so the fifth
  // packet stalls.
  const std::string payload(200, 'd');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);

  EXPECT_EQ(writer->drop_count(), 1u);
  EXPECT_EQ(num_drain_requests_, 3u);
}

// The same holds for the replacement acquisition of a relocation.
TEST_F(TraceWriterV2ImplTest, RelocationStallDeadlineCoversAllRetries) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/2);
  ON_CALL(endpoint_, DrainV2RingBuffer()).WillByDefault([this] {
    if (++num_drain_requests_ <= 3) {
      g_fake_now += kStallTimeout / 3;
    } else {
      Drain();
    }
  });
  auto writer = CreateWriter(BufferExhaustedPolicy::kStallThenDrop);
  UseFakeClock(writer.get());

  {
    auto packet = OpenPacketInScrapedChunk(writer.get());
  }  // Closing the fragment stalls until the deadline, then drops it.

  EXPECT_EQ(writer->drop_count(), 1u);
  EXPECT_EQ(num_drain_requests_, 3u);
}

// A claimed replacement starts a new acquisition with its own deadline:
//
//   time   event
//   -----  -------------------------------------------------------------
//   0 s    The first acquisition waits. Its deadline is 30 s.
//   20 s   A drain frees chunks. The writer claims one as the replacement.
//          Before the publication, the reader requests a rewrite of it,
//          and another writer takes the other free chunk. The second
//          acquisition waits. Its deadline is 50 s.
//   35 s   Past the first deadline. The second acquisition still waits.
//   35 s   A drain frees a chunk. The publication succeeds.
//
// With one deadline for both acquisitions, the writer would drop at 35 s.
TEST_F(TraceWriterV2ImplTest, RelocationDeadlineRestartsAfterReplacementClaim) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/2);
  ON_CALL(endpoint_, DrainV2RingBuffer()).WillByDefault([this] {
    ++num_drain_requests_;
    if (num_drain_requests_ == 1) {
      g_fake_now += base::TimeMillis(20000);
      Drain();
    } else if (num_drain_requests_ == 2) {
      g_fake_now += base::TimeMillis(15000);
    } else {
      Drain();
    }
  });
  auto writer = CreateWriter(BufferExhaustedPolicy::kStallThenDrop);
  UseFakeClock(writer.get());

  std::unique_ptr<SharedRingBufferWriter> blocker;
  bool rewrite_replacement = true;
  Internals::SetAfterReplacementClaimCallback(
      TraceWriterV2ImplTestPeer::GetRingBufferWriter(writer.get()), [&] {
        if (!rewrite_replacement)
          return;
        rewrite_replacement = false;
        const ChunkIndex replacement_idx = ChunkIndex::FromIndex(1);
        uint32_t observed =
            reader_ring_buffer_->LoadChunkStateWordAcquire(replacement_idx);
        EXPECT_EQ(ChunkStateOf(observed), ChunkState::kBeingWritten);
        EXPECT_TRUE(
            reader_ring_buffer_->TryRequestRewrite(replacement_idx, &observed));
        blocker = std::make_unique<SharedRingBufferWriter>(
            reader_ring_buffer_.get(), /*writer_id=*/98, kTargetBuffer);
        EXPECT_EQ(blocker->BeginFragment(1, false).result,
                  SharedRingBufferWriter::BeginFragmentResult::kSuccess);
      });

  {
    auto packet = OpenPacketInScrapedChunk(writer.get());
  }  // Closing the fragment needs two replacement acquisitions.

  EXPECT_FALSE(rewrite_replacement);
  EXPECT_EQ(writer->drop_count(), 0u);
  EXPECT_EQ(num_drain_requests_, 3u);

  // Read the rest directly. The endpoint above does not drain every request.
  writer.reset();
  Drain();
  EXPECT_THAT(StringsOf(ReadPackets()),
              UnorderedElementsAre(std::string(200, 'a'), std::string(200, 'b'),
                                   "relocated"));
}

// A drop at the deadline also asks for a drain if the last round left
// unclaimed positions. Otherwise nothing would move past them: later attempts
// find the ring buffer full, reserve nothing, and ask for no drain.
TEST_F(TraceWriterV2ImplTest, DeadlineDropAfterFailedClaimsRequestsDrain) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  // Every claim fails on a pinned chunk. Each drain moves read_pos past the
  // reserved positions, but cannot free the chunks. The first request also
  // reaches the deadline.
  ASSERT_TRUE(PinAllChunks(reader_ring_buffer_.get(), /*owner=*/99));
  ON_CALL(endpoint_, DrainV2RingBuffer()).WillByDefault([this] {
    if (++num_drain_requests_ == 1)
      g_fake_now += kStallTimeout;
    Drain();
  });
  auto writer = CreateWriter(BufferExhaustedPolicy::kStallThenDrop);
  UseFakeClock(writer.get());

  WritePacket(writer.get(), "dropped");
  EXPECT_EQ(writer->drop_count(), 1u);
  EXPECT_EQ(num_drain_requests_, 1u);

  // The drop posted one more request for the positions of the last round.
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 2u);
}

// At the deadline, kStall aborts. The death test runs in a forked child, so
// the child creates the writer: a writer detects use after a fork.
TEST_F(TraceWriterV2ImplTest, StallDeadlineAbortsUnderStall) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/4);
  // The service never drains. The first request reaches the deadline.
  ON_CALL(endpoint_, DrainV2RingBuffer()).WillByDefault([] {
    g_fake_now += kStallTimeout;
  });

  const std::string payload(200, 's');
  EXPECT_DEATH_IF_SUPPORTED(
      {
        auto writer = CreateWriter(BufferExhaustedPolicy::kStall);
        UseFakeClock(writer.get());
        for (int i = 0; i < 8; ++i)
          WritePacket(writer.get(), payload);
      },
      "possible deadlock");
}

// --- Relocation ---

// A relocation that finds no free chunk waits like a new fragment does. The
// drain frees a chunk, and the packet arrives intact.
TEST_F(TraceWriterV2ImplTest, RelocationWaitsForSpaceThenPublishes) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/2);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);
  {
    auto packet = OpenPacketInScrapedChunk(writer.get());
    EXPECT_EQ(num_drain_requests_, 0u);
  }  // Closing the fragment stalls once. The stall drains at once.

  EXPECT_EQ(num_drain_requests_, 1u);
  EXPECT_EQ(writer->drop_count(), 0u);
  writer.reset();
  task_runner_.RunUntilIdle();
  Drain();

  const auto packets = ReadPackets();
  EXPECT_THAT(StringsOf(packets),
              UnorderedElementsAre(std::string(200, 'a'), std::string(200, 'b'),
                                   "relocated"));
  for (const ReadPacket& read : packets)
    EXPECT_EQ(read.previous_dropped, 0u);
}

// Under kDrop, a relocation that finds no free chunk drops the fragment. The
// rest of the packet goes to the drop buffer, and the next chunk reports the
// loss.
TEST_F(TraceWriterV2ImplTest, RelocationDropDiscardsPacketAndReportsLoss) {
  CreateRingBufferArbiterWithReader(/*num_chunks=*/2);
  auto writer = CreateWriter(BufferExhaustedPolicy::kDrop);
  {
    auto packet = OpenPacketInScrapedChunk(writer.get());
    // This field does not fit the chunk. Closing the fragment needs the
    // relocation, which drops it. The rest goes to the drop buffer.
    packet->set_for_testing()->set_str(std::string(300, 'x'));
    EXPECT_EQ(writer->drop_count(), 1u);
  }
  // kDrop does not wait, so it sends no request from this thread.
  EXPECT_EQ(num_drain_requests_, 0u);

  // The posted drain frees the ring buffer. The first chunk after the loss
  // carries kFlagDataLoss. The reader discards all of its fragments.
  task_runner_.RunUntilIdle();
  WritePacket(writer.get(), "in flagged chunk");
  writer->Flush();
  WritePacket(writer.get(), "recovered");
  writer.reset();
  task_runner_.RunUntilIdle();

  const auto packets = ReadPackets();
  EXPECT_THAT(StringsOf(packets),
              UnorderedElementsAre(std::string(200, 'a'), std::string(200, 'b'),
                                   "recovered"));
  for (const ReadPacket& read : packets) {
    if (read.packet.for_testing().str() == "recovered")
      EXPECT_NE(read.previous_dropped, 0u);
  }
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

// The writer publishes the first dropped packet of an episode at once. It
// publishes later ones in batches of 64, and the rest when the episode ends.
TEST_F(TraceWriterV2ImplTest, DroppedPacketsArePublishedPromptly) {
  // Without a reader, kDrop does not wait and nothing frees space.
  CreateRingBufferArbiter(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kDrop);
  auto published = [this] {
    return reader_ring_buffer_->LoadHeaderRelaxed().dropped_packets;
  };
  // Each 200-byte packet needs its own chunk. Four fill the ring buffer.
  const std::string payload(200, 'd');
  for (int i = 0; i < 4; ++i)
    WritePacket(writer.get(), payload);
  ASSERT_EQ(writer->drop_count(), 0u);

  // The first loss is visible at once, before any flush.
  WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 1u);
  EXPECT_EQ(published(), 1u);

  // The next 63 losses wait for a batch. The 64th completes it.
  for (int i = 0; i < 63; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(published(), 1u);
  WritePacket(writer.get(), payload);
  EXPECT_EQ(published(), 65u);

  // Two more wait. The end of the episode publishes them.
  WritePacket(writer.get(), payload);
  WritePacket(writer.get(), payload);
  EXPECT_EQ(published(), 65u);
  AttachReader();
  Drain();
  WritePacket(writer.get(), "fits");
  EXPECT_EQ(published(), 67u);
  // One episode counts as one drop.
  EXPECT_EQ(writer->drop_count(), 1u);
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

// The destructor finishes the open packet, publishes it and flushes, as
// TraceWriterBase documents. Then it releases the WriterID.
TEST_F(TraceWriterV2ImplTest, DestructionPublishesFlushesThenReleasesWriterId) {
  // With 8 chunks, one packet stays below the drain threshold.
  CreateRingBufferArbiterWithReader(/*num_chunks=*/8);
  auto writer = CreateWriter();
  EXPECT_NE(writer->writer_id(), 0u);

  // A raw stream packet stays open until FinishTracePacket(). So only the
  // destructor can publish it.
  protos::gen::TracePacket raw_packet;
  raw_packet.mutable_for_testing()->set_str("last");
  const std::string raw_bytes = raw_packet.SerializeAsString();
  {
    auto packet = writer->NewTracePacket();
    packet.TakeStreamWriter()->WriteBytes(
        reinterpret_cast<const uint8_t*>(raw_bytes.data()), raw_bytes.size());
  }
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 0u);

  // The live WriterID keeps the SMB arbiter, and so the endpoint, alive.
  EXPECT_FALSE(smb_arbiter_->TryShutdown());
  writer.reset();
  EXPECT_TRUE(smb_arbiter_->TryShutdown());

  // No manual drain: the destructor's flush must deliver the packet.
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "last");
}

}  // namespace
}  // namespace perfetto::tracing_v2
