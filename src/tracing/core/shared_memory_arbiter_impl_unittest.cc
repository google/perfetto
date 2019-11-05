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
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "src/base/test/gtest_test_suite.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/patch_list.h"
#include "src/tracing/test/aligned_buffer_test.h"
#include "src/tracing/test/fake_producer_endpoint.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using testing::Invoke;
using testing::_;

class MockProducerEndpoint : public TracingService::ProducerEndpoint {
 public:
  void RegisterDataSource(const DataSourceDescriptor&) override {}
  void UnregisterDataSource(const std::string&) override {}
  void NotifyFlushComplete(FlushRequestID) override {}
  void NotifyDataSourceStarted(DataSourceInstanceID) override {}
  void NotifyDataSourceStopped(DataSourceInstanceID) override {}
  void ActivateTriggers(const std::vector<std::string>&) {}
  SharedMemory* shared_memory() const override { return nullptr; }
  size_t shared_buffer_page_size_kb() const override { return 0; }
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID,
      BufferExhaustedPolicy) override {
    return nullptr;
  }
  SharedMemoryArbiter* GetInProcessShmemArbiter() override { return nullptr; }

  MOCK_METHOD2(CommitData, void(const CommitDataRequest&, CommitDataCallback));
  MOCK_METHOD2(RegisterTraceWriter, void(uint32_t, uint32_t));
  MOCK_METHOD1(UnregisterTraceWriter, void(uint32_t));
};

class SharedMemoryArbiterImplTest : public AlignedBufferTest {
 public:
  void SetUp() override {
    AlignedBufferTest::SetUp();
    task_runner_.reset(new base::TestTaskRunner());
    arbiter_.reset(new SharedMemoryArbiterImpl(buf(), buf_size(), page_size(),
                                               &mock_producer_endpoint_,
                                               task_runner_.get()));
  }

  void TearDown() override {
    arbiter_.reset();
    task_runner_.reset();
  }

  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<SharedMemoryArbiterImpl> arbiter_;
  MockProducerEndpoint mock_producer_endpoint_;
  std::function<void(const std::vector<uint32_t>&)> on_pages_complete_;
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
          ASSERT_EQ(i % 5, req.chunks_to_move()[i].target_buffer());
        }
        ASSERT_EQ(2u, req.chunks_to_move()[28].page());
        ASSERT_EQ(1u, req.chunks_to_move()[28].chunk());
        ASSERT_EQ(42u, req.chunks_to_move()[28].target_buffer());
        on_commit_1();
      }));
  PatchList ignored;
  for (size_t i = 0; i < 14 * 2; i++)
    arbiter_->ReturnCompletedChunk(std::move(chunks[i ^ 1]), i % 5, &ignored);
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

// Helper for verifying trace writer id allocations.
class TraceWriterIdChecker : public FakeProducerEndpoint {
 public:
  TraceWriterIdChecker(std::function<void()> checkpoint)
      : checkpoint_(std::move(checkpoint)) {}

  void RegisterTraceWriter(uint32_t id, uint32_t) override {
    EXPECT_GT(id, 0u);
    EXPECT_LE(id, kMaxWriterID);
    if (id > 0 && id <= kMaxWriterID) {
      registered_ids_.set(id - 1);
    }
  }

  void UnregisterTraceWriter(uint32_t id) override {
    if (++unregister_calls_ == kMaxWriterID)
      checkpoint_();

    EXPECT_GT(id, 0u);
    EXPECT_LE(id, kMaxWriterID);
    if (id > 0 && id <= kMaxWriterID) {
      unregistered_ids_.set(id - 1);
    }
  }

  // bit N corresponds to id N+1
  std::bitset<kMaxWriterID> registered_ids_;
  std::bitset<kMaxWriterID> unregistered_ids_;

  int unregister_calls_ = 0;

 private:
  std::function<void()> checkpoint_;
};

// Check that we can actually create up to kMaxWriterID TraceWriter(s).
TEST_P(SharedMemoryArbiterImplTest, WriterIDsAllocation) {
  auto checkpoint = task_runner_->CreateCheckpoint("last_unregistered");

  TraceWriterIdChecker id_checking_endpoint(checkpoint);
  arbiter_.reset(new SharedMemoryArbiterImpl(buf(), buf_size(), page_size(),
                                             &id_checking_endpoint,
                                             task_runner_.get()));
  {
    std::map<WriterID, std::unique_ptr<TraceWriter>> writers;

    for (size_t i = 0; i < kMaxWriterID; i++) {
      std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(0);
      ASSERT_TRUE(writer);
      WriterID writer_id = writer->writer_id();
      ASSERT_TRUE(writers.emplace(writer_id, std::move(writer)).second);
    }

    // A further call should return a null impl of trace writer as we exhausted
    // writer IDs.
    ASSERT_EQ(arbiter_->CreateTraceWriter(0)->writer_id(), 0);
  }

  // This should run the Register/UnregisterTraceWriter tasks enqueued by the
  // memory arbiter.
  task_runner_->RunUntilCheckpoint("last_unregistered", 15000);

  EXPECT_TRUE(id_checking_endpoint.registered_ids_.all());
  EXPECT_TRUE(id_checking_endpoint.unregistered_ids_.all());
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
  arbiter_->ReturnCompletedChunk(std::move(chunks[0]), 0, &ignored);

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

// TODO(primiano): add multi-threaded tests.

}  // namespace
}  // namespace perfetto
