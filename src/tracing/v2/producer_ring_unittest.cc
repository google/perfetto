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

#include "src/tracing/v2/producer_ring.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#include <functional>
#include <utility>
#include <vector>

#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace tracing_v2 {
namespace {

// Records NotifyRingData() calls. Tests invoke their callbacks to simulate
// service drain completion.
class RecordingChannel : public ProducerRing::ServiceChannel {
 public:
  void NotifyRingData(std::function<void()> on_picked_up) override {
    ++notify_count;
    pending_.push_back(std::move(on_picked_up));
  }
  void RetireWriter(WriterID, std::function<void(bool)> callback) override {
    NotifyRingData([cb = std::move(callback)] { cb(true); });
  }

  // Runs the outstanding pick-up callbacks, as if the service had drained.
  void PickUpAll() {
    std::vector<std::function<void()>> pending;
    pending.swap(pending_);
    for (auto& cb : pending) {
      if (cb)
        cb();
    }
  }

  int notify_count = 0;

 private:
  std::vector<std::function<void()>> pending_;
};

std::shared_ptr<SharedMemory> MakeMemory(uint32_t num_chunks,
                                         uint32_t chunk_size) {
  return InProcessSharedMemory::Create(
      ProducerRing::LogicalSize(num_chunks, chunk_size));
}

// Casts the ring to its writer-facing delegate to drive the callbacks directly.
tracing_v2::SharedRingBufferWriter::Delegate* AsWriterDelegate(
    ProducerRing* ring) {
  return static_cast<tracing_v2::SharedRingBufferWriter::Delegate*>(ring);
}
TraceWriterV2Impl::Delegate* AsV2Delegate(ProducerRing* ring) {
  return static_cast<TraceWriterV2Impl::Delegate*>(ring);
}

TEST(ProducerRingTest, AllocatesDistinctWriterIds) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(4, 256), 4, 256, &channel);
  ASSERT_TRUE(ring);
  auto w1 = ring->CreateTraceWriter(BufferID(1), BufferExhaustedPolicy::kDrop);
  auto w2 = ring->CreateTraceWriter(BufferID(1), BufferExhaustedPolicy::kDrop);
  auto w3 = ring->CreateTraceWriter(BufferID(2), BufferExhaustedPolicy::kDrop);
  EXPECT_EQ(w1->writer_id(), 1u);
  EXPECT_EQ(w2->writer_id(), 2u);
  EXPECT_EQ(w3->writer_id(), 3u);
}

TEST(ProducerRingTest, WriterIdsWaitForRetirementAndCanBeReused) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(4, 256), 4, 256, &channel);
  for (uint32_t i = 0; i < kMaxWriterID; ++i) {
    auto writer = ring->CreateTraceWriter(1, BufferExhaustedPolicy::kDrop);
    ASSERT_NE(writer->writer_id(), 0u);
  }
  EXPECT_EQ(
      ring->CreateTraceWriter(1, BufferExhaustedPolicy::kDrop)->writer_id(),
      0u);
  channel.PickUpAll();
  EXPECT_NE(
      ring->CreateTraceWriter(1, BufferExhaustedPolicy::kDrop)->writer_id(),
      0u);
}

TEST(ProducerRingTest, ActualPacketsCoalesceNotifications) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(64, 4096), 64, 4096, &channel);
  auto writer = ring->CreateTraceWriter(1, BufferExhaustedPolicy::kDrop);
  for (uint32_t i = 0; i < 1000; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  EXPECT_EQ(channel.notify_count, 1);
  channel.PickUpAll();
  EXPECT_EQ(channel.notify_count, 2);
  channel.PickUpAll();
  EXPECT_EQ(channel.notify_count, 2);
}

TEST(ProducerRingTest, OfflineRingAttachesOnceAndDropsInsteadOfWaiting) {
  auto ring = ProducerRing::Create(MakeMemory(2, 256), 2, 256, nullptr);
  ASSERT_TRUE(ring);
  EXPECT_TRUE(AsWriterDelegate(ring.get())->DrainRunsOnCurrentThread());
  auto writer = ring->CreateTraceWriter(1, BufferExhaustedPolicy::kStall);
  writer->NewTracePacket()->set_timestamp(42);
  RecordingChannel channel;
  EXPECT_TRUE(ring->AttachToService(&channel));
  EXPECT_EQ(channel.notify_count, 1);
  EXPECT_FALSE(ring->AttachToService(&channel));
  ring->DetachFromService();
  EXPECT_FALSE(ring->AttachToService(&channel));
}

TEST(ProducerRingTest, RejectsInvalidGeometry) {
  RecordingChannel channel;
  // Not a power-of-two chunk count.
  EXPECT_FALSE(ProducerRing::Create(MakeMemory(2, 256), 3, 256, &channel));
  // chunk_size below the minimum.
  EXPECT_FALSE(ProducerRing::Create(MakeMemory(2, 256), 2, 128, &channel));
  // chunk_size not a multiple of the required alignment.
  EXPECT_FALSE(ProducerRing::Create(MakeMemory(2, 256), 2, 258, &channel));
  // Backing memory too small for the geometry. Allocations round up to a page,
  // so ask for a geometry whose logical extent clearly exceeds one page.
  EXPECT_FALSE(ProducerRing::Create(InProcessSharedMemory::Create(4096), 64,
                                    4096, &channel));
}

TEST(ProducerRingTest, NotifiesServiceOnEachBackpressure) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(4, 256), 4, 256, &channel);
  ASSERT_TRUE(ring);
  auto* delegate = AsWriterDelegate(ring.get());

  // Each NotifyReader() call must request a drain without coalescing.
  // A writer that fills the ring repeatedly needs a drain for each fill.
  // The production in-process channel can drain inline. This fake counts calls.
  delegate->NotifyReader();
  delegate->NotifyReader();
  delegate->NotifyReader();
  EXPECT_EQ(channel.notify_count, 3);
}

TEST(ProducerRingTest, CoalescesBatchedNotifications) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(4, 256), 4, 256, &channel);
  ASSERT_TRUE(ring);
  auto* delegate = AsWriterDelegate(ring.get());

  // Five steady-state notifications with no drain in between collapse into a
  // single outstanding notification (unlike the progress-critical path above).
  for (int i = 0; i < 5; i++)
    delegate->NotifyReaderBatched();
  EXPECT_EQ(channel.notify_count, 1);

  // The drain completes. Packets kept finalizing while it was in flight, so the
  // ack sends exactly one recheck notification, not four more.
  channel.PickUpAll();
  EXPECT_EQ(channel.notify_count, 2);

  // The recheck drained with nothing new pending, so the chain stops.
  channel.PickUpAll();
  EXPECT_EQ(channel.notify_count, 2);
}

TEST(ProducerRingTest, BatchedRecheckDoesNotStrandLatePacket) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(4, 256), 4, 256, &channel);
  ASSERT_TRUE(ring);
  auto* delegate = AsWriterDelegate(ring.get());

  delegate->NotifyReaderBatched();  // Notification in flight.
  EXPECT_EQ(channel.notify_count, 1);
  delegate->NotifyReaderBatched();  // Arrives while in flight: marks dirty.
  EXPECT_EQ(channel.notify_count, 1);
  channel.PickUpAll();  // Ack rechecks dirty and drains the late packet.
  EXPECT_EQ(channel.notify_count, 2);
  channel.PickUpAll();  // Nothing pending now.
  EXPECT_EQ(channel.notify_count, 2);
}

TEST(ProducerRingTest, FlushNotifiesWithCallback) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(4, 256), 4, 256, &channel);
  ASSERT_TRUE(ring);

  bool flushed = false;
  AsV2Delegate(ring.get())->Flush(WriterID(1), [&flushed] { flushed = true; });
  EXPECT_EQ(channel.notify_count, 1);
  EXPECT_FALSE(flushed);  // Completes only once the service has drained.
  channel.PickUpAll();
  EXPECT_TRUE(flushed);
}

TEST(ProducerRingTest, DetachStopsServiceNotifications) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(4, 256), 4, 256, &channel);
  ASSERT_TRUE(ring);
  auto* delegate = AsWriterDelegate(ring.get());

  ring->DetachFromService();
  delegate->NotifyReader();
  EXPECT_EQ(channel.notify_count, 0);  // No calls after detach.

  // An unavailable service must not acknowledge this flush.
  bool flushed = false;
  AsV2Delegate(ring.get())->Flush(WriterID(1), [&flushed] { flushed = true; });
  EXPECT_FALSE(flushed);
}

TEST(ProducerRingTest, SurvivingWriterKeepsRingAlive) {
  RecordingChannel channel;
  auto ring = ProducerRing::Create(MakeMemory(4, 256), 4, 256, &channel);
  ASSERT_TRUE(ring);
  auto writer =
      ring->CreateTraceWriter(BufferID(1), BufferExhaustedPolicy::kDrop);
  SharedRingBuffer* rb = ring->ring_buffer();
  ring.reset();  // The writer retains its own reference.
  // The ring view is still valid: the writer holds the ProducerRing alive.
  EXPECT_EQ(rb->num_chunks(), 4u);
  writer.reset();  // The last reference releases the ring.
}

}  // namespace
}  // namespace tracing_v2
}  // namespace perfetto
