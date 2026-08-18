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

#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/base/test/test_task_runner.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

// Stands in for the v1 writer the relay forwards into. Its WriterID is what the
// bridge reuses in the v2 chunk header, so it has to be settable.
class FakeDownstreamWriter : public TraceWriter {
 public:
  struct Recorded {
    std::vector<protos::gen::TracePacket> packets;
    uint32_t flushes = 0;
    // A v1 writer hands its WriterID back to its arbiter when it is destroyed,
    // so when and how often that happens is part of the contract, not a
    // detail.
    uint32_t destructions = 0;
    // Called from inside NewTracePacket(), i.e. while the relay is part way
    // through a drain pass. A seam for making something happen at exactly that
    // moment, with no second thread and nothing to time.
    std::function<void()> on_forward;
  };

  FakeDownstreamWriter(WriterID writer_id, Recorded* recorded)
      : writer_id_(writer_id), recorded_(recorded) {}

  ~FakeDownstreamWriter() override { ++recorded_->destructions; }

  TracePacketHandle NewTracePacket() override {
    if (recorded_->on_forward)
      recorded_->on_forward();
    FinishTracePacket();
    buffer_.Reset();
    open_ = true;
    return TracePacketHandle(buffer_.get());
  }

  void FinishTracePacket() override {
    if (!open_)
      return;
    open_ = false;
    const std::vector<uint8_t> bytes = buffer_.SerializeAsArray();
    protos::gen::TracePacket packet;
    EXPECT_TRUE(packet.ParseFromArray(bytes.data(), bytes.size()));
    recorded_->packets.push_back(std::move(packet));
  }

  void Flush(std::function<void()> callback = {}) override {
    FinishTracePacket();
    ++recorded_->flushes;
    if (callback)
      callback();
  }

  WriterID writer_id() const override { return writer_id_; }
  uint64_t written() const override { return 0; }
  uint64_t drop_count() const override { return 0; }

 private:
  const WriterID writer_id_;
  Recorded* const recorded_;
  protozero::HeapBuffered<protos::pbzero::TracePacket> buffer_;
  bool open_ = false;
};

// A stand-in for the relay: another sequence, which queues what it is given and
// runs it only when the test says so. The queue-and-run part is what makes "one
// task for the whole burst" observable at all - a runner that ran tasks as they
// were posted would never have more than one queued - and reporting a different
// sequence is what makes "this did not run here" observable.
class QueuedTaskRunner : public base::TaskRunner {
 public:
  void PostTask(std::function<void()> task) override {
    ++posts_;
    queued_.push_back(std::move(task));
  }
  void PostDelayedTask(std::function<void()> task, uint32_t) override {
    PostTask(std::move(task));
  }
  void AddFileDescriptorWatch(base::PlatformHandle,
                              std::function<void()>) override {}
  void RemoveFileDescriptorWatch(base::PlatformHandle) override {}
  bool RunsTasksOnCurrentThread() const override { return false; }

  size_t posts() const { return posts_; }

  // Runs everything queued, including whatever those tasks queue in turn.
  void RunQueuedTasks() {
    while (!queued_.empty()) {
      std::vector<std::function<void()>> batch;
      batch.swap(queued_);
      for (std::function<void()>& task : batch)
        task();
    }
  }

 private:
  size_t posts_ = 0;
  std::vector<std::function<void()>> queued_;
};

// A task runner that queues tasks and never runs them, which is what one that
// is being shut down does with everything still in its queue: the loop returns
// and the pending callbacks are destroyed where they stand. Nothing else here
// needs a task runner to behave, so this is the whole of it.
class DiscardingTaskRunner : public base::TaskRunner {
 public:
  void PostTask(std::function<void()> task) override {
    queued_.push_back(std::move(task));
  }
  void PostDelayedTask(std::function<void()> task, uint32_t) override {
    queued_.push_back(std::move(task));
  }
  void AddFileDescriptorWatch(base::PlatformHandle,
                              std::function<void()>) override {}
  void RemoveFileDescriptorWatch(base::PlatformHandle) override {}
  bool RunsTasksOnCurrentThread() const override { return true; }

  size_t queued() const { return queued_.size(); }
  void DiscardQueuedTasks() { queued_.clear(); }

 private:
  std::vector<std::function<void()>> queued_;
};

class InProcessTracingV2BridgeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bridge_ =
        InProcessTracingV2Bridge::Create(&task_runner_, kNumChunks, kChunkSize);
    ASSERT_NE(bridge_, nullptr);
  }

  void TearDown() override {
    bridge_.reset();
    // The bridge's deleter posts its drain-and-delete; run it before the task
    // runner goes away.
    task_runner_.RunUntilIdle();
  }

  // Waits until everything currently in the ring has been forwarded. Positive
  // progress gets an explicit completion; RunUntilIdle() is for proving that
  // something does *not* happen.
  void DrainRelay() {
    const std::string name = "relay-quiescent-" + std::to_string(++drains_);
    std::function<void()> quiescent = task_runner_.CreateCheckpoint(name);
    bridge_->DrainToQuiescenceForTesting(std::move(quiescent));
    task_runner_.RunUntilCheckpoint(name);
  }

  std::unique_ptr<TraceWriter> CreateWriter(
      WriterID writer_id,
      BufferID target_buffer,
      BufferExhaustedPolicy policy = BufferExhaustedPolicy::kDrop) {
    auto downstream = std::unique_ptr<TraceWriter>(
        new FakeDownstreamWriter(writer_id, &recorded_[writer_id]));
    return bridge_->CreateTraceWriter(std::move(downstream), target_buffer,
                                      policy);
  }

  static constexpr uint32_t kNumChunks = 32;
  static constexpr uint32_t kChunkSize = 256;

  base::TestTaskRunner task_runner_;
  std::shared_ptr<InProcessTracingV2Bridge> bridge_;
  std::map<WriterID, FakeDownstreamWriter::Recorded> recorded_;
  uint32_t drains_ = 0;
};

TEST_F(InProcessTracingV2BridgeTest, ForwardsAPacketAsCanonicalProtobuf) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  {
    auto packet = writer->NewTracePacket();
    packet->set_timestamp(1234);
    packet->set_for_testing()->set_str("hello");
  }
  writer->Flush();
  DrainRelay();

  ASSERT_EQ(recorded_[7].packets.size(), 1u);
  EXPECT_EQ(recorded_[7].packets[0].timestamp(), 1234u);
  EXPECT_EQ(recorded_[7].packets[0].for_testing().str(), "hello");
  // One commit for the batch, not one per packet.
  EXPECT_EQ(recorded_[7].flushes, 1u);
  EXPECT_EQ(bridge_->num_packets_forwarded_for_testing(), 1u);
  EXPECT_EQ(bridge_->num_malformed_packets_for_testing(), 0u);
}

TEST_F(InProcessTracingV2BridgeTest, WriterWithNoIdStaysOnV1) {
  // A WriterID of zero means the arbiter ran out of them: there is nothing to
  // put in the chunk header and nothing to route back by.
  auto downstream =
      std::unique_ptr<TraceWriter>(new FakeDownstreamWriter(0, &recorded_[0]));
  TraceWriter* const raw = downstream.get();
  std::unique_ptr<TraceWriter> writer = bridge_->CreateTraceWriter(
      std::move(downstream), 11, BufferExhaustedPolicy::kDrop);
  EXPECT_EQ(writer.get(), raw);
}

TEST_F(InProcessTracingV2BridgeTest, PacketSpanningChunksIsReassembled) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  const std::string payload(2048, 'x');
  {
    auto packet = writer->NewTracePacket();
    packet->set_timestamp(5);
    packet->set_for_testing()->set_str(payload);
  }
  writer->Flush();
  DrainRelay();

  ASSERT_EQ(recorded_[7].packets.size(), 1u);
  EXPECT_EQ(recorded_[7].packets[0].timestamp(), 5u);
  EXPECT_EQ(recorded_[7].packets[0].for_testing().str(), payload);
  // Reassembled from many fragments, but exactly one packet came out and the
  // rewrite of its private framing never failed.
  EXPECT_EQ(bridge_->num_packets_forwarded_for_testing(), 1u);
  EXPECT_EQ(bridge_->num_malformed_packets_for_testing(), 0u);
}

TEST_F(InProcessTracingV2BridgeTest, PacketsKeepTheirOrderAndTheirWriter) {
  std::unique_ptr<TraceWriter> first = CreateWriter(7, 11);
  std::unique_ptr<TraceWriter> second = CreateWriter(9, 22);
  for (uint32_t i = 0; i < 20; ++i) {
    {
      first->NewTracePacket()->set_timestamp(100 + i);
    }
    {
      second->NewTracePacket()->set_timestamp(200 + i);
    }
  }
  first->Flush();
  second->Flush();
  DrainRelay();

  ASSERT_EQ(recorded_[7].packets.size(), 20u);
  ASSERT_EQ(recorded_[9].packets.size(), 20u);
  for (uint32_t i = 0; i < 20; ++i) {
    EXPECT_EQ(recorded_[7].packets[i].timestamp(), 100 + i) << i;
    EXPECT_EQ(recorded_[9].packets[i].timestamp(), 200 + i) << i;
  }
}

// The token, not the packet rate, is what bounds how often the relay is woken:
// a commit that finds a task already queued must post nothing at all. Counted
// exactly, on a runner that does nothing but count, because the number of
// posted tasks is the whole claim.
TEST(InProcessTracingV2BridgeBurstTest, CommitsWhileATaskIsQueuedPostNothing) {
  QueuedTaskRunner task_runner;
  std::map<WriterID, FakeDownstreamWriter::Recorded> recorded;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, /*num_chunks=*/32,
                                       /*chunk_size=*/256);
  ASSERT_NE(bridge, nullptr);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new FakeDownstreamWriter(7, &recorded[7])),
      /*target_buffer=*/11, BufferExhaustedPolicy::kDrop);

  const size_t posts_before = task_runner.posts();
  for (uint32_t i = 0; i < 200; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  writer->FinishTracePacket();

  // Two hundred packets, one task.
  EXPECT_EQ(task_runner.posts() - posts_before, 1u);
  EXPECT_TRUE(bridge->drain_scheduled_for_testing());

  task_runner.RunQueuedTasks();
  EXPECT_FALSE(bridge->drain_scheduled_for_testing());
  ASSERT_EQ(recorded[7].packets.size(), 200u);
  for (uint32_t i = 0; i < 200; ++i)
    EXPECT_EQ(recorded[7].packets[i].timestamp(), i) << i;

  writer.reset();
  bridge.reset();
  task_runner.RunQueuedTasks();
}

// The token is cleared before the ring is read, so a commit landing mid-pass
// schedules a fresh one instead of being swallowed.
TEST_F(InProcessTracingV2BridgeTest, CommitDuringDrainSchedulesAnotherPass) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();
  ASSERT_TRUE(bridge_->drain_scheduled_for_testing());

  // Run the pending drain, and while it is running - which is exactly when the
  // flag is already clear - publish another packet.
  task_runner_.PostTask([&] {
    EXPECT_FALSE(bridge_->drain_scheduled_for_testing());
    writer->NewTracePacket()->set_timestamp(2);
    writer->FinishTracePacket();
    EXPECT_TRUE(bridge_->drain_scheduled_for_testing());
  });
  DrainRelay();

  ASSERT_EQ(recorded_[7].packets.size(), 2u);
  EXPECT_EQ(recorded_[7].packets[0].timestamp(), 1u);
  EXPECT_EQ(recorded_[7].packets[1].timestamp(), 2u);
}

TEST_F(InProcessTracingV2BridgeTest, DrainToQuiescenceForwardsEverything) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  for (uint32_t i = 0; i < 50; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  writer->FinishTracePacket();

  bool done = false;
  std::function<void()> quiescent = task_runner_.CreateCheckpoint("quiescent");
  bridge_->DrainToQuiescenceForTesting([&, quiescent] {
    done = true;
    quiescent();
  });
  task_runner_.RunUntilCheckpoint("quiescent");

  EXPECT_TRUE(done);
  EXPECT_EQ(recorded_[7].packets.size(), 50u);
}

// The watermark is an upper bound for the operation that sampled it. Packets
// published after the sample - which really happens, because the relay flushes
// downstream writers from inside its own drain and that runs arbitrary caller
// code - belong to whatever drains next, not to this completion.
TEST_F(InProcessTracingV2BridgeTest, DrainToQuiescenceStopsAtItsWatermark) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  for (uint32_t i = 0; i < 50; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  writer->Flush();

  // The first commit already queued an ordinary drain, so the queue ends up as
  // [ordinary drain, this task, quiescence]. Publishing from the task in the
  // middle is what leaves the quiescence pass something to do, and arming from
  // it is what puts the publication below *inside* that pass. No second thread,
  // and nothing to time.
  bool armed = false;
  task_runner_.PostTask([&] {
    armed = true;
    writer->NewTracePacket()->set_timestamp(500);
    writer->Flush();
  });

  size_t packets_at_completion = 0;
  bool done = false;
  bridge_->DrainToQuiescenceForTesting([&] {
    done = true;
    packets_at_completion = recorded_[7].packets.size();
  });

  recorded_[7].on_forward = [&] {
    if (!armed)
      return;
    armed = false;
    // Give up the chunk the writer is holding first, so the new packet has to
    // reserve a position of its own rather than being appended inside one the
    // watermark already covers.
    writer->Flush();
    writer->NewTracePacket()->set_timestamp(999);
    writer->Flush();
  };

  task_runner_.RunUntilIdle();

  EXPECT_TRUE(done);
  // Exactly what was in the ring when the watermark was taken: the fifty
  // packets and the one the middle task published, and not the one published
  // while the drain was running.
  EXPECT_EQ(packets_at_completion, 51u);
  // And that one is not lost either; the ordinary drain picks it up.
  ASSERT_EQ(recorded_[7].packets.size(), 52u);
  EXPECT_EQ(recorded_[7].packets[50].timestamp(), 500u);
  EXPECT_EQ(recorded_[7].packets[51].timestamp(), 999u);
}

TEST_F(InProcessTracingV2BridgeTest, DataLossIsReportedOnTheNextPacket) {
  // A ring this small fills immediately, so the writer has to drop.
  bridge_ = InProcessTracingV2Bridge::Create(&task_runner_, /*num_chunks=*/2,
                                             /*chunk_size=*/256);
  ASSERT_NE(bridge_, nullptr);
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);

  const std::string payload(200, 'y');
  for (uint32_t i = 0; i < 40; ++i) {
    auto packet = writer->NewTracePacket();
    packet->set_timestamp(i);
    packet->set_for_testing()->set_str(payload);
  }
  writer->Flush();
  EXPECT_GT(writer->drop_count(), 0u);

  // Draining frees capacity, so the next packet gets in - and it is the one
  // that carries the gap report, exactly as previous_packet_dropped is defined.
  DrainRelay();
  const size_t before_recovery = recorded_[7].packets.size();
  ASSERT_GT(before_recovery, 0u);
  EXPECT_LT(before_recovery, 40u);
  for (const protos::gen::TracePacket& packet : recorded_[7].packets)
    EXPECT_EQ(packet.previous_packet_dropped(), 0u);

  writer->NewTracePacket()->set_timestamp(999);
  writer->Flush();
  DrainRelay();

  ASSERT_EQ(recorded_[7].packets.size(), before_recovery + 1);
  const protos::gen::TracePacket& recovered = recorded_[7].packets.back();
  EXPECT_EQ(recovered.timestamp(), 999u);
  EXPECT_NE(recovered.previous_packet_dropped() &
                protos::gen::TracePacket::DATA_LOSS_PRESENT,
            0u);
  EXPECT_NE(recovered.previous_packet_dropped() &
                protos::gen::TracePacket::DATA_LOSS_SMB_FULL,
            0u);
}

TEST_F(InProcessTracingV2BridgeTest, TeardownDrainsWhatIsStillInTheRing) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  for (uint32_t i = 0; i < 10; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  writer->FinishTracePacket();
  // Nothing has run yet: the packets are in the ring and nowhere else.
  ASSERT_TRUE(recorded_[7].packets.empty());

  // Drop the writer, then the last reference. The deleter posts a task that has
  // to drain before the bridge goes away.
  writer.reset();
  bridge_.reset();
  task_runner_.RunUntilIdle();

  EXPECT_EQ(recorded_[7].packets.size(), 10u);
}

TEST_F(InProcessTracingV2BridgeTest, WriterOutlivingTheBridgeReferenceIsSafe) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  // The writer holds a shared reference to the bridge, so dropping ours does
  // not destroy anything yet.
  bridge_.reset();
  writer->NewTracePacket()->set_timestamp(1);
  writer->Flush();
  task_runner_.RunUntilIdle();
  EXPECT_EQ(recorded_[7].packets.size(), 1u);

  writer.reset();
  task_runner_.RunUntilIdle();
}

// Teardown must not block on the relay. A drain already in flight can be inside
// a downstream v1 writer waiting for the muxer to pump the arbiter, so a muxer
// that waited here would be waiting for itself.
//
// What the caller does get synchronously is the guarantee that the set of
// retained writers is closed: after this returns, no writer will be accepted,
// so nothing new can appear behind the release.
TEST(InProcessTracingV2BridgeLifetimeTest, ReleaseDoesNotWaitForTheRelay) {
  QueuedTaskRunner task_runner;
  std::map<WriterID, FakeDownstreamWriter::Recorded> recorded;
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, /*num_chunks=*/32,
                                       /*chunk_size=*/256);
  ASSERT_NE(bridge, nullptr);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new FakeDownstreamWriter(7, &recorded[7])),
      /*target_buffer=*/11, BufferExhaustedPolicy::kDrop);
  ASSERT_NE(writer, nullptr);

  // Nothing on this task runner has run, and nothing here runs it: the writer
  // is still retained when the call returns.
  bridge->StartReleasingDownstreamWriters();
  EXPECT_TRUE(bridge->holds_downstream_writers());
  EXPECT_EQ(recorded[7].destructions, 0u);

  // The set is closed straight away, though: a writer created now stays on v1
  // rather than being attached to a relay that is stopping.
  auto* raw_late = new FakeDownstreamWriter(8, &recorded[8]);
  std::unique_ptr<TraceWriter> late = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(raw_late), /*target_buffer=*/11,
      BufferExhaustedPolicy::kDrop);
  EXPECT_EQ(late.get(), raw_late);

  task_runner.RunQueuedTasks();
  EXPECT_FALSE(bridge->holds_downstream_writers());
  EXPECT_EQ(recorded[7].destructions, 1u);
  // The refused one is the caller's; it goes when the caller drops it.
  EXPECT_EQ(recorded[8].destructions, 0u);
  late.reset();
  EXPECT_EQ(recorded[8].destructions, 1u);

  writer.reset();
  bridge.reset();
  task_runner.RunQueuedTasks();
  // Released exactly once, whichever path got there.
  EXPECT_EQ(recorded[7].destructions, 1u);
}

// Creating a writer and stopping the bridge race in production: creation runs
// on arbitrary SDK threads, the release is asked for on the muxer sequence. The
// two have to have one order. Whichever wins, the downstream v1 writer must be
// destroyed exactly once - by the bridge if it was accepted, by the caller if
// it was handed back - because a writer nobody destroys holds a WriterID in an
// arbiter that can then never shut down.
TEST(InProcessTracingV2BridgeLifetimeTest, CreateRacingWithReleaseHasOneOrder) {
  constexpr uint32_t kRounds = 200;
  for (uint32_t round = 0; round < kRounds; ++round) {
    QueuedTaskRunner task_runner;
    std::map<WriterID, FakeDownstreamWriter::Recorded> recorded;
    std::shared_ptr<InProcessTracingV2Bridge> bridge =
        InProcessTracingV2Bridge::Create(&task_runner, /*num_chunks=*/8,
                                         /*chunk_size=*/256);
    ASSERT_NE(bridge, nullptr);

    auto* raw = new FakeDownstreamWriter(7, &recorded[7]);
    std::unique_ptr<TraceWriter> created;
    base::WaitableEvent go;
    std::thread creator([&] {
      go.Wait();
      created = bridge->CreateTraceWriter(std::unique_ptr<TraceWriter>(raw),
                                          /*target_buffer=*/11,
                                          BufferExhaustedPolicy::kDrop);
    });
    // The release is queued and then run while the creator is still going, so
    // the insert and the move of the map really are in flight together.
    bridge->StartReleasingDownstreamWriters();
    go.Notify();
    task_runner.RunQueuedTasks();
    creator.join();

    task_runner.RunQueuedTasks();
    ASSERT_NE(created, nullptr);
    if (created.get() == raw) {
      // The release won: the writer was refused and is the caller's.
      EXPECT_EQ(recorded[7].destructions, 0u) << "round " << round;
      created.reset();
    } else {
      // The creation won: the bridge accepted it, so the release took it.
      EXPECT_EQ(recorded[7].destructions, 1u) << "round " << round;
      created.reset();
    }
    EXPECT_EQ(recorded[7].destructions, 1u) << "round " << round;

    bridge.reset();
    task_runner.RunQueuedTasks();
    // Still exactly once: teardown does not free it a second time.
    EXPECT_EQ(recorded[7].destructions, 1u) << "round " << round;
  }
}

// The bridge's deferred deletion has to own the bridge. The last reference can
// go away on any SDK thread, so deletion is handed to the relay; if the relay
// is being shut down at that moment, the task is destroyed without ever
// running. A task that held only a raw pointer would take the last owner with
// it and leave the bridge - and every downstream v1 writer it retains, each
// still holding a WriterID belonging to an arbiter that is going away -
// unreachable and undestroyed.
TEST(InProcessTracingV2BridgeLifetimeTest,
     DeletionTaskDiscardedAtShutdownStillDestroysEverything) {
  DiscardingTaskRunner task_runner;
  std::map<WriterID, FakeDownstreamWriter::Recorded> recorded;

  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, /*num_chunks=*/32,
                                       /*chunk_size=*/256);
  ASSERT_NE(bridge, nullptr);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new FakeDownstreamWriter(7, &recorded[7])),
      /*target_buffer=*/11, BufferExhaustedPolicy::kDrop);
  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();

  writer.reset();
  bridge.reset();
  // Nothing has run, so the deletion is still sitting in the queue and the
  // downstream writer is still alive inside the bridge.
  ASSERT_GT(task_runner.queued(), 0u);
  ASSERT_EQ(recorded[7].destructions, 0u);

  task_runner.DiscardQueuedTasks();
  EXPECT_EQ(recorded[7].destructions, 1u);
}

// The same ownership rule, one step further along: a bridge whose ring still
// has data reschedules its deletion, and the reschedule must carry the
// ownership rather than hand a raw pointer to the next task.
TEST(InProcessTracingV2BridgeLifetimeTest,
     DiscardedDeletionRetryStillDestroysEverything) {
  DiscardingTaskRunner task_runner;
  std::map<WriterID, FakeDownstreamWriter::Recorded> recorded;

  // Two chunks and a writer that fills them: the deletion task drains what it
  // can and comes back for the rest.
  std::shared_ptr<InProcessTracingV2Bridge> bridge =
      InProcessTracingV2Bridge::Create(&task_runner, /*num_chunks=*/2,
                                       /*chunk_size=*/256);
  ASSERT_NE(bridge, nullptr);
  std::unique_ptr<TraceWriter> writer = bridge->CreateTraceWriter(
      std::unique_ptr<TraceWriter>(new FakeDownstreamWriter(7, &recorded[7])),
      /*target_buffer=*/11, BufferExhaustedPolicy::kDrop);
  for (uint32_t i = 0; i < 4; ++i) {
    writer->NewTracePacket()->set_timestamp(i);
    writer->FinishTracePacket();
  }
  writer.reset();
  bridge.reset();
  ASSERT_EQ(recorded[7].destructions, 0u);

  // Nothing else owns the bridge from here on. Whether the queue is discarded
  // now or after any number of retries, it is destroyed exactly once.
  task_runner.DiscardQueuedTasks();
  EXPECT_EQ(recorded[7].destructions, 1u);
}

}  // namespace
}  // namespace perfetto::tracing_v2
