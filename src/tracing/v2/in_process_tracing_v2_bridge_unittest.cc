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

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/v2/relay_sequence.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {

class InProcessTracingV2BridgeTestPeer {
 public:
  static std::unique_ptr<TraceWriter> CreateTraceWriter(
      InProcessTracingV2Bridge* bridge,
      std::unique_ptr<TraceWriter> v1_writer,
      BufferID target_buffer,
      BufferExhaustedPolicy policy) {
    return bridge->CreateTraceWriter(std::move(v1_writer), target_buffer,
                                     policy);
  }
};

namespace {

// Fake for the v1 writer the bridge forwards into. The bridge takes the
// WriterID from it, hence the ctor argument.
class FakeV1Writer : public TraceWriter {
 public:
  struct Recorded {
    std::vector<protos::gen::TracePacket> packets;
    uint32_t flushes = 0;
    bool drop_packets = false;
    uint64_t drop_count = 0;
    // Destroying the v1 writer hands its WriterID back to the arbiter, so the
    // tests check when, and how many times, that happens.
    uint32_t destructions = 0;
    // Called from NewTracePacket(), i.e. while the relay is in the middle of a
    // drain pass. Lets a test inject work at that exact point without threads
    // or timing.
    std::function<void()> on_forward;
    // Called by a control barrier when it flushes the downstream writer.
    std::function<void()> on_flush;
    bool defer_flush_callbacks = false;
    std::vector<std::function<void()>> pending_flush_callbacks;

    void AcknowledgeFlushes() {
      std::vector<std::function<void()>> callbacks;
      callbacks.swap(pending_flush_callbacks);
      for (std::function<void()>& callback : callbacks)
        callback();
    }
  };

  FakeV1Writer(WriterID writer_id, Recorded* recorded)
      : writer_id_(writer_id), recorded_(recorded) {}

  ~FakeV1Writer() override { ++recorded_->destructions; }

  TracePacketHandle NewTracePacket() override {
    if (recorded_->on_forward)
      recorded_->on_forward();
    FinishTracePacket();
    buffer_.Reset();
    if (recorded_->drop_packets && !dropping_)
      ++recorded_->drop_count;
    const bool recovered = dropping_ && !recorded_->drop_packets;
    dropping_ = recorded_->drop_packets;
    open_ = true;
    if (recovered) {
      buffer_->set_previous_packet_dropped(
          protos::pbzero::TracePacket::DATA_LOSS_PRESENT |
          protos::pbzero::TracePacket::DATA_LOSS_SMB_FULL);
    }
    return TracePacketHandle(buffer_.get());
  }

  void FinishTracePacket() override {
    if (!open_)
      return;
    open_ = false;
    if (dropping_)
      return;
    const std::vector<uint8_t> bytes = buffer_.SerializeAsArray();
    protos::gen::TracePacket packet;
    EXPECT_TRUE(packet.ParseFromArray(bytes.data(), bytes.size()));
    recorded_->packets.push_back(std::move(packet));
  }

  void Flush(std::function<void()> callback = {}) override {
    if (recorded_->on_flush)
      recorded_->on_flush();
    FinishTracePacket();
    ++recorded_->flushes;
    if (callback && recorded_->defer_flush_callbacks) {
      recorded_->pending_flush_callbacks.push_back(std::move(callback));
    } else if (callback) {
      callback();
    }
  }

  WriterID writer_id() const override { return writer_id_; }
  uint64_t written() const override { return 0; }
  uint64_t drop_count() const override { return recorded_->drop_count; }

 private:
  const WriterID writer_id_;
  Recorded* const recorded_;
  protozero::HeapBuffered<protos::pbzero::TracePacket> buffer_;
  bool open_ = false;
  bool dropping_ = false;
};

// Creates a v2 writer over a fresh FakeV1Writer with |writer_id|.
std::unique_ptr<TraceWriter> CreateV2Writer(InProcessTracingV2Bridge* bridge,
                                            WriterID writer_id,
                                            FakeV1Writer::Recorded* recorded) {
  auto v1_writer =
      std::unique_ptr<TraceWriter>(new FakeV1Writer(writer_id, recorded));
  TraceWriter* const v1_writer_ptr = v1_writer.get();
  std::unique_ptr<TraceWriter> writer =
      InProcessTracingV2BridgeTestPeer::CreateTraceWriter(
          bridge, std::move(v1_writer), /*target_buffer=*/11,
          BufferExhaustedPolicy::kDrop);
  EXPECT_NE(writer.get(), v1_writer_ptr);
  return writer;
}

// A task runner that only queues. Tests decide when tasks run, or drop them,
// and can count the posts. Always claims to be another thread.
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
  size_t queued() const { return queued_.size(); }
  void DiscardQueuedTasks() { queued_.clear(); }

  std::function<void()> TakeNextTask() {
    PERFETTO_CHECK(!queued_.empty());
    auto task = std::move(queued_.front());
    queued_.erase(queued_.begin());
    return task;
  }

  void RunNextTask() {
    auto task = TakeNextTask();
    task();
  }

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

// RelaySequence owns its task runner, but most tests here want to keep driving
// one they hold on the stack. This forwards to it and owns nothing.
class BorrowedTaskRunner : public base::TaskRunner {
 public:
  explicit BorrowedTaskRunner(base::TaskRunner* task_runner)
      : task_runner_(task_runner) {}
  void PostTask(std::function<void()> task) override {
    task_runner_->PostTask(std::move(task));
  }
  void PostDelayedTask(std::function<void()> task, uint32_t delay_ms) override {
    task_runner_->PostDelayedTask(std::move(task), delay_ms);
  }
  void AddFileDescriptorWatch(base::PlatformHandle handle,
                              std::function<void()> callback) override {
    task_runner_->AddFileDescriptorWatch(handle, std::move(callback));
  }
  void RemoveFileDescriptorWatch(base::PlatformHandle handle) override {
    task_runner_->RemoveFileDescriptorWatch(handle);
  }
  bool RunsTasksOnCurrentThread() const override {
    return task_runner_->RunsTasksOnCurrentThread();
  }

 private:
  base::TaskRunner* const task_runner_;
};

// A RelaySequence on top of a runner the test owns. The runner must outlive
// the bridges.
std::shared_ptr<RelaySequence> MakeRelay(base::TaskRunner* task_runner) {
  return std::make_shared<RelaySequence>(
      std::unique_ptr<base::TaskRunner>(new BorrowedTaskRunner(task_runner)));
}

class InProcessTracingV2BridgeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    relay_ = MakeRelay(&task_runner_);
    bridge_ = InProcessTracingV2Bridge::Create(relay_, kNumChunks, kChunkSize);
    ASSERT_NE(bridge_, nullptr);
  }

  void TearDown() override {
    bridge_.reset();
    // Relay tasks hold their own reference, so the bridge outlives ours until
    // they have run. Run them before the task runner goes away.
    task_runner_.RunUntilIdle();
  }

  // Waits until preceding ring data has been flushed to v1.
  void DrainRelay() {
    const std::string name = "relay-quiescent-" + std::to_string(++drains_);
    std::function<void()> quiescent = task_runner_.CreateCheckpoint(name);
    bridge_->DrainPendingData(std::move(quiescent));
    task_runner_.RunUntilCheckpoint(name);
  }

  std::unique_ptr<TraceWriter> CreateWriter(
      WriterID writer_id,
      BufferID target_buffer,
      BufferExhaustedPolicy policy = BufferExhaustedPolicy::kDrop) {
    auto v1_writer = std::unique_ptr<TraceWriter>(
        new FakeV1Writer(writer_id, &recorded_[writer_id]));
    TraceWriter* const v1_writer_ptr = v1_writer.get();
    std::unique_ptr<TraceWriter> writer =
        InProcessTracingV2BridgeTestPeer::CreateTraceWriter(
            bridge_.get(), std::move(v1_writer), target_buffer, policy);
    EXPECT_NE(writer.get(), v1_writer_ptr);
    return writer;
  }

  enum class RejectedChunk { kMalformed, kUnsupportedFormat, kWrongBuffer };

  void TestRejectedMiddleChunk(RejectedChunk rejection) {
    auto writer = CreateWriter(7, 11);
    TraceWriterV2::Delegate& delegate = *bridge_;
    SharedRingBuffer& ring = delegate.ring_buffer();
    SharedRingBufferWriter chunk_writer(&ring, 7, 11,
                                        BufferExhaustedPolicy::kDrop,
                                        test::GetNoopWriterDelegate());

    // Each fragment is a whole proto field on purpose: if the bridge wrongly
    // glued prefix and tail together, the result would still parse.
    ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x40\x01", false, true));
    chunk_writer.FinishCurrentChunk();
    DrainRelay();
    ASSERT_TRUE(recorded_[7].packets.empty());

    ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x50\x02", true, true));
    chunk_writer.FinishCurrentChunk();
    // No writer or relay task races these edits to the completed chunk.
    switch (rejection) {
      case RejectedChunk::kMalformed:
        // A fragment size larger than the chunk's payload capacity.
        WriteFragmentSize(ring.chunk_at(1) + kChunkSize, kChunkSize);
        break;
      case RejectedChunk::kUnsupportedFormat:
        test::SharedRingBufferInternalsForTest::SetChunkStateWord(
            &ring, 1, ring.LoadChunkStateWord(1) | (1u << kChunkFormatShift));
        break;
      case RejectedChunk::kWrongBuffer:
        StoreTargetBufferID(ring.chunk_at(1), 12);
        break;
    }
    DrainRelay();
    ASSERT_TRUE(recorded_[7].packets.empty());

    ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x58\x03", true, false));
    chunk_writer.FinishCurrentChunk();
    DrainRelay();
    EXPECT_TRUE(recorded_[7].packets.empty());

    // Another orphan, spanning two chunks, must not revive the prefix either.
    ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x40\x04", true, true));
    chunk_writer.FinishCurrentChunk();
    DrainRelay();
    EXPECT_TRUE(recorded_[7].packets.empty());
    ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x58\x05", true, false));
    // Recover at the next fragment boundary within the same chunk.
    ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x40\x06"));
    ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x40\x07"));
    chunk_writer.FinishCurrentChunk();
    DrainRelay();

    ASSERT_EQ(recorded_[7].packets.size(), 2u);
    EXPECT_EQ(recorded_[7].packets[0].timestamp(), 6u);
    constexpr uint32_t kExpectedLoss =
        protos::gen::TracePacket::DATA_LOSS_PRESENT |
        protos::gen::TracePacket::DATA_LOSS_CHUNK_CORRUPTED |
        protos::gen::TracePacket::DATA_LOSS_ORPHAN_CONTINUATION;
    EXPECT_EQ(recorded_[7].packets[0].previous_packet_dropped(), kExpectedLoss);
    EXPECT_EQ(recorded_[7].packets[1].timestamp(), 7u);
    EXPECT_EQ(recorded_[7].packets[1].previous_packet_dropped(), 0u);
  }

  static constexpr uint32_t kNumChunks = 32;
  static constexpr uint32_t kChunkSize = 256;

  base::TestTaskRunner task_runner_;
  std::shared_ptr<RelaySequence> relay_;
  std::shared_ptr<InProcessTracingV2Bridge> bridge_;
  std::map<WriterID, FakeV1Writer::Recorded> recorded_;
  uint32_t drains_ = 0;
};

TEST_F(InProcessTracingV2BridgeTest, FlushCallbackWaitsForTheV1Writer) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;

  writer->NewTracePacket()->set_timestamp(1234);
  bool flush_complete = false;
  writer->Flush([&flush_complete] { flush_complete = true; });

  EXPECT_FALSE(flush_complete);
  EXPECT_TRUE(recorded_[7].packets.empty());

  task_runner_.RunUntilIdle();
  ASSERT_EQ(recorded_[7].packets.size(), 1u);
  EXPECT_EQ(recorded_[7].packets[0].timestamp(), 1234u);
  EXPECT_FALSE(flush_complete);
  EXPECT_EQ(recorded_[7].flushes, 1u);
  ASSERT_EQ(recorded_[7].pending_flush_callbacks.size(), 1u);

  // The caller's callback fires with the v1 ack, without another relay hop.
  recorded_[7].AcknowledgeFlushes();
  EXPECT_TRUE(flush_complete);
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flush_complete);
}

TEST_F(InProcessTracingV2BridgeTest, EmptyFlushStillWaitsForTheV1Writer) {
  auto writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;

  bool flush_complete = false;
  auto done = task_runner_.CreateCheckpoint("flushed");
  writer->Flush([&] {
    flush_complete = true;
    done();
  });
  task_runner_.RunUntilIdle();
  EXPECT_EQ(recorded_[7].flushes, 1u);
  EXPECT_FALSE(flush_complete);
  ASSERT_EQ(recorded_[7].pending_flush_callbacks.size(), 1u);

  recorded_[7].AcknowledgeFlushes();
  EXPECT_TRUE(flush_complete);
  task_runner_.RunUntilCheckpoint("flushed");
  EXPECT_TRUE(flush_complete);
  EXPECT_EQ(recorded_[7].flushes, 1u);
}

TEST_F(InProcessTracingV2BridgeTest,
       CleanWriterFlushStillRequestsAcknowledgement) {
  auto writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;
  writer->NewTracePacket()->set_timestamp(1);
  DrainRelay();
  EXPECT_EQ(recorded_[7].flushes, 1u);
  EXPECT_TRUE(recorded_[7].pending_flush_callbacks.empty());

  bool flushed = false;
  writer->Flush([&] { flushed = true; });
  task_runner_.RunUntilIdle();
  EXPECT_FALSE(flushed);
  EXPECT_EQ(recorded_[7].flushes, 2u);
  ASSERT_EQ(recorded_[7].pending_flush_callbacks.size(), 1u);
  recorded_[7].AcknowledgeFlushes();
  EXPECT_TRUE(flushed);
}

TEST_F(InProcessTracingV2BridgeTest, ExplicitCallbacksBelongToTheirOwnFlush) {
  auto writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;
  bool first = false;
  bool second = false;
  writer->NewTracePacket()->set_timestamp(1);
  writer->Flush([&] { first = true; });
  writer->NewTracePacket()->set_timestamp(2);
  writer->Flush([&] { second = true; });
  task_runner_.RunUntilIdle();
  EXPECT_FALSE(first);
  EXPECT_FALSE(second);
  ASSERT_EQ(recorded_[7].packets.size(), 2u);
  EXPECT_EQ(recorded_[7].flushes, 2u);
  auto callbacks = std::move(recorded_[7].pending_flush_callbacks);
  ASSERT_EQ(callbacks.size(), 2u);
  callbacks[1]();
  EXPECT_FALSE(first);
  EXPECT_TRUE(second);
  callbacks[0]();
  EXPECT_TRUE(first);
}

TEST_F(InProcessTracingV2BridgeTest, CleanRetirementDoesNotFlushV1Writer) {
  auto writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;
  writer.reset();
  EXPECT_EQ(recorded_[7].destructions, 0u);

  DrainRelay();
  EXPECT_EQ(recorded_[7].destructions, 1u);
  EXPECT_EQ(recorded_[7].flushes, 0u);
  EXPECT_TRUE(recorded_[7].pending_flush_callbacks.empty());

  // Retirement has released this ID, so it can be registered again.
  writer = CreateWriter(7, 11);
}

TEST_F(InProcessTracingV2BridgeTest, DirtyRetirementFlushesV1WriterOnce) {
  auto writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;
  recorded_[7].on_forward = task_runner_.CreateCheckpoint("forwarded");
  writer->NewTracePacket()->set_timestamp(1);
  task_runner_.RunUntilCheckpoint("forwarded");
  ASSERT_EQ(recorded_[7].flushes, 0u);
  recorded_[7].on_flush = [&] { EXPECT_EQ(recorded_[7].destructions, 0u); };

  writer.reset();
  DrainRelay();
  ASSERT_EQ(recorded_[7].packets.size(), 1u);
  EXPECT_EQ(recorded_[7].packets[0].timestamp(), 1u);
  EXPECT_EQ(recorded_[7].flushes, 1u);
  EXPECT_EQ(recorded_[7].destructions, 1u);
  EXPECT_TRUE(recorded_[7].pending_flush_callbacks.empty());
}

TEST_F(InProcessTracingV2BridgeTest, RetirementAfterFlushDoesNotFlushAgain) {
  auto writer = CreateWriter(7, 11);
  writer->NewTracePacket()->set_timestamp(1);
  auto done = task_runner_.CreateCheckpoint("flushed");
  writer->Flush(std::move(done));
  task_runner_.RunUntilCheckpoint("flushed");
  ASSERT_EQ(recorded_[7].flushes, 1u);

  recorded_[7].defer_flush_callbacks = true;
  writer.reset();
  DrainRelay();
  EXPECT_EQ(recorded_[7].destructions, 1u);
  EXPECT_EQ(recorded_[7].flushes, 1u);
  EXPECT_TRUE(recorded_[7].pending_flush_callbacks.empty());
}

TEST_F(InProcessTracingV2BridgeTest, ProtocolErrorDoesNotStrandBarriers) {
  // Check errors both before and after progress in the same reader batch.
  for (uint32_t malformed_pos : {0u, 1u}) {
    SCOPED_TRACE(malformed_pos);
    bridge_ = InProcessTracingV2Bridge::Create(relay_, kNumChunks, kChunkSize);
    TraceWriterV2::Delegate& delegate = *bridge_;
    SharedRingBuffer& ring = delegate.ring_buffer();
    for (uint32_t i = 0; i <= malformed_pos; ++i)
      ASSERT_EQ(ring.TryReserveWritePos().result,
                SharedRingBuffer::ReserveResult::kReserved);
    // No writer or relay task races this invalid Free wrap count.
    test::SharedRingBufferInternalsForTest::SetChunkStateWord(
        &ring, malformed_pos, MakeFreeStateWord(1));

#if PERFETTO_DCHECK_IS_ON()
    EXPECT_DEATH_IF_SUPPORTED(
        DrainRelay(), "Cannot drain malformed tracing v2 ring position");
#else
    // The reader stays stopped, but this request and a later one complete.
    DrainRelay();
    DrainRelay();
    EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring),
              malformed_pos);
#endif
  }
}

TEST_F(InProcessTracingV2BridgeTest,
       DrainPendingDataFlushesEveryDirtyWriterBeforeCompletion) {
  auto first = CreateWriter(7, 11);
  auto second = CreateWriter(9, 22);
  auto clean = CreateWriter(11, 33);
  const WriterID writer_ids[] = {7, 9, 11};
  for (WriterID id : writer_ids)
    recorded_[id].defer_flush_callbacks = true;
  first->NewTracePacket()->set_timestamp(1);
  first->FinishTracePacket();
  second->NewTracePacket()->set_timestamp(2);
  second->FinishTracePacket();

  bool done = false;
  bridge_->DrainPendingData([&] {
    EXPECT_EQ(recorded_[7].flushes, 1u);
    EXPECT_EQ(recorded_[9].flushes, 1u);
    ASSERT_EQ(recorded_[7].packets.size(), 1u);
    ASSERT_EQ(recorded_[9].packets.size(), 1u);
    EXPECT_EQ(recorded_[7].packets[0].timestamp(), 1u);
    EXPECT_EQ(recorded_[9].packets[0].timestamp(), 2u);
    done = true;
  });
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(done);
  EXPECT_EQ(recorded_[11].flushes, 0u);
  for (WriterID id : writer_ids)
    EXPECT_TRUE(recorded_[id].pending_flush_callbacks.empty());
}

TEST_F(InProcessTracingV2BridgeTest, WriterWithNoIdStaysOnV1) {
  // WriterID 0 cannot identify a downstream writer. There is nothing to put in
  // the chunk header or use to route the packet back.
  auto v1_writer =
      std::unique_ptr<TraceWriter>(new FakeV1Writer(0, &recorded_[0]));
  TraceWriter* const raw = v1_writer.get();
  std::unique_ptr<TraceWriter> writer =
      InProcessTracingV2BridgeTestPeer::CreateTraceWriter(
          bridge_.get(), std::move(v1_writer), 11,
          BufferExhaustedPolicy::kDrop);
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
}

TEST_F(InProcessTracingV2BridgeTest, MalformedChunkDiscardsPartialPacket) {
  TestRejectedMiddleChunk(RejectedChunk::kMalformed);
}

TEST_F(InProcessTracingV2BridgeTest, UnsupportedChunkDiscardsPartialPacket) {
  TestRejectedMiddleChunk(RejectedChunk::kUnsupportedFormat);
}

TEST_F(InProcessTracingV2BridgeTest, WrongTargetBufferDiscardsPartialPacket) {
#if PERFETTO_DCHECK_IS_ON()
  EXPECT_DEATH_IF_SUPPORTED(
      TestRejectedMiddleChunk(RejectedChunk::kWrongBuffer),
      "Tracing v2 writer 7 changed target buffer");
#else
  TestRejectedMiddleChunk(RejectedChunk::kWrongBuffer);
#endif
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

// Wake-up coalescing: a commit that finds a drain task already queued must not
// post another one. Counted on a runner that does nothing but count.
TEST(InProcessTracingV2BridgeBurstTest, CommitsWhileATaskIsQueuedPostNothing) {
  QueuedTaskRunner task_runner;
  std::map<WriterID, FakeV1Writer::Recorded> recorded;
  auto bridge =
      InProcessTracingV2Bridge::Create(MakeRelay(&task_runner),
                                       /*num_chunks=*/32, /*chunk_size=*/256);
  std::unique_ptr<TraceWriter> writer =
      CreateV2Writer(bridge.get(), 7, &recorded[7]);

  const size_t posts_before = task_runner.posts();
  for (uint32_t i = 0; i < 200; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  writer->FinishTracePacket();

  // Two hundred packets, one task.
  EXPECT_EQ(task_runner.posts() - posts_before, 1u);

  task_runner.RunQueuedTasks();

  // The drain leaves the v1 chunk open. Flush only after the post count check
  // above, since this posts too.
  bridge->DrainPendingData([] {});
  task_runner.RunQueuedTasks();
  ASSERT_EQ(recorded[7].packets.size(), 200u);
  for (uint32_t i = 0; i < 200; ++i)
    EXPECT_EQ(recorded[7].packets[i].timestamp(), i) << i;

  writer.reset();
  bridge.reset();
  task_runner.RunQueuedTasks();
}

// A commit made while the drain task is running must schedule another pass.
TEST(InProcessTracingV2BridgeBurstTest, CommitDuringDrainSchedulesAnotherPass) {
  QueuedTaskRunner task_runner;
  std::map<WriterID, FakeV1Writer::Recorded> recorded;
  auto bridge =
      InProcessTracingV2Bridge::Create(MakeRelay(&task_runner),
                                       /*num_chunks=*/32, /*chunk_size=*/256);
  std::unique_ptr<TraceWriter> writer =
      CreateV2Writer(bridge.get(), 7, &recorded[7]);

  bool published_mid_pass = false;
  recorded[7].on_forward = [&] {
    if (published_mid_pass)
      return;
    published_mid_pass = true;
    const size_t posts_before = task_runner.posts();
    writer->NewTracePacket()->set_timestamp(2);
    writer->FinishTracePacket();
    EXPECT_EQ(task_runner.posts(), posts_before + 1);
  };

  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();
  // Run the drain pass and whatever it queues, including the second pass.
  task_runner.RunQueuedTasks();

  ASSERT_TRUE(published_mid_pass);
  // Only now flush and read back: this posts too and would mask a missing
  // second pass.
  bridge->DrainPendingData([] {});
  task_runner.RunQueuedTasks();

  ASSERT_EQ(recorded[7].packets.size(), 2u);
  EXPECT_EQ(recorded[7].packets[0].timestamp(), 1u);
  EXPECT_EQ(recorded[7].packets[1].timestamp(), 2u);

  writer.reset();
  bridge.reset();
  task_runner.RunQueuedTasks();
}

TEST(InProcessTracingV2BridgeBurstTest,
     ControlDrainYieldsToAnotherBridgeAndKeepsItsWatermark) {
  QueuedTaskRunner task_runner;
  FakeV1Writer::Recorded recorded_a;
  FakeV1Writer::Recorded recorded_b;
  auto relay = MakeRelay(&task_runner);
  auto bridge_a = InProcessTracingV2Bridge::Create(relay, 1024, 256);
  auto bridge_b = InProcessTracingV2Bridge::Create(relay, 32, 256);
  auto writer_a = CreateV2Writer(bridge_a.get(), 7, &recorded_a);
  auto writer_b = CreateV2Writer(bridge_b.get(), 7, &recorded_b);
  TraceWriterV2::Delegate& delegate_a = *bridge_a;
  SharedRingBuffer& ring_a = delegate_a.ring_buffer();
  // Publish one packet per position without ordinary drain notifications, so
  // only the control barrier can advance A's reader.
  SharedRingBufferWriter chunk_writer(&ring_a, 7, 11,
                                      BufferExhaustedPolicy::kDrop,
                                      test::GetNoopWriterDelegate());
  constexpr uint32_t kBatchSize = 256;
  constexpr uint32_t kFirstTarget = 3 * kBatchSize + 1;
  for (uint32_t i = 0; i < kFirstTarget; ++i) {
    ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x40\x01"));
    chunk_writer.FinishCurrentChunk();
  }
  ASSERT_EQ(ring_a.LoadWritePos(), kFirstTarget);
  std::vector<uint32_t> completions;
  bridge_a->DrainPendingData([&] {
    EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring_a),
              kFirstTarget);
    EXPECT_EQ(recorded_a.packets.size(), kFirstTarget);
    EXPECT_EQ(recorded_a.flushes, 1u);
    completions.push_back(1);
  });
  writer_b->NewTracePacket()->set_timestamp(2);
  writer_b->FinishTracePacket();
  uint32_t forwarded_a = 0;
  uint32_t forwarded_b = 0;
  recorded_a.on_forward = [&] { ++forwarded_a; };
  recorded_b.on_forward = [&] {
    ++forwarded_b;
    EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring_a),
              kBatchSize);
    EXPECT_EQ(forwarded_a, kBatchSize);
    EXPECT_EQ(recorded_a.flushes, 0u);
    EXPECT_TRUE(completions.empty());
  };
  ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x40\x02"));
  chunk_writer.FinishCurrentChunk();
  bridge_a->DrainPendingData([&] {
    EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring_a),
              kFirstTarget + 1);
    EXPECT_EQ(recorded_a.packets.size(), kFirstTarget + 1);
    EXPECT_EQ(recorded_a.flushes, 2u);
    completions.push_back(2);
  });
  ASSERT_EQ(task_runner.queued(), 3u);

  task_runner.RunNextTask();  // A's first batch.
  EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring_a),
            kBatchSize);
  EXPECT_TRUE(completions.empty());
  ASSERT_EQ(task_runner.queued(), 3u);  // B, second barrier, continuation.
  // A keeps writing after both watermarks were sampled. Neither continuation
  // may extend its target to include this position.
  ASSERT_TRUE(test::WriteFragment(&chunk_writer, "\x40\x03"));
  chunk_writer.FinishCurrentChunk();
  task_runner.RunNextTask();  // B runs before A's continuation.
  EXPECT_EQ(forwarded_b, 1u);
  task_runner.RunNextTask();  // Queue A's second barrier behind the first.
  ASSERT_EQ(task_runner.queued(), 1u);
  EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring_a),
            kBatchSize);

  for (uint32_t read_pos :
       {2 * kBatchSize, 3 * kBatchSize, kFirstTarget, kFirstTarget + 1}) {
    ASSERT_EQ(task_runner.queued(), 1u);
    task_runner.RunNextTask();
    EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring_a),
              read_pos);
  }
  EXPECT_EQ(completions, (std::vector<uint32_t>{1, 2}));
  EXPECT_EQ(recorded_a.packets.size(), kFirstTarget + 1);
  EXPECT_EQ(ring_a.LoadWritePos(), kFirstTarget + 2);
  EXPECT_EQ(task_runner.queued(), 0u);
  // Ordinary notification still delivers the later packet.
  delegate_a.NotifyReader();
  ASSERT_EQ(task_runner.queued(), 1u);
  task_runner.RunNextTask();
  EXPECT_EQ(forwarded_a, kFirstTarget + 2);
  bridge_a->DrainPendingData([] {});
  bridge_b->DrainPendingData([] {});
  task_runner.RunQueuedTasks();
  ASSERT_EQ(recorded_a.packets.size(), kFirstTarget + 2);
  EXPECT_EQ(recorded_a.packets.back().timestamp(), 3u);
  ASSERT_EQ(recorded_b.packets.size(), 1u);
  EXPECT_EQ(recorded_b.packets[0].timestamp(), 2u);
  EXPECT_EQ(completions, (std::vector<uint32_t>{1, 2}));

  writer_a.reset();
  writer_b.reset();
  task_runner.RunQueuedTasks();
}

TEST(InProcessTracingV2BridgeBurstTest,
     StaleControlContinuationDoesNotDrainTheNextBarrier) {
  QueuedTaskRunner task_runner;
  auto bridge =
      InProcessTracingV2Bridge::Create(MakeRelay(&task_runner), 1024, 256);
  TraceWriterV2::Delegate& delegate = *bridge;
  SharedRingBuffer& ring = delegate.ring_buffer();
  // Unclaimed reservations are resolved as holes and need no retained writer.
  for (uint32_t i = 0; i < 257; ++i)
    ASSERT_EQ(ring.TryReserveWritePos().result,
              SharedRingBuffer::ReserveResult::kReserved);
  std::vector<uint32_t> completions;
  bridge->DrainPendingData([&] { completions.push_back(1); });
  for (uint32_t i = 0; i < 256; ++i)
    ASSERT_EQ(ring.TryReserveWritePos().result,
              SharedRingBuffer::ReserveResult::kReserved);
  bridge->DrainPendingData([&] { completions.push_back(2); });

  task_runner.RunNextTask();
  EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring), 256u);
  task_runner.RunNextTask();  // Only enqueues the second barrier.
  ASSERT_EQ(task_runner.queued(), 1u);
  auto continuation = task_runner.TakeNextTask();
  continuation();
  // Completing the first barrier must yield before the second reader batch.
  EXPECT_EQ(completions, (std::vector<uint32_t>{1}));
  EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring), 257u);
  ASSERT_EQ(task_runner.queued(), 1u);

  const size_t posts = task_runner.posts();
  continuation();  // Replay the old barrier's task while the next is pending.
  EXPECT_EQ(task_runner.posts(), posts);
  EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring), 257u);
  EXPECT_EQ(completions, (std::vector<uint32_t>{1}));
  task_runner.RunQueuedTasks();
  EXPECT_EQ(test::SharedRingBufferInternalsForTest::GetReadPos(&ring), 513u);
  EXPECT_EQ(completions, (std::vector<uint32_t>{1, 2}));
  continuation();  // An empty queue also makes it stale.
  EXPECT_EQ(task_runner.queued(), 0u);
  EXPECT_EQ(completions, (std::vector<uint32_t>{1, 2}));
}

// A barrier only covers what was in the ring when it sampled the write
// position. Packets written after that (which does happen: flushing a v1
// writer from the drain runs arbitrary callbacks) belong to the next drain,
// not to this completion.
TEST_F(InProcessTracingV2BridgeTest, DrainPendingDataStopsAtItsWatermark) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  for (uint32_t i = 0; i < 50; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  writer->Flush();

  // This task runs after the barrier has sampled its position. It arms
  // on_forward below, which writes one more packet from inside the drain.
  // Neither packet may be counted by the completion. No threads or timing
  // involved.
  bool armed = false;
  task_runner_.PostTask([&] {
    armed = true;
    writer->NewTracePacket()->set_timestamp(500);
    writer->Flush();
  });

  size_t packets_at_completion = 0;
  bool done = false;
  bridge_->DrainPendingData([&] {
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
  // Exactly the 50 packets that were in the ring at sampling time. The other
  // two are not lost, the next drain picks them up.
  EXPECT_EQ(packets_at_completion, 50u);
  ASSERT_EQ(recorded_[7].packets.size(), 52u);
  EXPECT_EQ(recorded_[7].packets[50].timestamp(), 500u);
  EXPECT_EQ(recorded_[7].packets[51].timestamp(), 999u);
}

// ---------------------------------------------------------------------------
// Barrier ordering: one queue, request order, independent of explicit writer
// acknowledgements.
// ---------------------------------------------------------------------------

TEST_F(InProcessTracingV2BridgeTest, OverlappingBarriersCompleteInOrder) {
  auto writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;
  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();

  std::vector<std::string> operations;
  recorded_[7].on_flush = [&] { operations.push_back("flush"); };
  bridge_->DrainPendingData([&] { operations.push_back("A"); });
  // Both requests are pending before the relay runs either one. B is clean.
  bridge_->DrainPendingData([&] { operations.push_back("B"); });
  task_runner_.RunUntilIdle();
  EXPECT_EQ(operations, (std::vector<std::string>{"flush", "A", "B"}));
  EXPECT_TRUE(recorded_[7].pending_flush_callbacks.empty());
}

TEST_F(InProcessTracingV2BridgeTest,
       WriterAcknowledgementDoesNotBlockTransportDrain) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;
  writer->NewTracePacket()->set_timestamp(1);

  std::vector<std::string> completions;
  writer->Flush([&] { completions.push_back("writer flush"); });
  task_runner_.RunUntilIdle();
  ASSERT_EQ(recorded_[7].pending_flush_callbacks.size(), 1u);

  bridge_->DrainPendingData([&] { completions.push_back("endpoint drain"); });
  task_runner_.RunUntilIdle();
  EXPECT_EQ(completions, (std::vector<std::string>{"endpoint drain"}));

  recorded_[7].AcknowledgeFlushes();
  EXPECT_EQ(completions,
            (std::vector<std::string>{"endpoint drain", "writer flush"}));
}

TEST_F(InProcessTracingV2BridgeTest, RetirementWaitsBehindAnEarlierBarrier) {
  auto writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;
  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();

  bool drained = false;
  bridge_->DrainPendingData([&] {
    EXPECT_EQ(recorded_[7].destructions, 0u);
    EXPECT_EQ(recorded_[7].flushes, 1u);
    drained = true;
  });
  writer.reset();
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(drained);
  EXPECT_EQ(recorded_[7].destructions, 1u);
  EXPECT_EQ(recorded_[7].flushes, 1u);
  EXPECT_TRUE(recorded_[7].pending_flush_callbacks.empty());
}

// If v1 drops the packet carrying a bridge-side loss reason, that reason is
// gone: the trace only shows v1's own SMB_FULL loss, once, on the first packet
// that gets through.
TEST_F(InProcessTracingV2BridgeTest,
       DownstreamDropCanLoseDetailedBridgeLossReason) {
  auto writer = CreateWriter(7, 11);
  // An unterminated proto group makes the bridge reject this packet.
  const uint8_t malformed[] = {0x0b};
  writer->NewTracePacket()->AppendRawProtoBytes(malformed, sizeof(malformed));
  writer->FinishTracePacket();
  DrainRelay();
  ASSERT_TRUE(recorded_[7].packets.empty());

  recorded_[7].drop_packets = true;
  for (uint64_t timestamp : {1u, 2u}) {
    writer->NewTracePacket()->set_timestamp(timestamp);
    writer->FinishTracePacket();
    DrainRelay();
    EXPECT_EQ(recorded_[7].drop_count, 1u);
    EXPECT_TRUE(recorded_[7].packets.empty());
  }
  recorded_[7].drop_packets = false;
  writer->NewTracePacket()->set_timestamp(3);
  writer->NewTracePacket()->set_timestamp(4);
  writer->FinishTracePacket();
  DrainRelay();
  ASSERT_EQ(recorded_[7].packets.size(), 2u);
  constexpr uint32_t kExpectedLoss =
      protos::gen::TracePacket::DATA_LOSS_PRESENT |
      protos::gen::TracePacket::DATA_LOSS_SMB_FULL;
  EXPECT_EQ(recorded_[7].packets[0].previous_packet_dropped(), kExpectedLoss);
  EXPECT_EQ(recorded_[7].packets[1].previous_packet_dropped(), 0u);
}

TEST_F(InProcessTracingV2BridgeTest, DataLossIsReportedOnTheNextPacket) {
  // A ring this small fills immediately, so the writer has to drop.
  bridge_ = InProcessTracingV2Bridge::Create(relay_, /*num_chunks=*/2,
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

  // Draining frees space. The next packet gets in and carries the loss report,
  // as previous_packet_dropped is defined.
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

TEST_F(InProcessTracingV2BridgeTest,
       WriterIsReleasedAfterItsLastPacketIsDrained) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  recorded_[7].defer_flush_callbacks = true;
  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();

  recorded_[7].on_forward = [&] { EXPECT_EQ(recorded_[7].destructions, 0u); };
  recorded_[7].on_flush = [&] { EXPECT_EQ(recorded_[7].destructions, 0u); };
  writer.reset();

  EXPECT_EQ(recorded_[7].destructions, 0u);
  task_runner_.RunUntilIdle();

  ASSERT_EQ(recorded_[7].packets.size(), 1u);
  EXPECT_EQ(recorded_[7].packets[0].timestamp(), 1u);
  EXPECT_TRUE(recorded_[7].pending_flush_callbacks.empty());
  EXPECT_EQ(recorded_[7].destructions, 1u);
  EXPECT_EQ(recorded_[7].flushes, 1u);
}

// Dropping our last reference does not lose what is in the ring: the
// retirement barrier posted by the writer keeps the bridge alive until it has
// forwarded everything.
TEST_F(InProcessTracingV2BridgeTest, TeardownDrainsWhatIsStillInTheRing) {
  std::unique_ptr<TraceWriter> writer = CreateWriter(7, 11);
  for (uint32_t i = 0; i < 10; ++i)
    writer->NewTracePacket()->set_timestamp(i);
  writer->FinishTracePacket();
  // Nothing has run yet: the packets are in the ring and nowhere else.
  ASSERT_TRUE(recorded_[7].packets.empty());

  // Drop the writer, then our reference. The queued retirement barrier is now
  // the only owner, and it drains before it goes away.
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

// Queued relay tasks own the bridge through shared_ptr. Discarding the queue
// releases those references; after the explicit owners are gone, the bridge
// and its retained v1 writers are destroyed. A raw pointer here would be a
// lifetime bug.
TEST(InProcessTracingV2BridgeLifetimeTest,
     DiscardingTheRelayQueueDestroysTheBridgeAndItsWriters) {
  QueuedTaskRunner task_runner;
  std::map<WriterID, FakeV1Writer::Recorded> recorded;

  auto bridge =
      InProcessTracingV2Bridge::Create(MakeRelay(&task_runner),
                                       /*num_chunks=*/32, /*chunk_size=*/256);
  std::unique_ptr<TraceWriter> writer =
      CreateV2Writer(bridge.get(), 7, &recorded[7]);
  writer->NewTracePacket()->set_timestamp(1);
  writer->FinishTracePacket();

  writer.reset();
  bridge.reset();
  // Nothing has run, so the queue is the last owner and the v1 writer is still
  // alive inside the bridge.
  ASSERT_GT(task_runner.queued(), 0u);
  ASSERT_EQ(recorded[7].destructions, 0u);

  task_runner.DiscardQueuedTasks();
  EXPECT_EQ(recorded[7].destructions, 1u);
}

// A TraceWriter::Flush() callback is handed to the v1 writer. Its ack must not
// go through the relay, which may be closed by then.
TEST(InProcessTracingV2BridgeLifetimeTest,
     ExplicitFlushAcknowledgementDoesNotNeedRelay) {
  auto queued_runner =
      std::unique_ptr<QueuedTaskRunner>(new QueuedTaskRunner());
  QueuedTaskRunner* const queued = queued_runner.get();
  auto relay = std::make_shared<RelaySequence>(std::move(queued_runner));
  std::map<WriterID, FakeV1Writer::Recorded> recorded;
  auto bridge = InProcessTracingV2Bridge::Create(relay, /*num_chunks=*/32,
                                                 /*chunk_size=*/256);
  std::unique_ptr<TraceWriter> writer =
      CreateV2Writer(bridge.get(), 7, &recorded[7]);
  recorded[7].defer_flush_callbacks = true;

  writer->NewTracePacket()->set_timestamp(1);
  bool flush_complete = false;
  writer->Flush([&flush_complete] { flush_complete = true; });
  queued->RunQueuedTasks();
  // The barrier forwarded the packet and asked the v1 writer to flush. That
  // flush is the one still outstanding when shutdown starts.
  ASSERT_EQ(recorded[7].packets.size(), 1u);
  ASSERT_EQ(recorded[7].pending_flush_callbacks.size(), 1u);
  ASSERT_FALSE(flush_complete);

  // Same as TracingMuxerImpl::Shutdown(): Close(), then destroy the runner.
  // Everything below happens in between.
  std::unique_ptr<base::TaskRunner> relay_task_runner = relay->Close();
  ASSERT_EQ(relay_task_runner.get(), queued);
  const size_t posts_after_close = queued->posts();

  recorded[7].AcknowledgeFlushes();
  EXPECT_EQ(queued->posts(), posts_after_close);
  EXPECT_TRUE(flush_complete);

  // Destroying a writer after the close must not post either.
  writer.reset();
  EXPECT_EQ(queued->posts(), posts_after_close);
  EXPECT_EQ(recorded[7].destructions, 0u);

  relay_task_runner.reset();
  // The v1 writer was never retired, so it goes when the bridge does.
  bridge.reset();
  EXPECT_EQ(recorded[7].destructions, 1u);
}

// Drain and explicit-flush barriers have callbacks, so requests made after
// close complete inline. Writer retirement has no waiter and is dropped.
TEST(InProcessTracingV2BridgeLifetimeTest,
     BarriersRequestedAfterCloseCompleteInline) {
  auto queued_runner =
      std::unique_ptr<QueuedTaskRunner>(new QueuedTaskRunner());
  QueuedTaskRunner* const queued = queued_runner.get();
  auto relay = std::make_shared<RelaySequence>(std::move(queued_runner));
  std::map<WriterID, FakeV1Writer::Recorded> recorded;
  auto bridge = InProcessTracingV2Bridge::Create(relay, /*num_chunks=*/32,
                                                 /*chunk_size=*/256);
  std::unique_ptr<TraceWriter> writer =
      CreateV2Writer(bridge.get(), 7, &recorded[7]);

  std::unique_ptr<base::TaskRunner> relay_task_runner = relay->Close();
  const size_t posts_after_close = queued->posts();

  bool drained = false;
  bridge->DrainPendingData([&drained] { drained = true; });
  EXPECT_TRUE(drained);

  bool flushed = false;
  writer->Flush([&flushed] { flushed = true; });
  EXPECT_TRUE(flushed);

  EXPECT_EQ(queued->posts(), posts_after_close);

  writer.reset();
  relay_task_runner.reset();
  bridge.reset();
}

// The flush ack callback ends up owned by the endpoint, which can outlive the
// arbiter. If that callback owned the bridge it would keep the v1 writers
// alive past their arbiter, and they would touch it on destruction.
//
// So the bridge must hand the caller's callback through as is, without
// capturing itself.
TEST(InProcessTracingV2BridgeLifetimeTest,
     ALateDownstreamAcknowledgementDoesNotOwnTheBridge) {
  auto queued_runner =
      std::unique_ptr<QueuedTaskRunner>(new QueuedTaskRunner());
  QueuedTaskRunner* const queued = queued_runner.get();
  auto relay = std::make_shared<RelaySequence>(std::move(queued_runner));
  std::map<WriterID, FakeV1Writer::Recorded> recorded;
  auto bridge = InProcessTracingV2Bridge::Create(relay, /*num_chunks=*/32,
                                                 /*chunk_size=*/256);
  std::unique_ptr<TraceWriter> writer =
      CreateV2Writer(bridge.get(), 7, &recorded[7]);
  recorded[7].defer_flush_callbacks = true;

  writer->NewTracePacket()->set_timestamp(1);
  bool completed = false;
  writer->Flush([&] { completed = true; });
  queued->RunQueuedTasks();
  // |recorded| now holds the acknowledgement, the way an endpoint would.
  ASSERT_EQ(recorded[7].pending_flush_callbacks.size(), 1u);

  std::unique_ptr<base::TaskRunner> relay_task_runner = relay->Close();
  const size_t posts_after_close = queued->posts();

  // Drop every legitimate owner. The pending ack must not keep the bridge, and
  // its v1 writer, alive.
  writer.reset();
  bridge.reset();
  EXPECT_EQ(recorded[7].destructions, 1u);

  // Delivering the ack now must not post to the closed relay or touch the
  // dead bridge.
  EXPECT_FALSE(completed);
  recorded[7].AcknowledgeFlushes();
  EXPECT_TRUE(completed);
  EXPECT_EQ(queued->posts(), posts_after_close);
}

// A 32 KiB packet in an 8 KiB ring: the writer stalls until a real relay
// thread frees space. The fake v1 writer never drops, so this checks the
// reassembly exactly.
TEST(InProcessTracingV2BridgeTestWithThread,
     PacketLargerThanRingReassemblesExactly) {
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "Requires writer waiting";
  FakeV1Writer::Recorded recorded;
  auto relay =
      std::make_shared<RelaySequence>(std::make_unique<base::ThreadTaskRunner>(
          base::ThreadTaskRunner::CreateAndStart("test.relay")));
  auto bridge = InProcessTracingV2Bridge::Create(relay, 32, 256);
  auto writer = InProcessTracingV2BridgeTestPeer::CreateTraceWriter(
      bridge.get(), std::make_unique<FakeV1Writer>(7, &recorded), 11,
      BufferExhaustedPolicy::kStall);
  const std::string payload(32768, 'z');
  writer->NewTracePacket()->set_for_testing()->set_str(payload);
  base::WaitableEvent flushed;
  writer->Flush([&] { flushed.Notify(); });
  flushed.Wait();
  writer.reset();
  bridge.reset();
  relay->Close().reset();
  ASSERT_EQ(recorded.packets.size(), 1u);
  EXPECT_EQ(recorded.packets[0].for_testing().str(), payload);
  EXPECT_EQ(recorded.packets[0].previous_packet_dropped(), 0u);
}

// Same shutdown sequence with a real relay thread instead of a queued runner.
TEST(InProcessTracingV2BridgeLifetimeTest, ClosingARealRelayThreadIsSafe) {
  auto relay = std::make_shared<RelaySequence>(
      std::unique_ptr<base::TaskRunner>(new base::ThreadTaskRunner(
          base::ThreadTaskRunner::CreateAndStart("TracingV2RelayTest"))));
  std::map<WriterID, FakeV1Writer::Recorded> recorded;
  auto bridge = InProcessTracingV2Bridge::Create(relay, /*num_chunks=*/32,
                                                 /*chunk_size=*/256);
  std::unique_ptr<TraceWriter> writer =
      CreateV2Writer(bridge.get(), 7, &recorded[7]);

  writer->NewTracePacket()->set_timestamp(1);
  base::WaitableEvent drained;
  bridge->DrainPendingData([&drained] { drained.Notify(); });
  drained.Wait();
  ASSERT_EQ(recorded[7].packets.size(), 1u);

  std::unique_ptr<base::TaskRunner> relay_task_runner = relay->Close();
  EXPECT_FALSE(relay->PostTask([] { ADD_FAILURE() << "ran after Close()"; }));

  // Retirement is refused after the close, so the v1 writer goes with the
  // bridge rather than being handed back.
  writer.reset();
  bridge.reset();
  // The task that ran the completion may hold the last bridge reference until
  // it returns.
  relay_task_runner.reset();
  EXPECT_EQ(recorded[7].destructions, 1u);
}

// After a reconnect the new arbiter hands out WriterIDs from 1 again, so the
// same id can be live on the old and the new connection at once. Each
// connection has its own bridge, with its own ring and writer map, so the two
// never mix.
//
// TracingV2SurvivesASystemServiceRestart in api_integrationtest.cc relies on
// this.
TEST(InProcessTracingV2BridgeLifetimeTest,
     TwoBridgesWithTheSameWriterIdStayIndependent) {
  base::TestTaskRunner task_runner;
  std::map<WriterID, FakeV1Writer::Recorded> old_recorded;
  std::map<WriterID, FakeV1Writer::Recorded> new_recorded;
  std::shared_ptr<InProcessTracingV2Bridge> old_bridge =
      InProcessTracingV2Bridge::Create(MakeRelay(&task_runner),
                                       /*num_chunks=*/32, /*chunk_size=*/256);
  std::shared_ptr<InProcessTracingV2Bridge> new_bridge =
      InProcessTracingV2Bridge::Create(MakeRelay(&task_runner),
                                       /*num_chunks=*/32, /*chunk_size=*/256);

  std::unique_ptr<TraceWriter> old_writer =
      CreateV2Writer(old_bridge.get(), 7, &old_recorded[7]);
  std::unique_ptr<TraceWriter> new_writer =
      CreateV2Writer(new_bridge.get(), 7, &new_recorded[7]);

  old_writer->NewTracePacket()->set_timestamp(1);
  old_writer->Flush();
  new_writer->NewTracePacket()->set_timestamp(2);
  new_writer->Flush();
  task_runner.RunUntilIdle();

  ASSERT_EQ(old_recorded[7].packets.size(), 1u);
  EXPECT_EQ(old_recorded[7].packets[0].timestamp(), 1u);
  ASSERT_EQ(new_recorded[7].packets.size(), 1u);
  EXPECT_EQ(new_recorded[7].packets[0].timestamp(), 2u);

  old_writer.reset();
  new_writer.reset();
  old_bridge.reset();
  new_bridge.reset();
  task_runner.RunUntilIdle();
}

// The ring is sized from the SMB, which is any multiple of 4 KiB and does not
// necessarily hold a power-of-two number of chunks. Round down rather than
// reject.
TEST(InProcessTracingV2BridgeSizingTest, CapacityRoundsDownToAPowerOfTwo) {
  constexpr uint32_t kChunkSize = InProcessTracingV2Bridge::kDefaultChunkSize;
  const auto chunks = [](size_t bytes) {
    return InProcessTracingV2Bridge::NumChunksForCapacity(bytes, kChunkSize);
  };

  // The default shared memory size maps exactly.
  EXPECT_EQ(chunks(256 * 1024), 1024u);
  // 132 KiB is a legal SMB size. 528 chunks is not a power of two.
  EXPECT_EQ(chunks(132 * 1024), 512u);
  EXPECT_EQ(chunks(kChunkSize), 1u);
  EXPECT_EQ(chunks(2 * kChunkSize - 1), 1u);
  // Too small for a single chunk: the caller has to reject the request.
  EXPECT_EQ(chunks(kChunkSize - 1), 0u);
  EXPECT_EQ(chunks(0), 0u);
}

// num_chunks == 0 is a CHECK, before anything is allocated.
TEST(InProcessTracingV2BridgeSizingTest, ZeroChunksIsRefused) {
  base::TestTaskRunner task_runner;
  EXPECT_DEATH_IF_SUPPORTED(
      InProcessTracingV2Bridge::Create(
          MakeRelay(&task_runner),
          /*num_chunks=*/0, InProcessTracingV2Bridge::kDefaultChunkSize),
      "");
}

// Block a PostTask() inside the runner, i.e. while holding the RelaySequence
// mutex. Close() must not look at or take the runner until that post is done.
TEST(RelaySequenceTest, CloseWaitsForAnAdmittedPost) {
  base::WaitableEvent post_entered;
  base::WaitableEvent release_post;
  base::WaitableEvent close_started;
  std::atomic<bool> post_finished{false};
  std::atomic<bool> close_inspected_runner{false};
  std::atomic<bool> runner_destroyed{false};

  class GatedTaskRunner : public QueuedTaskRunner {
   public:
    GatedTaskRunner(base::WaitableEvent* entered,
                    base::WaitableEvent* release,
                    std::atomic<bool>* finished,
                    std::atomic<bool>* inspected,
                    std::atomic<bool>* destroyed)
        : entered_(entered),
          release_(release),
          finished_(finished),
          inspected_(inspected),
          destroyed_(destroyed) {}

    ~GatedTaskRunner() override {
      EXPECT_TRUE(finished_->load());
      destroyed_->store(true);
    }
    void PostTask(std::function<void()> task) override {
      entered_->Notify();
      release_->Wait();
      QueuedTaskRunner::PostTask(std::move(task));
      finished_->store(true);
    }
    bool RunsTasksOnCurrentThread() const override {
      EXPECT_TRUE(finished_->load());
      inspected_->store(true);
      return false;
    }

   private:
    base::WaitableEvent* const entered_;
    base::WaitableEvent* const release_;
    std::atomic<bool>* const finished_;
    std::atomic<bool>* const inspected_;
    std::atomic<bool>* const destroyed_;
  };

  auto runner = std::make_unique<GatedTaskRunner>(
      &post_entered, &release_post, &post_finished, &close_inspected_runner,
      &runner_destroyed);
  auto* queued = runner.get();
  RelaySequence relay(std::move(runner));
  bool accepted = false;
  bool task_ran = false;
  std::thread poster(
      [&] { accepted = relay.PostTask([&] { task_ran = true; }); });
  post_entered.Wait();
  std::thread closer([&] {
    close_started.Notify();
    auto taken_runner = relay.Close();
    EXPECT_EQ(taken_runner.get(), queued);
    EXPECT_EQ(queued->posts(), 1u);
    queued->RunQueuedTasks();
    taken_runner.reset();
  });
  close_started.Wait();
  EXPECT_FALSE(post_finished.load());
  EXPECT_FALSE(close_inspected_runner.load());
  EXPECT_FALSE(runner_destroyed.load());

  release_post.Notify();
  poster.join();
  closer.join();
  EXPECT_TRUE(accepted);
  EXPECT_TRUE(close_inspected_runner.load());
  EXPECT_TRUE(task_ran);
  EXPECT_TRUE(runner_destroyed.load());
  EXPECT_FALSE(relay.PostTask([] { ADD_FAILURE() << "ran after Close()"; }));
}

}  // namespace
}  // namespace perfetto::tracing_v2
