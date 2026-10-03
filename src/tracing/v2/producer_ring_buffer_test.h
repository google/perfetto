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

#ifndef SRC_TRACING_V2_PRODUCER_RING_BUFFER_TEST_H_
#define SRC_TRACING_V2_PRODUCER_RING_BUFFER_TEST_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/protozero/field.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/test/mock_producer_endpoint.h"
#include "src/tracing/v2/producer_ring_buffer_arbiter.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/data_source_config.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {

// Base fixture for the tests of the tracing v2 producer side. It runs a real
// producer against a fake service, on one thread.
//
// Used by:
// - ProducerRingBufferArbiterTest, in producer_ring_buffer_arbiter_unittest.cc.
// - TraceWriterV2ImplTest, in trace_writer_v2_impl_unittest.cc.
//
// The producer side is the code under test:
// - CreateRingBufferArbiter() sets up a v2 instance on |arbiter_|, a
//   ProducerRingBufferArbiter, so the arbiter creates a new ring buffer.
// - CreateWriter() creates a TraceWriterV2Impl from that arbiter.
//
// The fake service side reads back what the writers published:
// - |endpoint_| is a mock ProducerEndpoint. Its DrainV2RingBuffer() calls
//   Drain(), but only after AttachReader(), as the service does after it
//   accepts the ring buffer.
// - Its AttachV2RingBuffer() keeps the mapping and the reply, and
//   AttachReader() or RejectAttach() sends the reply later.
// - Drain() reads the ring buffer with a SharedRingBufferReader, through its
//   own view of the mapping. This fixture is that reader's delegate.
//   OnChunkRead() and OnDataLoss() copy the chunks into |trace_buffer_|, as
//   the service does.
// - ReadPackets() reads the packets back from |trace_buffer_|.
//
// Posted tasks, such as drain requests, run only when a test calls
// task_runner_.RunUntilIdle(). So each test controls when the service reads.
class ProducerRingBufferTest : public ::testing::Test,
                               public SharedRingBufferReader::Delegate {
 protected:
  // The arbiter uses this chunk size if the config has no chunk size option.
  static constexpr uint32_t kChunkSize = kMinChunkSize;
  static constexpr BufferID kTargetBuffer = 7;
  static constexpr DataSourceInstanceID kInstance = 1;
  static constexpr ProducerID kProducerId = 1;
  static constexpr size_t kSmbPageSize = 4096;
  static constexpr size_t kSmbSize = 4 * kSmbPageSize;

  // A packet read back from the trace buffer.
  struct ReadPacket {
    protos::gen::TracePacket packet;
    // Nonzero if data was lost before this packet on its sequence.
    uint32_t previous_dropped = 0;
  };

  void SetUp() override {
    using ::testing::_;
    ON_CALL(endpoint_, CommitData(_, _))
        .WillByDefault([](const CommitDataRequest&,
                          ProducerEndpoint::CommitDataCallback callback) {
          if (callback)
            callback();
        });
    ON_CALL(endpoint_, AttachV2RingBuffer(_, _, _))
        .WillByDefault([this](const std::shared_ptr<SharedMemory>& memory,
                              uint32_t chunk_size,
                              std::function<void(bool)> reply) {
          ++num_attach_requests_;
          // As the service does, the reader uses its own view of the mapping.
          service_memory_ = memory;
          reader_ring_buffer_ = std::make_unique<SharedRingBuffer>(
              static_cast<uint8_t*>(service_memory_->start()),
              service_memory_->size(), chunk_size);
          reader_ = std::make_unique<SharedRingBufferReader>(
              reader_ring_buffer_.get(), this);
          attach_reply_ = std::move(reply);
        });
    // Like the service, drain only after the reader is attached.
    ON_CALL(endpoint_, DrainV2RingBuffer()).WillByDefault([this] {
      ++num_drain_requests_;
      if (service_reader_attached_)
        Drain();
    });
    smb_arbiter_ = SharedMemoryArbiter::CreateInstance(
        &smb_, kSmbPageSize, SharedMemoryABI::ShmemMode::kDefault, &endpoint_,
        &task_runner_);
    ON_CALL(endpoint_, MaybeSharedMemoryArbiter())
        .WillByDefault(::testing::Return(smb_arbiter_.get()));
    arbiter_ = std::make_unique<ProducerRingBufferArbiter>(
        &task_runner_, &endpoint_,
        [this](size_t size) -> std::shared_ptr<SharedMemory> {
          ++num_allocations_;
          if (fail_allocation_)
            return nullptr;
          return std::make_shared<InProcessSharedMemory>(size);
        });
  }

  // The budget for a ring buffer of |num_chunks| chunks, header not included.
  static size_t BudgetFor(uint32_t num_chunks) {
    return num_chunks * kChunkSize;
  }

  // A config that selects v2 with |probability_percent|.
  static DataSourceConfig V2Config(uint32_t probability_percent = 100) {
    DataSourceConfig config;
    config.set_supports_tracing_v2(true);
    config.mutable_experimental_tracing_v2()->set_use_v2_probability_percent(
        probability_percent);
    return config;
  }

  void SetupInstance(DataSourceInstanceID id,
                     const DataSourceConfig& config,
                     uint32_t num_chunks = 4) {
    arbiter_->SetupInstance(id, config, protocol_abi_versions_,
                            BudgetFor(num_chunks));
  }

  // Gives |arbiter_| a ring buffer of |num_chunks| chunks: sets up
  // |kInstance| with v2.
  // The ring buffer starts in kPending.
  void CreateRingBufferArbiter(uint32_t num_chunks) {
    SetupInstance(kInstance, V2Config(), num_chunks);
    ASSERT_TRUE(attach_reply_);
  }

  void CreateRingBufferArbiterWithReader(uint32_t num_chunks) {
    CreateRingBufferArbiter(num_chunks);
    AttachReader();
  }

  // Acts like the service accepting the ring buffer.
  void AttachReader() {
    ASSERT_TRUE(attach_reply_);
    service_reader_attached_ = true;
    std::exchange(attach_reply_, nullptr)(true);
  }

  // Acts like the service rejecting the ring buffer.
  void RejectAttach() {
    ASSERT_TRUE(attach_reply_);
    std::exchange(attach_reply_, nullptr)(false);
  }

  std::unique_ptr<TraceWriter> CreateWriter(
      BufferExhaustedPolicy policy = BufferExhaustedPolicy::kDrop,
      DataSourceInstanceID id = kInstance) {
    return arbiter_->MaybeCreateTraceWriter(kTargetBuffer, policy, id);
  }

  static void WritePacket(TraceWriter* writer, const std::string& str) {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str(str);
  }

  // Reads all published chunks into the trace buffer, as the service does.
  void Drain() {
    for (;;) {
      const auto result = reader_->Drain(/*max_positions=*/64);
      if (result.positions_consumed == 0)
        return;
    }
  }

  // Returns the test packets in the trace buffer. Reads consume them.
  std::vector<ReadPacket> ReadPackets() {
    std::vector<ReadPacket> result;
    trace_buffer_->BeginRead();
    for (;;) {
      TracePacket packet;
      TraceBuffer::PacketSequenceProperties sequence{};
      uint32_t previous_dropped = 0;
      if (!trace_buffer_->ReadNextTracePacket(&packet, &sequence,
                                              &previous_dropped)) {
        return result;
      }
      ReadPacket read;
      EXPECT_TRUE(read.packet.ParseFromString(packet.GetRawBytesForTesting()));
      read.previous_dropped = previous_dropped;
      if (read.packet.has_for_testing())
        result.push_back(std::move(read));
    }
  }

  // SharedRingBufferReader::Delegate:
  // The fake service's reader calls these during Drain().
  void OnChunkRead(
      const SharedRingBufferReader::ChunkContents& chunk) override {
    std::vector<protozero::ConstBytes> fragments;
    for (uint32_t i = 0; i < chunk.num_fragments; ++i)
      fragments.push_back({chunk.fragments[i].data, chunk.fragments[i].size});
    const TraceBuffer::PacketSequenceProperties sequence{
        kProducerId, ClientIdentity(/*uid=*/0, /*pid=*/0), chunk.writer_id};
    EXPECT_EQ(chunk.target_buffer, kTargetBuffer);
    trace_buffer_->CopyChunkV2Untrusted(
        sequence, fragments.data(), fragments.size(),
        chunk.payload_flags & kFlagContinuesFromPrevChunk,
        chunk.payload_flags & kFlagContinuesOnNextChunk);
  }

  void OnDataLoss(WriterID writer_id) override {
    trace_buffer_->RecordChunkV2DataLoss(kProducerId, writer_id);
  }

  base::TestTaskRunner task_runner_;
  uint32_t protocol_abi_versions_ = kProtocolAbiV1 | kProtocolAbiV2;
  ::testing::NiceMock<MockProducerEndpoint> endpoint_;
  InProcessSharedMemory smb_{kSmbSize};
  std::unique_ptr<SharedMemoryArbiter> smb_arbiter_;
  std::unique_ptr<ProducerRingBufferArbiter> arbiter_;

  // The fake service side.
  std::unique_ptr<TraceBufferV2> trace_buffer_ =
      TraceBufferV2::Create(64 * 1024);
  std::shared_ptr<SharedMemory> service_memory_;
  std::unique_ptr<SharedRingBuffer> reader_ring_buffer_;
  std::unique_ptr<SharedRingBufferReader> reader_;
  std::function<void(bool)> attach_reply_;
  bool service_reader_attached_ = false;

  bool fail_allocation_ = false;
  uint32_t num_allocations_ = 0;
  uint32_t num_attach_requests_ = 0;
  uint32_t num_drain_requests_ = 0;
};

}  // namespace perfetto::tracing_v2

#endif  // SRC_TRACING_V2_PRODUCER_RING_BUFFER_TEST_H_
