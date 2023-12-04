/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/tracing/core/shared_memory_arbiter_impl.h"

#include <bitset>
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "src/base/test/gtest_test_suite.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/core/patch_list.h"
#include "src/tracing/test/aligned_buffer_test.h"
#include "src/tracing/test/mock_producer_endpoint.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

using testing::_;
using testing::Between;
using testing::Invoke;
using testing::Mock;
using testing::NiceMock;
using testing::UnorderedElementsAreArray;

using ShmemMode = SharedMemoryABI::ShmemMode;

class SharedMemoryArbiterImplTest : public AlignedBufferTest {
 public:
  void SetUp() override {
    default_layout_ =
        SharedMemoryArbiterImpl::default_page_layout_for_testing();
    AlignedBufferTest::SetUp();
    task_runner_.reset(new base::TestTaskRunner());
    arbiter_.reset(new SharedMemoryArbiterImpl(
        buf(), buf_size(), ShmemMode::kDefault, page_size(),
        &mock_producer_endpoint_, task_runner_.get()));
  }

  bool IsArbiterFullyBound() { return arbiter_->fully_bound_; }

  void TearDown() override {
    arbiter_.reset();
    task_runner_.reset();
    SharedMemoryArbiterImpl::set_default_layout_for_testing(default_layout_);
  }

  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<SharedMemoryArbiterImpl> arbiter_;
  NiceMock<MockProducerEndpoint> mock_producer_endpoint_;
  std::function<void(const std::vector<uint32_t>&)> on_pages_complete_;
  SharedMemoryABI::PageLayout default_layout_;
};

size_t const kPageSizes[] = {4096, 65536};
INSTANTIATE_TEST_SUITE_P(PageSize,
                         SharedMemoryArbiterImplTest,
                         ::testing::ValuesIn(kPageSizes));

// The buffer has 14 pages (kNumPages), each will be partitioned in 14 chunks.
// The test requests 30 chunks (2 full pages + 2 chunks from a 3rd page) and
// releases them in different batches. It tests the consistency of the batches
// and the releasing order.
TEST_P(SharedMemoryArbiterImplTest, GetAndReturnChunks) {
  SharedMemoryArbiterImpl::set_default_layout_for_testing(
      SharedMemoryABI::PageLayout::kPageDiv14);
  static constexpr size_t kTotChunks = kNumPages * 14;
  SharedMemoryABI::Chunk chunks[kTotChunks];
  for (size_t i = 0; i < 14 * 2 + 2; i++) {
    chunks[i] = arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kStall);
    ASSERT_TRUE(chunks[i].is_valid());
  }

  // Finally return the first 28 chunks (full 2 pages) and only the 2nd chunk of
  // the 2rd page. Chunks are release in interleaved order: 1,0,3,2,5,4,7,6.
  // Check that the notification callback is posted and order is consistent.
  auto on_commit_1 = task_runner_->CreateCheckpoint("on_commit_1");
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
      .WillOnce(Invoke([on_commit_1](const CommitDataRequest& req,
                                     MockProducerEndpoint::CommitDataCallback) {
        ASSERT_EQ(14 * 2 + 1, req.chunks_to_move_size());
        for (size_t i = 0; i < 14 * 2; i++) {
          ASSERT_EQ(i / 14, req.chunks_to_move()[i].page());
          ASSERT_EQ((i % 14) ^ 1, req.chunks_to_move()[i].chunk());
          ASSERT_EQ(i % 5 + 1, req.chunks_to_move()[i].target_buffer());
        }
        ASSERT_EQ(2u, req.chunks_to_move()[28].page());
        ASSERT_EQ(1u, req.chunks_to_move()[28].chunk());
        ASSERT_EQ(42u, req.chunks_to_move()[28].target_buffer());
        on_commit_1();
      }));
  PatchList ignored;
  for (size_t i = 0; i < 14 * 2; i++) {
    arbiter_->ReturnCompletedChunk(std::move(chunks[i ^ 1]), i % 5 + 1,
                                   &ignored);
  }
  arbiter_->ReturnCompletedChunk(std::move(chunks[29]), 42, &ignored);
  task_runner_->RunUntilCheckpoint("on_commit_1");

  // Then release the 1st chunk of the 3rd page, and check that we get a
  // notification for that as well.
  auto on_commit_2 = task_runner_->CreateCheckpoint("on_commit_2");
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
      .WillOnce(Invoke([on_commit_2](const CommitDataRequest& req,
                                     MockProducerEndpoint::CommitDataCallback) {
        ASSERT_EQ(1, req.chunks_to_move_size());
        ASSERT_EQ(2u, req.chunks_to_move()[0].page());
        ASSERT_EQ(0u, req.chunks_to_move()[0].chunk());
        ASSERT_EQ(43u, req.chunks_to_move()[0].target_buffer());
        on_commit_2();
      }));
  arbiter_->ReturnCompletedChunk(std::move(chunks[28]), 43, &ignored);
  task_runner_->RunUntilCheckpoint("on_commit_2");
}

TEST_P(SharedMemoryArbiterImplTest, BatchCommits) {
  SharedMemoryArbiterImpl::set_default_layout_for_testing(
      SharedMemoryABI::PageLayout::kPageDiv1);

  // Batching period is 0s - chunks are being committed as soon as they are
  // returned.
  SharedMemoryABI::Chunk chunk =
      arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDefault);
  ASSERT_TRUE(chunk.is_valid());
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _)).Times(1);
  PatchList ignored;
  arbiter_->ReturnCompletedChunk(std::move(chunk), 0, &ignored);
  task_runner_->RunUntilIdle();
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&mock_producer_endpoint_));

  // Since we cannot explicitly control the passage of time in task_runner_, to
  // simulate a non-zero batching period and a commit at the end of it, set the
  // batching duration to a very large value and call
  // FlushPendingCommitDataRequests to manually trigger the commit.
  arbiter_->SetDirectSMBPatchingSupportedByService();
  ASSERT_TRUE(arbiter_->EnableDirectSMBPatching());
  arbiter_->SetBatchCommitsDuration(UINT32_MAX);

  // First chunk that will be batched. CommitData should not be called
  // immediately this time.
  chunk = arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDefault);
  ASSERT_TRUE(chunk.is_valid());
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _)).Times(0);
  // We'll pretend that the chunk needs patching. This is done in order to
  // verify that chunks that need patching are not marked as complete (i.e. they
  // are kept in state kChunkBeingWritten) before the batching period ends - in
  // case a patch for them arrives during the batching period.
  chunk.SetFlag(SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
  arbiter_->ReturnCompletedChunk(std::move(chunk), 1, &ignored);
  task_runner_->RunUntilIdle();
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&mock_producer_endpoint_));
  ASSERT_EQ(SharedMemoryABI::kChunkBeingWritten,
            arbiter_->shmem_abi_for_testing()->GetChunkState(1u, 0u));

  // Add a second chunk to the batch. This should also not trigger an immediate
  // call to CommitData.
  chunk = arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDefault);
  ASSERT_TRUE(chunk.is_valid());
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _)).Times(0);
  arbiter_->ReturnCompletedChunk(std::move(chunk), 2, &ignored);
  task_runner_->RunUntilIdle();
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&mock_producer_endpoint_));
  // This chunk does not need patching, so it should be marked as complete even
  // before the end of the batching period - to allow the service to read it in
  // full.
  ASSERT_EQ(SharedMemoryABI::kChunkComplete,
            arbiter_->shmem_abi_for_testing()->GetChunkState(2u, 0u));

  // Make sure that CommitData gets called once (should happen at the end
  // of the batching period), with the two chunks in the batch.
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
      .WillOnce(Invoke([](const CommitDataRequest& req,
                          MockProducerEndpoint::CommitDataCallback) {
        ASSERT_EQ(2, req.chunks_to_move_size());

        // Verify that this is the first chunk that we expect to have been
        // batched.
        ASSERT_EQ(1u, req.chunks_to_move()[0].page());
        ASSERT_EQ(0u, req.chunks_to_move()[0].chunk());
        ASSERT_EQ(1u, req.chunks_to_move()[0].target_buffer());

        // Verify that this is the second chunk that we expect to have been
        // batched.
        ASSERT_EQ(2u, req.chunks_to_move()[1].page());
        ASSERT_EQ(0u, req.chunks_to_move()[1].chunk());
        ASSERT_EQ(2u, req.chunks_to_move()[1].target_buffer());
      }));

  // Pretend we've reached the end of the batching period.
  arbiter_->FlushPendingCommitDataRequests();
}

TEST_P(SharedMemoryArbiterImplTest, UseShmemEmulation) {
  arbiter_.reset(new SharedMemoryArbiterImpl(
      buf(), buf_size(), ShmemMode::kShmemEmulation, page_size(),
      &mock_producer_endpoint_, task_runner_.get()));

  SharedMemoryArbiterImpl::set_default_layout_for_testing(
      SharedMemoryABI::PageLayout::kPageDiv1);

  size_t page_idx;
  size_t chunk_idx;
  auto* abi = arbiter_->shmem_abi_for_testing();

  // Test returning a completed chunk.
  SharedMemoryABI::Chunk chunk =
      arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDefault);
  std::tie(page_idx, chunk_idx) = abi->GetPageAndChunkIndex(chunk);
  ASSERT_TRUE(chunk.is_valid());
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _)).Times(1);
  PatchList ignored;
  arbiter_->ReturnCompletedChunk(std::move(chunk), 0, &ignored);
  task_runner_->RunUntilIdle();
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&mock_producer_endpoint_));
  // When running in the emulation mode, the chunk is freed when the
  // CommitDataRequest is flushed.
  ASSERT_EQ(
      SharedMemoryABI::kChunkFree,
      arbiter_->shmem_abi_for_testing()->GetChunkState(page_idx, chunk_idx));

  // Direct patching is supported in the emulation mode.
  arbiter_->SetDirectSMBPatchingSupportedByService();
  ASSERT_TRUE(arbiter_->EnableDirectSMBPatching());

  chunk = arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDefault);
  std::tie(page_idx, chunk_idx) = abi->GetPageAndChunkIndex(chunk);
  ASSERT_TRUE(chunk.is_valid());
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
      .WillOnce(Invoke([&](const CommitDataRequest& req,
                           MockProducerEndpoint::CommitDataCallback) {
        ASSERT_EQ(1, req.chunks_to_move_size());

        ASSERT_EQ(page_idx, req.chunks_to_move()[0].page());
        ASSERT_EQ(chunk_idx, req.chunks_to_move()[0].chunk());
        ASSERT_EQ(1u, req.chunks_to_move()[0].target_buffer());

        // The request should contain chunk data.
        ASSERT_TRUE(req.chunks_to_move()[0].has_data());
      }));
  chunk.SetFlag(SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
  arbiter_->ReturnCompletedChunk(std::move(chunk), 1, &ignored);
  task_runner_->RunUntilIdle();
  ASSERT_TRUE(Mock::VerifyAndClearExpectations(&mock_producer_endpoint_));
  // A chunk is freed after being flushed.
  ASSERT_EQ(
      SharedMemoryABI::kChunkFree,
      arbiter_->shmem_abi_for_testing()->GetChunkState(page_idx, chunk_idx));
}

// Check that we can actually create up to kMaxWriterID TraceWriter(s).
TEST_P(SharedMemoryArbiterImplTest, WriterIDsAllocation) {
  auto checkpoint = task_runner_->CreateCheckpoint("last_unregistered");

  std::vector<uint32_t> registered_ids;
  std::vector<uint32_t> unregistered_ids;

  ON_CALL(mock_producer_endpoint_, RegisterTraceWriter)
      .WillByDefault(
          [&](uint32_t id, uint32_t) { registered_ids.push_back(id); });
  ON_CALL(mock_producer_endpoint_, UnregisterTraceWriter)
      .WillByDefault([&](uint32_t id) {
        unregistered_ids.push_back(id);
        if (unregistered_ids.size() == kMaxWriterID) {
          checkpoint();
        }
      });
  {
    std::map<WriterID, std::unique_ptr<TraceWriter>> writers;

    for (size_t i = 0; i < kMaxWriterID; i++) {
      std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(1);
      ASSERT_TRUE(writer);
      WriterID writer_id = writer->writer_id();
      ASSERT_TRUE(writers.emplace(writer_id, std::move(writer)).second);
    }

    // A further call should return a null impl of trace writer as we exhausted
    // writer IDs.
    ASSERT_EQ(arbiter_->CreateTraceWriter(1)->writer_id(), 0);
  }

  // This should run the Register/UnregisterTraceWriter tasks enqueued by the
  // memory arbiter.
  task_runner_->RunUntilCheckpoint("last_unregistered", 15000);

  std::vector<uint32_t> expected_ids;  // 1..kMaxWriterID
  for (uint32_t i = 1; i <= kMaxWriterID; i++)
    expected_ids.push_back(i);
  EXPECT_THAT(registered_ids, UnorderedElementsAreArray(expected_ids));
  EXPECT_THAT(unregistered_ids, UnorderedElementsAreArray(expected_ids));
}

TEST_P(SharedMemoryArbiterImplTest, Shutdown) {
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(1);
  EXPECT_TRUE(writer);
  EXPECT_FALSE(arbiter_->TryShutdown());

  // We still get a valid trace writer after shutdown, but it's a null one
  // that's not connected to the arbiter.
  std::unique_ptr<TraceWriter> writer2 = arbiter_->CreateTraceWriter(2);
  EXPECT_TRUE(writer2);
  EXPECT_EQ(writer2->writer_id(), 0);

  // Shutdown will succeed once the only non-null writer goes away.
  writer.reset();
  EXPECT_TRUE(arbiter_->TryShutdown());
}

// Verify that getting a new chunk doesn't stall when kDrop policy is chosen.
TEST_P(SharedMemoryArbiterImplTest, BufferExhaustedPolicyDrop) {
  // Grab all chunks in the SMB.
  SharedMemoryArbiterImpl::set_default_layout_for_testing(
      SharedMemoryABI::PageLayout::kPageDiv1);
  static constexpr size_t kTotChunks = kNumPages;
  SharedMemoryABI::Chunk chunks[kTotChunks];
  for (size_t i = 0; i < kTotChunks; i++) {
    chunks[i] = arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDrop);
    ASSERT_TRUE(chunks[i].is_valid());
  }

  // SMB is exhausted, thus GetNewChunk() should return an invalid chunk. In
  // kStall mode, this would stall.
  SharedMemoryABI::Chunk invalid_chunk =
      arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDrop);
  ASSERT_FALSE(invalid_chunk.is_valid());

  // Returning the chunk is not enough to be able to reacquire it.
  PatchList ignored;
  arbiter_->ReturnCompletedChunk(std::move(chunks[0]), 1, &ignored);

  invalid_chunk = arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDrop);
  ASSERT_FALSE(invalid_chunk.is_valid());

  // After releasing the chunk as free, we can reacquire it.
  chunks[0] =
      arbiter_->shmem_abi_for_testing()->TryAcquireChunkForReading(0, 0);
  ASSERT_TRUE(chunks[0].is_valid());
  arbiter_->shmem_abi_for_testing()->ReleaseChunkAsFree(std::move(chunks[0]));

  chunks[0] = arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDrop);
  ASSERT_TRUE(chunks[0].is_valid());
}

TEST_P(SharedMemoryArbiterImplTest, CreateUnboundAndBind) {
  auto checkpoint_writer = task_runner_->CreateCheckpoint("writer_registered");
  auto checkpoint_flush = task_runner_->CreateCheckpoint("flush_completed");

  // Create an unbound arbiter and bind immediately.
  arbiter_.reset(new SharedMemoryArbiterImpl(
      buf(), buf_size(), ShmemMode::kDefault, page_size(), nullptr, nullptr));
  arbiter_->BindToProducerEndpoint(&mock_producer_endpoint_,
                                   task_runner_.get());
  EXPECT_TRUE(IsArbiterFullyBound());

  // Trace writer should be registered in a non-delayed task.
  EXPECT_CALL(mock_producer_endpoint_, RegisterTraceWriter(_, 1))
      .WillOnce(testing::InvokeWithoutArgs(checkpoint_writer));
  std::unique_ptr<TraceWriter> writer =
      arbiter_->CreateTraceWriter(1, BufferExhaustedPolicy::kDrop);
  task_runner_->RunUntilCheckpoint("writer_registered", 5000);

  // Commits/flushes should be sent right away.
  EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
      .WillOnce(testing::InvokeArgument<1>());
  writer->Flush(checkpoint_flush);
  task_runner_->RunUntilCheckpoint("flush_completed", 5000);
}

// Startup tracing tests are run with the arbiter in either bound or unbound
// initial state. Startup tracing in bound state can still be useful, e.g. in
// integration tests or to enable tracing in the consumer process immediately
// before/after instructing the service to start a session, avoiding the
// round-trip time through the service.
enum class InitialBindingState { kUnbound, kBound };

class SharedMemoryArbiterImplStartupTracingTest
    : public SharedMemoryArbiterImplTest {
 public:
  void SetupArbiter(InitialBindingState initial_state) {
    if (initial_state == InitialBindingState::kUnbound) {
      arbiter_.reset(
          new SharedMemoryArbiterImpl(buf(), buf_size(), ShmemMode::kDefault,
                                      page_size(), nullptr, nullptr));
      EXPECT_FALSE(IsArbiterFullyBound());
    } else {
      // A bound arbiter is already set up by the base class.
      EXPECT_TRUE(IsArbiterFullyBound());
    }
  }

  void EnsureArbiterBoundToEndpoint(InitialBindingState initial_state) {
    if (initial_state == InitialBindingState::kUnbound) {
      arbiter_->BindToProducerEndpoint(&mock_producer_endpoint_,
                                       task_runner_.get());
    }
  }

  void TestStartupTracing(InitialBindingState initial_state) {
    constexpr uint16_t kTargetBufferReservationId1 = 1;
    constexpr uint16_t kTargetBufferReservationId2 = 2;

    SetupArbiter(initial_state);

    // Create an unbound startup writer.
    std::unique_ptr<TraceWriter> writer =
        arbiter_->CreateStartupTraceWriter(kTargetBufferReservationId1);
    EXPECT_FALSE(IsArbiterFullyBound());

    // Write two packets while unbound (if InitialBindingState::kUnbound) and
    // flush the chunk after each packet. The writer will return the chunk to
    // the arbiter and grab a new chunk for the second packet. The flush should
    // only add the chunk into the queued commit request.
    for (int i = 0; i < 2; i++) {
      {
        auto packet = writer->NewTracePacket();
        packet->set_for_testing()->set_str("foo");
      }
      writer->Flush();
    }

    // Bind to producer endpoint if initially unbound. This should not register
    // the trace writer yet, because its buffer reservation is still unbound.
    EnsureArbiterBoundToEndpoint(initial_state);
    EXPECT_FALSE(IsArbiterFullyBound());

    // Write another packet into another chunk and queue it.
    {
      auto packet = writer->NewTracePacket();
      packet->set_for_testing()->set_str("foo");
    }
    bool flush_completed = false;
    writer->Flush([&flush_completed] { flush_completed = true; });

    // Bind the buffer reservation to a buffer. Trace writer should be
    // registered and queued commits flushed.
    EXPECT_CALL(mock_producer_endpoint_, RegisterTraceWriter(_, 42));
    EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
        .WillOnce(Invoke([](const CommitDataRequest& req,
                            MockProducerEndpoint::CommitDataCallback callback) {
          ASSERT_EQ(3, req.chunks_to_move_size());
          EXPECT_EQ(42u, req.chunks_to_move()[0].target_buffer());
          EXPECT_EQ(42u, req.chunks_to_move()[1].target_buffer());
          EXPECT_EQ(42u, req.chunks_to_move()[2].target_buffer());
          callback();
        }));

    arbiter_->BindStartupTargetBuffer(kTargetBufferReservationId1, 42);
    EXPECT_TRUE(IsArbiterFullyBound());

    testing::Mock::VerifyAndClearExpectations(&mock_producer_endpoint_);
    EXPECT_TRUE(flush_completed);

    // Creating a new startup writer for the same buffer posts an immediate task
    // to register it.
    auto checkpoint_register1b =
        task_runner_->CreateCheckpoint("writer1b_registered");
    EXPECT_CALL(mock_producer_endpoint_, RegisterTraceWriter(_, 42))
        .WillOnce(testing::InvokeWithoutArgs(checkpoint_register1b));
    std::unique_ptr<TraceWriter> writer1b =
        arbiter_->CreateStartupTraceWriter(kTargetBufferReservationId1);
    task_runner_->RunUntilCheckpoint("writer1b_registered", 5000);

    // And a commit on this new writer should be flushed to the right buffer,
    // too.
    EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
        .WillOnce(Invoke([](const CommitDataRequest& req,
                            MockProducerEndpoint::CommitDataCallback callback) {
          ASSERT_EQ(1, req.chunks_to_move_size());
          EXPECT_EQ(42u, req.chunks_to_move()[0].target_buffer());
          callback();
        }));
    {
      auto packet = writer1b->NewTracePacket();
      packet->set_for_testing()->set_str("foo");
    }
    flush_completed = false;
    writer1b->Flush([&flush_completed] { flush_completed = true; });

    testing::Mock::VerifyAndClearExpectations(&mock_producer_endpoint_);
    EXPECT_TRUE(flush_completed);

    // Create another startup writer for another target buffer, which puts the
    // arbiter back into unbound state.
    std::unique_ptr<TraceWriter> writer2 =
        arbiter_->CreateStartupTraceWriter(kTargetBufferReservationId2);
    EXPECT_FALSE(IsArbiterFullyBound());

    // Write a chunk into both writers. Both should be queued up into the next
    // commit request.
    {
      auto packet = writer->NewTracePacket();
      packet->set_for_testing()->set_str("foo");
    }
    writer->Flush();
    {
      auto packet = writer2->NewTracePacket();
      packet->set_for_testing()->set_str("bar");
    }
    flush_completed = false;
    writer2->Flush([&flush_completed] { flush_completed = true; });

    // Destroy the first trace writer, which should cause the arbiter to post a
    // task to unregister it.
    auto checkpoint_writer =
        task_runner_->CreateCheckpoint("writer_unregistered");
    EXPECT_CALL(mock_producer_endpoint_,
                UnregisterTraceWriter(writer->writer_id()))
        .WillOnce(testing::InvokeWithoutArgs(checkpoint_writer));
    writer.reset();
    task_runner_->RunUntilCheckpoint("writer_unregistered", 5000);

    // Bind the second buffer reservation to a buffer. Second trace writer
    // should be registered and queued commits flushed.
    EXPECT_CALL(mock_producer_endpoint_, RegisterTraceWriter(_, 23));
    EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
        .WillOnce(Invoke([](const CommitDataRequest& req,
                            MockProducerEndpoint::CommitDataCallback callback) {
          ASSERT_EQ(2, req.chunks_to_move_size());
          EXPECT_EQ(42u, req.chunks_to_move()[0].target_buffer());
          EXPECT_EQ(23u, req.chunks_to_move()[1].target_buffer());
          callback();
        }));

    arbiter_->BindStartupTargetBuffer(kTargetBufferReservationId2, 23);
    EXPECT_TRUE(IsArbiterFullyBound());

    testing::Mock::VerifyAndClearExpectations(&mock_producer_endpoint_);
    EXPECT_TRUE(flush_completed);
  }

  void TestAbortStartupTracingForReservation(
      InitialBindingState initial_state) {
    constexpr uint16_t kTargetBufferReservationId1 = 1;
    constexpr uint16_t kTargetBufferReservationId2 = 2;

    SetupArbiter(initial_state);

    // Create two unbound startup writers the same target buffer.
    SharedMemoryABI* shmem_abi = arbiter_->shmem_abi_for_testing();
    std::unique_ptr<TraceWriter> writer =
        arbiter_->CreateStartupTraceWriter(kTargetBufferReservationId1);
    std::unique_ptr<TraceWriter> writer2 =
        arbiter_->CreateStartupTraceWriter(kTargetBufferReservationId1);

    // Write two packet while unbound and flush the chunk after each packet. The
    // writer will return the chunk to the arbiter and grab a new chunk for the
    // second packet. The flush should only add the chunk into the queued commit
    // request.
    for (int i = 0; i < 2; i++) {
      {
        auto packet = writer->NewTracePacket();
        packet->set_for_testing()->set_str("foo");
      }
      writer->Flush();
    }

    // Expectations for the below calls.
    EXPECT_CALL(mock_producer_endpoint_, RegisterTraceWriter(_, _)).Times(0);
    EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
        .WillOnce(Invoke([shmem_abi](const CommitDataRequest& req,
                                     MockProducerEndpoint::CommitDataCallback) {
          ASSERT_EQ(2, req.chunks_to_move_size());
          for (size_t i = 0; i < 2; i++) {
            EXPECT_EQ(0u, req.chunks_to_move()[i].target_buffer());
            SharedMemoryABI::Chunk chunk = shmem_abi->TryAcquireChunkForReading(
                req.chunks_to_move()[i].page(),
                req.chunks_to_move()[i].chunk());
            shmem_abi->ReleaseChunkAsFree(std::move(chunk));
          }
        }));

    // Abort the first session. This should resolve the two chunks committed up
    // to this point to an invalid target buffer (ID 0). They will remain
    // buffered until bound to an endpoint (if InitialBindingState::kUnbound).
    arbiter_->AbortStartupTracingForReservation(kTargetBufferReservationId1);

    // Destroy a writer that was created before the abort. This should not cause
    // crashes.
    EXPECT_CALL(mock_producer_endpoint_,
                UnregisterTraceWriter(writer2->writer_id()))
        .Times(Between(0, 1));  // Depending on `initial_state`.
    writer2.reset();

    // Bind to producer endpoint if unbound. The trace writer should not be
    // registered as its target buffer is invalid. Since no startup sessions are
    // active anymore, the arbiter should be fully bound. The commit data
    // request is flushed.
    EnsureArbiterBoundToEndpoint(initial_state);
    EXPECT_TRUE(IsArbiterFullyBound());

    // SMB should be free again, as no writer holds on to any chunk anymore.
    for (size_t i = 0; i < shmem_abi->num_pages(); i++)
      EXPECT_TRUE(shmem_abi->is_page_free(i));

    // Write another packet into another chunk and commit it. It should be sent
    // to the arbiter with invalid target buffer (ID 0).
    {
      auto packet = writer->NewTracePacket();
      packet->set_for_testing()->set_str("foo");
    }
    EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
        .WillOnce(Invoke(
            [shmem_abi](const CommitDataRequest& req,
                        MockProducerEndpoint::CommitDataCallback callback) {
              ASSERT_EQ(1, req.chunks_to_move_size());
              EXPECT_EQ(0u, req.chunks_to_move()[0].target_buffer());
              SharedMemoryABI::Chunk chunk =
                  shmem_abi->TryAcquireChunkForReading(
                      req.chunks_to_move()[0].page(),
                      req.chunks_to_move()[0].chunk());
              shmem_abi->ReleaseChunkAsFree(std::move(chunk));
              callback();
            }));
    bool flush_completed = false;
    writer->Flush([&flush_completed] { flush_completed = true; });
    EXPECT_TRUE(flush_completed);

    // Creating a new startup writer for the same buffer does not cause it to
    // register.
    EXPECT_CALL(mock_producer_endpoint_, RegisterTraceWriter(_, _)).Times(0);
    std::unique_ptr<TraceWriter> writer1b =
        arbiter_->CreateStartupTraceWriter(kTargetBufferReservationId1);

    // And a commit on this new writer should again be flushed to the invalid
    // target buffer.
    {
      auto packet = writer1b->NewTracePacket();
      packet->set_for_testing()->set_str("foo");
    }
    EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
        .WillOnce(Invoke(
            [shmem_abi](const CommitDataRequest& req,
                        MockProducerEndpoint::CommitDataCallback callback) {
              ASSERT_EQ(1, req.chunks_to_move_size());
              EXPECT_EQ(0u, req.chunks_to_move()[0].target_buffer());
              SharedMemoryABI::Chunk chunk =
                  shmem_abi->TryAcquireChunkForReading(
                      req.chunks_to_move()[0].page(),
                      req.chunks_to_move()[0].chunk());
              shmem_abi->ReleaseChunkAsFree(std::move(chunk));
              callback();
            }));
    flush_completed = false;
    writer1b->Flush([&flush_completed] { flush_completed = true; });
    EXPECT_TRUE(flush_completed);

    // Create another startup writer for another target buffer, which puts the
    // arbiter back into unbound state.
    std::unique_ptr<TraceWriter> writer3 =
        arbiter_->CreateStartupTraceWriter(kTargetBufferReservationId2);
    EXPECT_FALSE(IsArbiterFullyBound());

    // Write a chunk into both writers. Both should be queued up into the next
    // commit request.
    {
      auto packet = writer->NewTracePacket();
      packet->set_for_testing()->set_str("foo");
    }
    writer->Flush();
    {
      auto packet = writer3->NewTracePacket();
      packet->set_for_testing()->set_str("bar");
    }
    flush_completed = false;
    writer3->Flush([&flush_completed] { flush_completed = true; });

    // Destroy the first trace writer, which should cause the arbiter to post a
    // task to unregister it.
    auto checkpoint_writer =
        task_runner_->CreateCheckpoint("writer_unregistered");
    EXPECT_CALL(mock_producer_endpoint_,
                UnregisterTraceWriter(writer->writer_id()))
        .WillOnce(testing::InvokeWithoutArgs(checkpoint_writer));
    writer.reset();
    task_runner_->RunUntilCheckpoint("writer_unregistered", 5000);

    // Abort the second session. Its commits should now also be associated with
    // target buffer 0, and both writers' commits flushed.
    EXPECT_CALL(mock_producer_endpoint_, RegisterTraceWriter(_, _)).Times(0);
    EXPECT_CALL(mock_producer_endpoint_, CommitData(_, _))
        .WillOnce(Invoke(
            [shmem_abi](const CommitDataRequest& req,
                        MockProducerEndpoint::CommitDataCallback callback) {
              ASSERT_EQ(2, req.chunks_to_move_size());
              for (size_t i = 0; i < 2; i++) {
                EXPECT_EQ(0u, req.chunks_to_move()[i].target_buffer());
                SharedMemoryABI::Chunk chunk =
                    shmem_abi->TryAcquireChunkForReading(
                        req.chunks_to_move()[i].page(),
                        req.chunks_to_move()[i].chunk());
                shmem_abi->ReleaseChunkAsFree(std::move(chunk));
              }
              callback();
            }));

    arbiter_->AbortStartupTracingForReservation(kTargetBufferReservationId2);
    EXPECT_TRUE(IsArbiterFullyBound());
    EXPECT_TRUE(flush_completed);

    // SMB should be free again, as no writer holds on to any chunk anymore.
    for (size_t i = 0; i < shmem_abi->num_pages(); i++)
      EXPECT_TRUE(shmem_abi->is_page_free(i));
  }
};

INSTANTIATE_TEST_SUITE_P(PageSize,
                         SharedMemoryArbiterImplStartupTracingTest,
                         ::testing::ValuesIn(kPageSizes));

TEST_P(SharedMemoryArbiterImplStartupTracingTest, StartupTracingUnbound) {
  TestStartupTracing(InitialBindingState::kUnbound);
}

TEST_P(SharedMemoryArbiterImplStartupTracingTest, StartupTracingBound) {
  TestStartupTracing(InitialBindingState::kBound);
}

TEST_P(SharedMemoryArbiterImplStartupTracingTest,
       AbortStartupTracingForReservationUnbound) {
  TestAbortStartupTracingForReservation(InitialBindingState::kUnbound);
}

TEST_P(SharedMemoryArbiterImplStartupTracingTest,
       AbortStartupTracingForReservationBound) {
  TestAbortStartupTracingForReservation(InitialBindingState::kBound);
}

// TODO(primiano): add multi-threaded tests.

}  // namespace perfetto
