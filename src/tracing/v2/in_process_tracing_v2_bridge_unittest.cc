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

#include "src/tracing/v2/in_process_tracing_v2_bridge.h"

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace tracing_v2 {
namespace {

struct CapturedWriterState {
  std::vector<protos::gen::TracePacket> packets;
  bool flushed = false;
  uint32_t flush_count = 0;
  bool destroyed = false;
  // Set when the downstream writer was flushed before the user's flush
  // callback ran, which is the ordering Flush() has always promised.
  bool flushed_before_callback = false;
  // Invoked from NewTracePacket(), i.e. while the relay is forwarding.
  std::function<void()> on_packet_forwarded;
};

class CapturingTraceWriter : public TraceWriterForTesting {
 public:
  explicit CapturingTraceWriter(CapturedWriterState*);
  ~CapturingTraceWriter() override;

  WriterID writer_id() const override;
  void Flush(std::function<void()> callback) override;
  TracePacketHandle NewTracePacket() override;

 private:
  CapturedWriterState* const state_;
};

CapturingTraceWriter::CapturingTraceWriter(CapturedWriterState* state)
    : state_(state) {}

CapturingTraceWriter::~CapturingTraceWriter() {
  state_->packets = GetAllTracePackets();
  state_->destroyed = true;
}

WriterID CapturingTraceWriter::writer_id() const {
  return 7;
}

void CapturingTraceWriter::Flush(std::function<void()> callback) {
  state_->flushed = true;
  ++state_->flush_count;
  TraceWriterForTesting::Flush(std::move(callback));
}

TraceWriter::TracePacketHandle CapturingTraceWriter::NewTracePacket() {
  TracePacketHandle handle = TraceWriterForTesting::NewTracePacket();
  if (state_->on_packet_forwarded)
    state_->on_packet_forwarded();
  return handle;
}

TEST(InProcessTracingV2BridgeTest, WriterKeepsBridgeAliveThroughRetirement) {
  base::TestTaskRunner task_runner;
  CapturedWriterState captured;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, 16);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new CapturingTraceWriter(&captured)),
      /*target_buffer=*/2, BufferExhaustedPolicy::kDrop);

  // Model a producer endpoint being replaced while its SDK writer remains in
  // thread-local storage. The writer owns the relay lifetime from here on.
  bridge.reset();
  writer->NewTracePacket()->set_timestamp(42);
  bool flush_callback_called = false;
  writer->Flush([&flush_callback_called] { flush_callback_called = true; });
  writer.reset();

  EXPECT_FALSE(captured.destroyed);
  task_runner.RunUntilIdle();

  EXPECT_TRUE(captured.flushed);
  EXPECT_TRUE(flush_callback_called);
  EXPECT_TRUE(captured.destroyed);
  ASSERT_EQ(captured.packets.size(), 1u);
  EXPECT_EQ(captured.packets[0].timestamp(), 42u);
}

// The relay must not be torn down while the ring still holds packets. The
// writer's retirement and the deletion of the bridge are posted from different
// places, so teardown keeps draining rather than assuming one pass is enough.
//
// Note this covers the teardown path, not the case that motivated it: a drain
// pass stops after one ring lap, so an operation is only ever deferred when
// other writers are filling the ring concurrently, which a single-threaded
// test cannot stage deterministically.
TEST(InProcessTracingV2BridgeTest, TeardownDeliversPacketsStillInTheRing) {
  base::TestTaskRunner task_runner;
  CapturedWriterState captured;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, 16);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new CapturingTraceWriter(&captured)),
      /*target_buffer=*/2, BufferExhaustedPolicy::kDrop);

  // Nothing is drained yet: no task has run since the packets were written.
  for (uint64_t i = 1; i <= 8; ++i)
    writer->NewTracePacket()->set_timestamp(i);

  writer.reset();
  bridge.reset();
  task_runner.RunUntilIdle();

  EXPECT_TRUE(captured.destroyed);
  ASSERT_EQ(captured.packets.size(), 8u);
  for (uint64_t i = 0; i < 8; ++i)
    EXPECT_EQ(captured.packets[i].timestamp(), i + 1);
}

// A writer that finds a drain already scheduled posts nothing and relies on
// that pending pass to pick its chunk up. Both packets below are written
// before any task runs, so the second one is committed while the flag is
// already set and no second task exists to deliver it.
TEST(InProcessTracingV2BridgeTest, CoalescedCommitIsStillDrained) {
  base::TestTaskRunner task_runner;
  CapturedWriterState captured;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, 16);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new CapturingTraceWriter(&captured)),
      /*target_buffer=*/2, BufferExhaustedPolicy::kDrop);

  writer->NewTracePacket()->set_timestamp(1);  // Schedules the drain.
  writer->NewTracePacket()->set_timestamp(2);  // Coalesced into the same one.
  task_runner.RunUntilIdle();

  writer.reset();
  bridge.reset();
  task_runner.RunUntilIdle();

  ASSERT_EQ(captured.packets.size(), 2u);
  EXPECT_EQ(captured.packets[0].timestamp(), 1u);
  EXPECT_EQ(captured.packets[1].timestamp(), 2u);
}

// The flag is cleared before the ring is read, so a packet committed from
// inside the drain - here re-entrantly, while a forwarded packet is being
// handed to the downstream writer - schedules a fresh pass instead of being
// swallowed by the one already in flight.
TEST(InProcessTracingV2BridgeTest, CommitDuringDrainSchedulesAnotherPass) {
  base::TestTaskRunner task_runner;
  CapturedWriterState captured;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, 16);
  auto* const capturing = new CapturingTraceWriter(&captured);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(capturing), /*target_buffer=*/2,
      BufferExhaustedPolicy::kDrop);

  TraceWriter* const v2_writer = writer.get();
  InProcessTracingV2Bridge* const bridge_ptr = bridge.get();
  bool wrote_reentrantly = false;
  bool flag_clear_during_drain = false;
  captured.on_packet_forwarded = [&] {
    if (wrote_reentrantly)
      return;
    wrote_reentrantly = true;
    // The whole point: by the time the ring is being read the flag is already
    // clear, so the commit below posts a fresh pass instead of being dropped.
    flag_clear_during_drain = !bridge_ptr->drain_scheduled_for_testing();
    v2_writer->NewTracePacket()->set_timestamp(2);
  };

  writer->NewTracePacket()->set_timestamp(1);
  task_runner.RunUntilIdle();
  EXPECT_TRUE(wrote_reentrantly);
  EXPECT_TRUE(flag_clear_during_drain);

  writer.reset();
  bridge.reset();
  task_runner.RunUntilIdle();

  ASSERT_EQ(captured.packets.size(), 2u);
  EXPECT_EQ(captured.packets[0].timestamp(), 1u);
  EXPECT_EQ(captured.packets[1].timestamp(), 2u);
}

TEST(InProcessTracingV2BridgeTest, FlushCallbackRunsAfterDownstreamFlush) {
  base::TestTaskRunner task_runner;
  CapturedWriterState captured;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, 16);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new CapturingTraceWriter(&captured)),
      /*target_buffer=*/2, BufferExhaustedPolicy::kDrop);

  writer->NewTracePacket()->set_timestamp(1);
  bool flush_callback_called = false;
  writer->Flush([&captured, &flush_callback_called] {
    flush_callback_called = true;
    captured.flushed_before_callback = captured.flushed;
  });

  // Publishing to the local ring is not enough: the packet has to cross the
  // relay and the downstream v1 writer first.
  EXPECT_FALSE(flush_callback_called);
  task_runner.RunUntilIdle();

  EXPECT_TRUE(flush_callback_called);
  EXPECT_TRUE(captured.flushed_before_callback);

  writer.reset();
  bridge.reset();
  task_runner.RunUntilIdle();
}

// A v1 writer's packets are in the shared memory buffer as soon as they are
// written, so the producer can tell the service so at any point. A v2 packet
// sits in the ring until the relay moves it, and with the relay on its own
// thread nothing orders that against the flush and stop acknowledgements. This
// fence is what those acknowledgements wait behind; without it the service can
// read the buffer before the packet has arrived.
TEST(InProcessTracingV2BridgeTest, DrainThroughFencesForwarding) {
  // Declared before the relay: the bridge's deletion is posted to that thread
  // and destroys the downstream writer, which writes into |captured|, so the
  // thread has to be joined while these are still alive.
  CapturedWriterState captured;
  std::atomic<uint32_t> forwarded{0};
  captured.on_packet_forwarded = [&forwarded] {
    forwarded.fetch_add(1, std::memory_order_release);
  };
  base::ThreadTaskRunner relay =
      base::ThreadTaskRunner::CreateAndStart("test.relay");
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&relay, 16);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new CapturingTraceWriter(&captured)),
      /*target_buffer=*/2, BufferExhaustedPolicy::kDrop);

  for (uint64_t i = 1; i <= 4; ++i)
    writer->NewTracePacket()->set_timestamp(i);

  // The event is the test thread's way of waiting. Production callers never
  // block on the fence; that is the whole point of it being asynchronous.
  base::WaitableEvent fenced;
  uint32_t forwarded_at_completion = 0;
  bool completed_on_relay = false;
  bridge->DrainThrough(bridge->write_pos(), [&] {
    forwarded_at_completion = forwarded.load(std::memory_order_acquire);
    completed_on_relay = relay.RunsTasksOnCurrentThread();
    fenced.Notify();
  });
  fenced.Wait();

  EXPECT_EQ(forwarded_at_completion, 4u);
  EXPECT_TRUE(completed_on_relay);
  EXPECT_TRUE(captured.flushed);

  writer.reset();
  bridge.reset();
}

// The fence is a promise about ordering, not about there being work to do. A
// watermark the reader has already passed still completes, and still completes
// on the relay rather than under the caller.
TEST(InProcessTracingV2BridgeTest, DrainThroughCompletesWhenAlreadyDrained) {
  CapturedWriterState captured;
  base::ThreadTaskRunner relay =
      base::ThreadTaskRunner::CreateAndStart("test.relay");
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&relay, 16);

  base::WaitableEvent fenced;
  uint32_t completions = 0;
  bool completed_on_relay = false;
  bridge->DrainThrough(bridge->write_pos(), [&] {
    ++completions;
    completed_on_relay = relay.RunsTasksOnCurrentThread();
    fenced.Notify();
  });
  fenced.Wait();

  EXPECT_EQ(completions, 1u);
  EXPECT_TRUE(completed_on_relay);

  bridge.reset();
}

// The temporary v1 hop is flushed once per drain batch, not once per packet:
// otherwise a low-rate producer pays a downstream commit for every event.
TEST(InProcessTracingV2BridgeTest, OneAutomaticFlushPerWriterPerDrainBatch) {
  base::TestTaskRunner task_runner;
  CapturedWriterState captured;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, 16);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new CapturingTraceWriter(&captured)),
      /*target_buffer=*/2, BufferExhaustedPolicy::kDrop);

  for (uint64_t i = 1; i <= 8; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  task_runner.RunUntilIdle();

  EXPECT_EQ(captured.flush_count, 1u);

  writer.reset();
  bridge.reset();
  task_runner.RunUntilIdle();
}

// An explicit writer flush already commits everything the pass forwarded, so
// the automatic batch flush must skip that writer rather than commit it twice.
TEST(InProcessTracingV2BridgeTest, ExplicitFlushDoesNotDuplicateTheBatchFlush) {
  base::TestTaskRunner task_runner;
  CapturedWriterState captured;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, 16);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new CapturingTraceWriter(&captured)),
      /*target_buffer=*/2, BufferExhaustedPolicy::kDrop);

  writer->NewTracePacket()->set_timestamp(1);
  uint32_t callbacks = 0;
  writer->Flush([&callbacks] { ++callbacks; });
  task_runner.RunUntilIdle();

  EXPECT_EQ(captured.flush_count, 1u);
  EXPECT_EQ(callbacks, 1u);

  writer.reset();
  bridge.reset();
  task_runner.RunUntilIdle();
}

// Several flushes queued for one writer keep one downstream flush each, which
// is what keeps their callbacks in the order they were requested.
TEST(InProcessTracingV2BridgeTest, MultipleFlushCallbacksStayInOrder) {
  base::TestTaskRunner task_runner;
  CapturedWriterState captured;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, 16);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new CapturingTraceWriter(&captured)),
      /*target_buffer=*/2, BufferExhaustedPolicy::kDrop);

  std::vector<int> order;
  for (int i = 1; i <= 3; ++i) {
    writer->NewTracePacket()->set_timestamp(static_cast<uint64_t>(i));
    writer->Flush([&order, i] { order.push_back(i); });
  }
  task_runner.RunUntilIdle();

  EXPECT_EQ(order, std::vector<int>({1, 2, 3}));

  writer.reset();
  bridge.reset();
  task_runner.RunUntilIdle();
}

}  // namespace
}  // namespace tracing_v2
}  // namespace perfetto
