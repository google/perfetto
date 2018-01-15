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
#include "perfetto/tracing/core/trace_writer.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"
#include "src/tracing/test/aligned_buffer_test.h"

#include "protos/trace_packet.pbzero.h"

namespace perfetto {
namespace {

class TraceWriterImplTest : public AlignedBufferTest {
 public:
  void SetUp() override {
    SharedMemoryArbiterImpl::set_default_layout_for_testing(
        SharedMemoryABI::PageLayout::kPageDiv4);
    AlignedBufferTest::SetUp();
    auto callback = [](const std::vector<uint32_t>& arg) {};
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
                        TraceWriterImplTest,
                        ::testing::ValuesIn(kPageSizes));

TEST_P(TraceWriterImplTest, SingleWriter) {
  const BufferID tgt_buf_id = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(tgt_buf_id);
  for (int i = 0; i < 32; i++) {
    auto packet = writer->NewTracePacket();
    char str[16];
    sprintf(str, "foobar %d", i);
    packet->set_test(str);
  }
  writer->NewTracePacket()->set_test("workaround for returing the last chunk");
  writer.reset();

  SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
  size_t packets_count = 0;
  for (size_t page_idx = 0; page_idx < kNumPages; page_idx++) {
    uint32_t page_layout = abi->page_layout_dbg(page_idx);
    size_t num_chunks = SharedMemoryABI::GetNumChunksForLayout(page_layout);
    for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
      auto chunk =
          abi->TryAcquireChunkForReading(page_idx, chunk_idx, tgt_buf_id);
      if (!chunk.is_valid())
        continue;
      packets_count += chunk.header()->packets_state.load().count;
    }
  }
  EXPECT_EQ(32u, packets_count);
  // TODO(primiano): check also the content of the packets decoding the protos.
}

// TODO(primiano): add multi-writer test.

}  // namespace
}  // namespace perfetto
