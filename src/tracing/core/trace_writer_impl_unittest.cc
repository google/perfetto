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

#include "src/tracing/core/trace_writer_impl.h"

#include "gtest/gtest.h"
#include "perfetto/base/utils.h"
#include "perfetto/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/tracing_service.h"
#include "src/base/test/gtest_test_suite.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"
#include "src/tracing/test/aligned_buffer_test.h"
#include "src/tracing/test/fake_producer_endpoint.h"

#include "perfetto/trace/test_event.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

class TraceWriterImplTest : public AlignedBufferTest {
 public:
  void SetUp() override {
    SharedMemoryArbiterImpl::set_default_layout_for_testing(
        SharedMemoryABI::PageLayout::kPageDiv4);
    AlignedBufferTest::SetUp();
    task_runner_.reset(new base::TestTaskRunner());
    arbiter_.reset(new SharedMemoryArbiterImpl(buf(), buf_size(), page_size(),
                                               &fake_producer_endpoint_,
                                               task_runner_.get()));
  }

  void TearDown() override {
    arbiter_.reset();
    task_runner_.reset();
  }

  FakeProducerEndpoint fake_producer_endpoint_;
  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<SharedMemoryArbiterImpl> arbiter_;
  std::function<void(const std::vector<uint32_t>&)> on_pages_complete_;
};

size_t const kPageSizes[] = {4096, 65536};
INSTANTIATE_TEST_SUITE_P(PageSize,
                         TraceWriterImplTest,
                         ::testing::ValuesIn(kPageSizes));

TEST_P(TraceWriterImplTest, SingleWriter) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);
  const size_t kNumPackets = 32;
  for (size_t i = 0; i < kNumPackets; i++) {
    auto packet = writer->NewTracePacket();
    char str[16];
    sprintf(str, "foobar %zu", i);
    packet->set_for_testing()->set_str(str);
  }

  // Destroying the TraceWriteImpl should cause the last packet to be finalized
  // and the chunk to be put back in the kChunkComplete state.
  writer.reset();

  SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
  size_t packets_count = 0;
  for (size_t page_idx = 0; page_idx < kNumPages; page_idx++) {
    uint32_t page_layout = abi->GetPageLayout(page_idx);
    size_t num_chunks = SharedMemoryABI::GetNumChunksForLayout(page_layout);
    for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
      auto chunk_state = abi->GetChunkState(page_idx, chunk_idx);
      ASSERT_TRUE(chunk_state == SharedMemoryABI::kChunkFree ||
                  chunk_state == SharedMemoryABI::kChunkComplete);
      auto chunk = abi->TryAcquireChunkForReading(page_idx, chunk_idx);
      if (!chunk.is_valid())
        continue;
      packets_count += chunk.header()->packets.load().count;
    }
  }
  EXPECT_EQ(kNumPackets, packets_count);
  // TODO(primiano): check also the content of the packets decoding the protos.
}

TEST_P(TraceWriterImplTest, FragmentingPacket) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  // Write a packet that's guaranteed to span more than a single chunk.
  auto packet = writer->NewTracePacket();
  size_t chunk_size = page_size() / 4;
  std::stringstream large_string_writer;
  for (size_t pos = 0; pos < chunk_size; pos++)
    large_string_writer << "x";
  std::string large_string = large_string_writer.str();
  packet->set_for_testing()->set_str(large_string.data(), large_string.size());

  // First chunk should be committed.
  arbiter_->FlushPendingCommitDataRequests();
  const auto& last_commit = fake_producer_endpoint_.last_commit_data_request;
  ASSERT_EQ(1, last_commit.chunks_to_move_size());
  EXPECT_EQ(0u, last_commit.chunks_to_move()[0].page());
  EXPECT_EQ(0u, last_commit.chunks_to_move()[0].chunk());
  EXPECT_EQ(kBufId, last_commit.chunks_to_move()[0].target_buffer());
  EXPECT_EQ(0, last_commit.chunks_to_patch_size());

  SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();

  // The first allocated chunk should be complete but need patching, since the
  // packet extended past the chunk and no patches for the packet size or string
  // field size were applied yet.
  ASSERT_EQ(SharedMemoryABI::kChunkComplete, abi->GetChunkState(0u, 0u));
  auto chunk = abi->TryAcquireChunkForReading(0u, 0u);
  ASSERT_TRUE(chunk.is_valid());
  ASSERT_EQ(1, chunk.header()->packets.load().count);
  ASSERT_TRUE(chunk.header()->packets.load().flags &
              SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
  ASSERT_TRUE(chunk.header()->packets.load().flags &
              SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);

  // Starting a new packet should cause patches to be applied.
  packet->Finalize();
  auto packet2 = writer->NewTracePacket();
  arbiter_->FlushPendingCommitDataRequests();
  EXPECT_EQ(0, last_commit.chunks_to_move_size());
  ASSERT_EQ(1, last_commit.chunks_to_patch_size());
  EXPECT_EQ(writer->writer_id(), last_commit.chunks_to_patch()[0].writer_id());
  EXPECT_EQ(kBufId, last_commit.chunks_to_patch()[0].target_buffer());
  EXPECT_EQ(chunk.header()->chunk_id.load(),
            last_commit.chunks_to_patch()[0].chunk_id());
  EXPECT_FALSE(last_commit.chunks_to_patch()[0].has_more_patches());
  ASSERT_EQ(1, last_commit.chunks_to_patch()[0].patches_size());
}

// TODO(primiano): add multi-writer test.
// TODO(primiano): add Flush() test.

}  // namespace
}  // namespace perfetto
