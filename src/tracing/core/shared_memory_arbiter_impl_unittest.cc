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

#include "gtest/gtest.h"
#include "perfetto/base/utils.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/test/aligned_buffer_test.h"

namespace perfetto {
namespace {

constexpr size_t kMaxWriterID = SharedMemoryABI::kMaxWriterID;

class SharedMemoryArbiterImplTest : public AlignedBufferTest {
 public:
  void SetUp() override {
    AlignedBufferTest::SetUp();
    auto callback = [this](const std::vector<uint32_t>& arg) {
      if (on_pages_complete_)
        on_pages_complete_(arg);
    };
    task_runner_.reset(new base::TestTaskRunner());
    arbiter_.reset(new SharedMemoryArbiterImpl(buf(), buf_size(), page_size(),
                                               callback, task_runner_.get()));
  }

  void TearDown() override {
    arbiter_.reset();
    task_runner_.reset();
  }

  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<SharedMemoryArbiterImpl> arbiter_;
  std::function<void(const std::vector<uint32_t>&)> on_pages_complete_;
};

size_t const kPageSizes[] = {4096, 65536};
INSTANTIATE_TEST_CASE_P(PageSize,
                        SharedMemoryArbiterImplTest,
                        ::testing::ValuesIn(kPageSizes));

// Checks that chunks that target different buffer IDs are placed in different
// pages.
TEST_P(SharedMemoryArbiterImplTest, ChunksAllocationByTargetBufferID) {
  SharedMemoryArbiterImpl::set_default_layout_for_testing(
      SharedMemoryABI::PageLayout::kPageDiv4);
  SharedMemoryABI::Chunk chunks[8];
  chunks[0] = arbiter_->GetNewChunk({}, 1 /* target buffer id */, 0);
  chunks[1] = arbiter_->GetNewChunk({}, 1 /* target buffer id */, 0);
  chunks[2] = arbiter_->GetNewChunk({}, 1 /* target buffer id */, 0);
  chunks[3] = arbiter_->GetNewChunk({}, 2 /* target buffer id */, 0);
  chunks[4] = arbiter_->GetNewChunk({}, 1 /* target buffer id */, 0);
  chunks[5] = arbiter_->GetNewChunk({}, 1 /* target buffer id */, 0);
  chunks[6] = arbiter_->GetNewChunk({}, 3 /* target buffer id */, 0);
  chunks[7] = arbiter_->GetNewChunk({}, 3 /* target buffer id */, 0);

  // "first" == "page index", "second" == "chunk index".
  std::pair<size_t, size_t> idx[base::ArraySize(chunks)];
  for (size_t i = 0; i < base::ArraySize(chunks); i++)
    idx[i] = arbiter_->shmem_abi_for_testing()->GetPageAndChunkIndex(chunks[i]);

  // The first three chunks should lay in the same page, as they target the same
  // buffer id (1).
  EXPECT_EQ(idx[0].first, idx[1].first);
  EXPECT_EQ(idx[0].first, idx[2].first);

  // Check also that the chunk IDs are different.
  EXPECT_NE(idx[0].second, idx[1].second);
  EXPECT_NE(idx[1].second, idx[2].second);
  EXPECT_NE(idx[0].second, idx[2].second);

  // The next one instead should be given a dedicated page because it targets
  // a different buffer id (2);
  EXPECT_NE(idx[2].first, idx[3].first);

  // Hoever the next two chunks should be able to fit back into the same page.
  EXPECT_EQ(idx[4].first, idx[5].first);
  EXPECT_NE(idx[4].second, idx[5].second);

  // Similarly the last two chunks should be able to share the same page, but
  // not any page of the previous chunks.
  EXPECT_NE(idx[0].first, idx[6].first);
  EXPECT_NE(idx[3].first, idx[6].first);
  EXPECT_EQ(idx[6].first, idx[7].first);
  EXPECT_NE(idx[6].second, idx[7].second);

  // TODO(primiano): check that after saturating all the pages, the arbiter
  // goes back and reuses free chunks of previous pages. e.g., at some point
  // a chunk targeting buffer id == 1 should be placed into (page:0, chunk:3).
}

// The buffer has 14 pages (kNumPages), each will be partitioned in 14 chunks.
// The test requests all 14 * 14 chunks, alternating amongst 14 target buf IDs.
// Because a chunk can share a page only if all other chunks in the page have
// the same target buffer ID, there is only one possible final distribution:
// each page is filled with chunks that all belong to the same buffer ID.
TEST_P(SharedMemoryArbiterImplTest, GetAndReturnChunks) {
  SharedMemoryArbiterImpl::set_default_layout_for_testing(
      SharedMemoryABI::PageLayout::kPageDiv14);
  static constexpr size_t kTotChunks = kNumPages * 14;
  SharedMemoryABI::Chunk chunks[kTotChunks];
  for (size_t i = 0; i < kTotChunks; i++) {
    BufferID target_buffer = i % 14;
    chunks[i] = arbiter_->GetNewChunk({}, target_buffer, 0 /*size_hint*/);
    ASSERT_TRUE(chunks[i].is_valid());
  }

  SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
  for (size_t page_idx = 0; page_idx < kNumPages; page_idx++) {
    ASSERT_FALSE(abi->is_page_free(page_idx));
    ASSERT_EQ(0u, abi->GetFreeChunks(page_idx));
    const uint32_t page_layout = abi->page_layout_dbg(page_idx);
    ASSERT_EQ(14u, SharedMemoryABI::GetNumChunksForLayout(page_layout));
    ASSERT_EQ(page_idx % 14, abi->page_header(page_idx)->target_buffer.load());
    for (size_t chunk_idx = 0; chunk_idx < 14; chunk_idx++) {
      auto chunk = abi->GetChunkUnchecked(page_idx, page_layout, chunk_idx);
      ASSERT_TRUE(chunk.is_valid());
    }
  }

  // Finally return just two pages marking all their chunks as complete, and
  // check that the notification callback is posted.

  auto on_callback = task_runner_->CreateCheckpoint("on_callback");
  on_pages_complete_ =
      [on_callback](const std::vector<uint32_t>& completed_pages) {
        ASSERT_EQ(2u, completed_pages.size());
        ASSERT_EQ(0u, completed_pages[0]);
        ASSERT_EQ(3u, completed_pages[1]);
        on_callback();
      };
  for (size_t i = 0; i < 14; i++) {
    arbiter_->ReturnCompletedChunk(std::move(chunks[14 * i]));
    arbiter_->ReturnCompletedChunk(std::move(chunks[14 * i + 3]));
  }
  task_runner_->RunUntilCheckpoint("on_callback");
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

  // A further call should fail as we exhausted writer IDs.
  ASSERT_EQ(nullptr, arbiter_->CreateTraceWriter(0).get());
}

// TODO(primiano): add multi-threaded tests.

}  // namespace
}  // namespace perfetto
