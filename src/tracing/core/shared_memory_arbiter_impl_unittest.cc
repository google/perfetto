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

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/utils.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/patch_list.h"
#include "src/tracing/test/aligned_buffer_test.h"

namespace perfetto {
namespace {

using testing::Invoke;
using testing::_;

class MockProducerEndpoint : public TracingService::ProducerEndpoint {
 public:
  void RegisterDataSource(const DataSourceDescriptor&) override {}
  void UnregisterDataSource(const std::string&) override {}
  void NotifyFlushComplete(FlushRequestID) override {}
  SharedMemory* shared_memory() const override { return nullptr; }
  size_t shared_buffer_page_size_kb() const override { return 0; }
  std::unique_ptr<TraceWriter> CreateTraceWriter(BufferID) override {
    return nullptr;
  }

  MOCK_METHOD2(CommitData, void(const CommitDataRequest&, CommitDataCallback));
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
INSTANTIATE_TEST_CASE_P(PageSize,
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
    chunks[i] = arbiter_->GetNewChunk({}, 0 /*size_hint*/);
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

// Check that we can actually create up to kMaxWriterID TraceWriter(s).
TEST_P(SharedMemoryArbiterImplTest, WriterIDsAllocation) {
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

// TODO(primiano): add multi-threaded tests.

}  // namespace
}  // namespace perfetto
