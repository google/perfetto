/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/ext/tracing/core/startup_trace_writer.h"

#include "perfetto/ext/tracing/core/startup_trace_writer_registry.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "src/base/test/gtest_test_suite.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/patch_list.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"
#include "src/tracing/core/trace_buffer.h"
#include "src/tracing/test/aligned_buffer_test.h"
#include "src/tracing/test/fake_producer_endpoint.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

class StartupTraceWriterTest : public AlignedBufferTest {
 public:
  void SetUp() override {
    SharedMemoryArbiterImpl::set_default_layout_for_testing(
        SharedMemoryABI::PageLayout::kPageDiv1);
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

  std::unique_ptr<StartupTraceWriter> CreateUnboundWriter() {
    std::shared_ptr<StartupTraceWriterRegistryHandle> registry;
    return std::unique_ptr<StartupTraceWriter>(new StartupTraceWriter(
        registry, BufferExhaustedPolicy::kDrop, max_buffer_size_bytes()));
  }

  bool BindWriter(StartupTraceWriter* writer, size_t chunks_per_batch = 0) {
    const BufferID kBufId = 42;
    return writer->BindToArbiter(arbiter_.get(), kBufId, chunks_per_batch);
  }

  void WritePackets(StartupTraceWriter* writer, size_t packet_count) {
    for (size_t i = 0; i < packet_count; i++) {
      auto packet = writer->NewTracePacket();
      packet->set_for_testing()->set_str(kPacketPayload);
    }
  }

  size_t CountCompleteChunksInSMB() {
    SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
    size_t num_complete_chunks = 0;
    for (size_t page_idx = 0; page_idx < kNumPages; page_idx++) {
      uint32_t page_layout = abi->GetPageLayout(page_idx);
      size_t num_chunks = SharedMemoryABI::GetNumChunksForLayout(page_layout);
      for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
        auto chunk_state = abi->GetChunkState(page_idx, chunk_idx);
        if (chunk_state == SharedMemoryABI::kChunkComplete)
          num_complete_chunks++;
      }
    }
    return num_complete_chunks;
  }

  void VerifyPackets(size_t expected_count,
                     bool expect_data_loss_packet = false) {
    SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
    auto buffer = TraceBuffer::Create(abi->size());

    if (expect_data_loss_packet)
      expected_count++;

    size_t total_packets_count = 0;
    ChunkID current_max_chunk_id = 0;
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

        // Should only see new chunks with IDs larger than the previous read
        // since our reads and writes are serialized.
        ChunkID chunk_id = chunk.header()->chunk_id.load();
        if (last_read_max_chunk_id_ != 0) {
          EXPECT_LT(last_read_max_chunk_id_, chunk_id);
        }
        current_max_chunk_id = std::max(current_max_chunk_id, chunk_id);

        auto packets_header = chunk.header()->packets.load();
        total_packets_count += packets_header.count;
        if (packets_header.flags &
            SharedMemoryABI::ChunkHeader::kFirstPacketContinuesFromPrevChunk) {
          // Don't count fragmented packets twice.
          total_packets_count--;
        }

        buffer->CopyChunkUntrusted(
            /*producer_id_trusted=*/1, /*producer_uid_trusted=*/1,
            chunk.header()->writer_id.load(), chunk_id, packets_header.count,
            packets_header.flags, /*chunk_complete=*/true,
            chunk.payload_begin(), chunk.payload_size());
        abi->ReleaseChunkAsFree(std::move(chunk));
      }
    }
    last_read_max_chunk_id_ = current_max_chunk_id;
    EXPECT_EQ(expected_count, total_packets_count);

    // Now verify chunk and packet contents.
    buffer->BeginRead();
    size_t num_packets_read = 0;
    while (true) {
      TracePacket packet;
      TraceBuffer::PacketSequenceProperties sequence_properties{};
      bool previous_packet_dropped;
      if (!buffer->ReadNextTracePacket(&packet, &sequence_properties,
                                       &previous_packet_dropped)) {
        break;
      }
      EXPECT_EQ(static_cast<uid_t>(1),
                sequence_properties.producer_uid_trusted);

      protos::gen::TracePacket parsed_packet;
      bool res = parsed_packet.ParseFromString(packet.GetRawBytesForTesting());
      EXPECT_TRUE(res);
      if (!res)
        break;

      // If the buffer size was exceeded, the data loss packet should be the
      // last committed packet.
      if (expect_data_loss_packet && parsed_packet.previous_packet_dropped()) {
        num_packets_read++;
        EXPECT_EQ(expected_count, num_packets_read);
        break;
      }

      EXPECT_TRUE(parsed_packet.has_for_testing());
      EXPECT_EQ(kPacketPayload, parsed_packet.for_testing().str());
      num_packets_read++;
    }
    EXPECT_EQ(expected_count, num_packets_read);
  }

  size_t GetUnboundWriterCount(
      const StartupTraceWriterRegistry& registry) const {
    return registry.unbound_writers_.size() +
           registry.unbound_owned_writers_.size();
  }

  size_t GetBindingRegistriesCount(
      const SharedMemoryArbiterImpl& arbiter) const {
    return arbiter.startup_trace_writer_registries_.size();
  }

  size_t GetUnboundWriterCount(const SharedMemoryArbiterImpl& arbiter) const {
    size_t count = 0u;
    for (const auto& reg : arbiter.startup_trace_writer_registries_) {
      count += reg->unbound_writers_.size();
      count += reg->unbound_owned_writers_.size();
    }
    return count;
  }

  size_t GetTotalBufferSize(StartupTraceWriter* writer) const {
    return writer->memory_buffer_->GetTotalSize();
  }

  size_t max_buffer_size_bytes() const { return page_size() * 5; }

 protected:
  static constexpr char kPacketPayload[] = "foo";

  FakeProducerEndpoint fake_producer_endpoint_;
  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<SharedMemoryArbiterImpl> arbiter_;
  std::function<void(const std::vector<uint32_t>&)> on_pages_complete_;

  ChunkID last_read_max_chunk_id_ = 0;
};

constexpr char StartupTraceWriterTest::kPacketPayload[];

namespace {

size_t const kPageSizes[] = {4096, 32768};
INSTANTIATE_TEST_SUITE_P(PageSize,
                         StartupTraceWriterTest,
                         ::testing::ValuesIn(kPageSizes));

TEST_P(StartupTraceWriterTest, CreateUnboundAndBind) {
  auto writer = CreateUnboundWriter();

  // Bind writer right away without having written any data before.
  EXPECT_TRUE(BindWriter(writer.get()));

  const size_t kNumPackets = 32;
  WritePackets(writer.get(), kNumPackets);
  // Finalizes the last packet and returns the chunk.
  writer.reset();

  VerifyPackets(kNumPackets);
}

TEST_P(StartupTraceWriterTest, CreateBound) {
  // Create a bound writer immediately.
  const BufferID kBufId = 42;
  std::unique_ptr<StartupTraceWriter> writer(
      new StartupTraceWriter(arbiter_->CreateTraceWriter(kBufId)));

  const size_t kNumPackets = 32;
  WritePackets(writer.get(), kNumPackets);
  // Finalizes the last packet and returns the chunk.
  writer.reset();

  VerifyPackets(kNumPackets);
}

TEST_P(StartupTraceWriterTest, WriteWhileUnboundAndDiscard) {
  auto writer = CreateUnboundWriter();

  const size_t kNumPackets = 32;
  WritePackets(writer.get(), kNumPackets);

  // Should discard the written data.
  writer.reset();

  VerifyPackets(0);
}

TEST_P(StartupTraceWriterTest, WriteWhileUnboundAndBind) {
  auto writer = CreateUnboundWriter();

  const size_t kNumPackets = 32;
  WritePackets(writer.get(), kNumPackets);

  // Binding the writer should cause the previously written packets to be
  // written to the SMB and committed.
  EXPECT_TRUE(BindWriter(writer.get()));

  VerifyPackets(kNumPackets);

  // Any further packets should be written to the SMB directly.
  const size_t kNumAdditionalPackets = 16;
  WritePackets(writer.get(), kNumAdditionalPackets);
  // Finalizes the last packet and returns the chunk.
  writer.reset();

  VerifyPackets(kNumAdditionalPackets);
}

TEST_P(StartupTraceWriterTest, WriteMultipleChunksWhileUnboundAndBind) {
  auto writer = CreateUnboundWriter();

  // Write a single packet to determine its size in the buffer.
  WritePackets(writer.get(), 1);
  size_t packet_size = writer->used_buffer_size();

  // Write at least 3 pages worth of packets.
  const size_t kNumPackets = (page_size() * 3 + packet_size - 1) / packet_size;
  WritePackets(writer.get(), kNumPackets);

  // Binding the writer should cause the previously written packets to be
  // written to the SMB and committed.
  EXPECT_TRUE(BindWriter(writer.get()));

  VerifyPackets(kNumPackets + 1);

  // Any further packets should be written to the SMB directly.
  const size_t kNumAdditionalPackets = 16;
  WritePackets(writer.get(), kNumAdditionalPackets);
  // Finalizes the last packet and returns the chunk.
  writer.reset();

  VerifyPackets(kNumAdditionalPackets);
}

TEST_P(StartupTraceWriterTest, MaxBufferSizeExceeded) {
  auto writer = CreateUnboundWriter();

  // Write packets until the buffer is extended above kMaxBufferSizeBytes.
  size_t valid_packets = 0;
  while (GetTotalBufferSize(writer.get()) < max_buffer_size_bytes()) {
    WritePackets(writer.get(), 1);
    valid_packets++;
  }

  // The next packet should be dropped into a void.
  WritePackets(writer.get(), 1);

  // Binding the writer should cause the valid previously written packets to be
  // written to the SMB and committed.
  EXPECT_TRUE(BindWriter(writer.get()));

  VerifyPackets(valid_packets, /*expect_data_loss_packet=*/true);

  // Any further packets should be written to the SMB directly.
  const size_t kNumAdditionalPackets = 16;
  WritePackets(writer.get(), kNumAdditionalPackets);
  // Finalizes the last packet and returns the chunk.
  writer.reset();

  VerifyPackets(kNumAdditionalPackets);
}

TEST_P(StartupTraceWriterTest, BindingWhileWritingFails) {
  auto writer = CreateUnboundWriter();

  {
    // Begin a write by opening a TracePacket.
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str(kPacketPayload);

    // Binding while writing should fail.
    EXPECT_FALSE(BindWriter(writer.get()));
  }

  // Packet was completed, so binding should work now and emit the packet.
  EXPECT_TRUE(BindWriter(writer.get()));
  VerifyPackets(1);
}

TEST_P(StartupTraceWriterTest, CreateAndBindViaRegistry) {
  std::unique_ptr<StartupTraceWriterRegistry> registry(
      new StartupTraceWriterRegistry());

  // Create unbound writers.
  auto writer1 =
      registry->CreateUnboundTraceWriter(BufferExhaustedPolicy::kDrop);
  auto writer2 =
      registry->CreateUnboundTraceWriter(BufferExhaustedPolicy::kDrop);

  EXPECT_EQ(2u, GetUnboundWriterCount(*registry));

  // Return |writer2|. It should be kept alive until the registry is bound.
  StartupTraceWriter::ReturnToRegistry(std::move(writer2));

  {
    // Begin a write by opening a TracePacket on |writer1|.
    auto packet = writer1->NewTracePacket();

    // Binding |writer1| writing should fail, but |writer2| should be bound.
    const BufferID kBufId = 42;
    arbiter_->BindStartupTraceWriterRegistry(std::move(registry), kBufId);
    EXPECT_EQ(1u, GetUnboundWriterCount(*arbiter_));
  }

  // Wait for |writer1| to be bound and the registry to be deleted.
  auto checkpoint_name = "all_bound";
  auto all_bound = task_runner_->CreateCheckpoint(checkpoint_name);
  std::function<void()> task;
  task = [&task, &all_bound, this]() {
    if (!GetBindingRegistriesCount(*arbiter_)) {
      all_bound();
      return;
    }
    task_runner_->PostDelayedTask(task, 1);
  };
  task_runner_->PostDelayedTask(task, 1);
  task_runner_->RunUntilCheckpoint(checkpoint_name);

  StartupTraceWriter::ReturnToRegistry(std::move(writer1));
}

TEST_P(StartupTraceWriterTest, BindAndCommitInBatches) {
  auto writer = CreateUnboundWriter();

  // Write a single packet to determine its size in the buffer.
  WritePackets(writer.get(), 1);
  size_t packet_size = writer->used_buffer_size();

  // Write at least 3 pages/chunks worth of packets.
  const size_t kNumPackets = (page_size() * 3 + packet_size - 1) / packet_size;
  WritePackets(writer.get(), kNumPackets);

  static constexpr size_t kChunksPerBatch = 2;

  // Binding the writer with a batch size of 2 chunks should cause the first 2
  // chunks of previously written packets to be written to the SMB and
  // committed. The remaining chunks will be written when the
  // |commit_data_callback| is executed.
  EXPECT_TRUE(BindWriter(writer.get(), kChunksPerBatch));

  EXPECT_EQ(
      fake_producer_endpoint_.last_commit_data_request.chunks_to_move().size(),
      kChunksPerBatch);
  EXPECT_EQ(CountCompleteChunksInSMB(), kChunksPerBatch);
  auto commit_data_callback = fake_producer_endpoint_.last_commit_data_callback;
  EXPECT_TRUE(commit_data_callback);

  // Send a commit with a single packet from the bound trace writer before the
  // remaining chunk batches of the buffered data are written.
  const size_t kNumAdditionalPackets = 1;
  WritePackets(writer.get(), 1);
  // Finalizes the packet and returns the chunk.
  writer.reset();

  // The packet should fit into a chunk.
  EXPECT_EQ(
      fake_producer_endpoint_.last_commit_data_request.chunks_to_move().size(),
      1u);
  EXPECT_EQ(CountCompleteChunksInSMB(), kChunksPerBatch + 1);

  // Write and commit the remaining chunks to the SMB.
  while (commit_data_callback) {
    commit_data_callback();
    commit_data_callback = fake_producer_endpoint_.last_commit_data_callback;
  }

  // Verify that all chunks + packets are in the SMB.
  VerifyPackets(1 + kNumPackets + kNumAdditionalPackets);
}

TEST_P(StartupTraceWriterTest, BindAndCommitInBatchesWithSMBExhaustion) {
  auto writer = CreateUnboundWriter();

  // Write a single packet to determine its size in the buffer.
  WritePackets(writer.get(), 1);
  size_t packet_size = writer->used_buffer_size();

  // Write at least 3 pages/chunks worth of packets.
  const size_t kNumPackets = (page_size() * 3 + packet_size - 1) / packet_size;
  WritePackets(writer.get(), kNumPackets);

  // Acquire all chunks in the SMB.
  static constexpr size_t kTotChunks = kNumPages;
  SharedMemoryABI::Chunk chunks[kTotChunks];
  for (size_t i = 0; i < kTotChunks; i++) {
    chunks[i] = arbiter_->GetNewChunk({}, BufferExhaustedPolicy::kDrop);
    ASSERT_TRUE(chunks[i].is_valid());
  }

  // Binding the writer should fail if no chunks are available in the SMB.
  static constexpr size_t kChunksPerBatch = 2;
  EXPECT_FALSE(BindWriter(writer.get(), kChunksPerBatch));

  // Return and free the first chunk, so that there is only a single free chunk.
  PatchList ignored;
  arbiter_->ReturnCompletedChunk(std::move(chunks[0]), 42, &ignored);
  chunks[0] =
      arbiter_->shmem_abi_for_testing()->TryAcquireChunkForReading(0, 0);
  ASSERT_TRUE(chunks[0].is_valid());
  arbiter_->shmem_abi_for_testing()->ReleaseChunkAsFree(std::move(chunks[0]));
  arbiter_->FlushPendingCommitDataRequests();

  // Binding the writer should only cause the first chunks of previously written
  // packets to be written to the SMB and committed because no further chunks
  // are available in the SMB. The remaining chunks will be written when the
  // |commit_data_callback| is executed.
  EXPECT_TRUE(BindWriter(writer.get(), kChunksPerBatch));

  EXPECT_EQ(
      fake_producer_endpoint_.last_commit_data_request.chunks_to_move().size(),
      1u);
  EXPECT_EQ(CountCompleteChunksInSMB(), 1u);
  auto commit_data_callback = fake_producer_endpoint_.last_commit_data_callback;
  EXPECT_TRUE(commit_data_callback);

  // Free up the other SMB chunks.
  for (size_t i = 1; i < kTotChunks; i++) {
    arbiter_->ReturnCompletedChunk(std::move(chunks[i]), 42, &ignored);
    chunks[i] =
        arbiter_->shmem_abi_for_testing()->TryAcquireChunkForReading(i, 0);
    ASSERT_TRUE(chunks[i].is_valid());
    arbiter_->shmem_abi_for_testing()->ReleaseChunkAsFree(std::move(chunks[i]));
  }
  arbiter_->FlushPendingCommitDataRequests();

  // Write and commit the remaining buffered startup writer data to the SMB.
  while (commit_data_callback) {
    commit_data_callback();
    commit_data_callback = fake_producer_endpoint_.last_commit_data_callback;
  }
  EXPECT_GT(
      fake_producer_endpoint_.last_commit_data_request.chunks_to_move().size(),
      0u);

  // Verify that all chunks + packets are in the SMB.
  VerifyPackets(1 + kNumPackets);
}

}  // namespace
}  // namespace perfetto
