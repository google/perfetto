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

// Tests TraceWriterV2Impl and ProducerRingBufferEndpoint together.
// - A mock endpoint plays the service. Its DrainRingBuffer() reads the ring
//   buffer into a TraceBufferV2, as the service does.
// - Packets are read back from TBv2 and parsed as ordinary TracePackets.

#include "src/tracing/v2/trace_writer_v2_impl.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/field.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/in_process_shared_memory.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/test/mock_producer_endpoint.h"
#include "src/tracing/v2/producer_ring_buffer_endpoint.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {

namespace test {

// Reaches the merge flag of a ProducerRingBufferEndpoint.
class ProducerRingBufferEndpointTestPeer {
 public:
  // Acts like a writer that set the merge flag in PostDrainTask() but did
  // not post its drain task yet.
  static void SetDrainTaskPending(ProducerRingBufferEndpoint* endpoint) {
    endpoint->drain_task_pending_.store(true);
  }
};

}  // namespace test

namespace {

using ::testing::_;
using ::testing::NiceMock;

constexpr uint32_t kChunkSize = 256;
constexpr BufferID kTargetBuffer = 7;
constexpr ProducerID kProducerId = 1;
constexpr size_t kSmbPageSize = 4096;
constexpr size_t kSmbSize = 4 * kSmbPageSize;

// A packet read back from the trace buffer.
struct ReadPacket {
  protos::gen::TracePacket packet;
  // Nonzero if data was lost before this packet on its sequence.
  uint32_t previous_dropped = 0;
};

class TraceWriterV2ImplTest : public ::testing::Test,
                              public SharedRingBufferReader::Delegate {
 protected:
  void SetUp() override {
    ON_CALL(endpoint_, CommitData(_, _))
        .WillByDefault([](const CommitDataRequest&,
                          ProducerEndpoint::CommitDataCallback callback) {
          if (callback)
            callback();
        });
    ON_CALL(endpoint_, DrainRingBuffer()).WillByDefault([this] {
      ++num_drain_requests_;
      Drain();
    });
    smb_arbiter_ = SharedMemoryArbiter::CreateInstance(
        &smb_, kSmbPageSize, SharedMemoryABI::ShmemMode::kDefault, &endpoint_,
        &task_runner_);
  }

  void TearDown() override {
    // The reader view borrows the mapping that the ring buffer endpoint owns.
    reader_.reset();
    reader_ring_buffer_.reset();
    ring_buffer_endpoint_.reset();
  }

  static std::unique_ptr<SharedMemory> CreateRingBufferMemory(
      uint32_t num_chunks) {
    return std::make_unique<InProcessSharedMemory>(sizeof(RingBufferHeader) +
                                                   num_chunks * kChunkSize);
  }

  // Creates the ring buffer endpoint with a ring buffer of |num_chunks|
  // chunks. It starts in kPending.
  void CreateRingBufferEndpoint(uint32_t num_chunks) {
    auto memory = CreateRingBufferMemory(num_chunks);
    // As the service does, the reader uses its own view of the mapping.
    reader_ring_buffer_ = std::make_unique<SharedRingBuffer>(
        static_cast<uint8_t*>(memory->start()), memory->size(), kChunkSize);
    ring_buffer_endpoint_ = ProducerRingBufferEndpoint::Create(
        &task_runner_, &endpoint_, smb_arbiter_.get(), std::move(memory),
        kChunkSize);
    ASSERT_TRUE(ring_buffer_endpoint_);
    reader_ = std::make_unique<SharedRingBufferReader>(
        reader_ring_buffer_.get(), this);
  }

  void CreateRingBufferEndpointWithReader(uint32_t num_chunks) {
    CreateRingBufferEndpoint(num_chunks);
    ring_buffer_endpoint_->OnReaderAttached();
    task_runner_.RunUntilIdle();
    num_drain_requests_ = 0;
  }

  std::unique_ptr<TraceWriter> CreateWriter(
      BufferExhaustedPolicy policy = BufferExhaustedPolicy::kDrop) {
    return ring_buffer_endpoint_->CreateTraceWriter(kTargetBuffer, policy);
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
  void OnChunkRead(
      const SharedRingBufferReader::ChunkContents& chunk) override {
    std::vector<protozero::ConstBytes> fragments;
    for (uint32_t i = 0; i < chunk.num_fragments; ++i)
      fragments.push_back({chunk.fragments[i].data, chunk.fragments[i].size});
    const TraceBuffer::PacketSequenceProperties sequence{
        kProducerId, ClientIdentity(/*uid=*/0, /*pid=*/0), chunk.writer_id};
    EXPECT_EQ(chunk.target_buffer, kTargetBuffer);
    trace_buffer_->AppendProtoGroupFragments(
        sequence, fragments.data(), fragments.size(),
        chunk.payload_flags & kFlagContinuesFromPrevChunk,
        chunk.payload_flags & kFlagContinuesOnNextChunk);
  }

  void OnDataLoss(WriterID writer_id) override {
    trace_buffer_->RecordProtoGroupLoss(kProducerId, writer_id);
  }

  base::TestTaskRunner task_runner_;
  NiceMock<MockProducerEndpoint> endpoint_;
  InProcessSharedMemory smb_{kSmbSize};
  std::unique_ptr<SharedMemoryArbiter> smb_arbiter_;
  std::unique_ptr<TraceBufferV2> trace_buffer_ =
      TraceBufferV2::Create(64 * 1024);
  std::unique_ptr<ProducerRingBufferEndpoint> ring_buffer_endpoint_;
  std::unique_ptr<SharedRingBuffer> reader_ring_buffer_;
  std::unique_ptr<SharedRingBufferReader> reader_;

  uint32_t num_drain_requests_ = 0;
};

// --- Packet encoding ---

TEST_F(TraceWriterV2ImplTest, PacketsRoundTripThroughTraceBuffer) {
  CreateRingBufferEndpointWithReader(/*num_chunks=*/8);
  // 600 bytes need three 256-byte chunks, so the nested message crosses chunk
  // boundaries. A length-delimited message would need a patch here, which
  // TraceWriterV2Impl does not support. So this also checks the proto group
  // encoding.
  const std::string large(600, 'x');
  {
    auto writer = CreateWriter();
    WritePacket(writer.get(), "small");
    WritePacket(writer.get(), large);
    EXPECT_EQ(writer->drop_count(), 0u);
  }
  task_runner_.RunUntilIdle();

  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "small");
  EXPECT_TRUE(packets[0].packet.first_packet_on_sequence());
  EXPECT_EQ(packets[1].packet.for_testing().str(), large);
  EXPECT_FALSE(packets[1].packet.first_packet_on_sequence());
  EXPECT_EQ(packets[1].previous_dropped, 0u);
}

// --- Buffer exhaustion ---

TEST_F(TraceWriterV2ImplTest, DropWhenFullThenReportLoss) {
  CreateRingBufferEndpointWithReader(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kDrop);

  // Nothing drains until the tasks run. Each 200-byte packet needs its own
  // chunk, so the fifth packet finds the ring buffer full.
  const std::string payload(200, 'd');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  ASSERT_GT(writer->drop_count(), 0u);
  task_runner_.RunUntilIdle();
  ReadPackets();

  // The first chunk after the loss carries kFlagDataLoss. The reader discards
  // all of its fragments, so this packet is lost too.
  WritePacket(writer.get(), "in flagged chunk");
  writer->Flush();
  WritePacket(writer.get(), "recovered");
  writer.reset();
  task_runner_.RunUntilIdle();

  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "recovered");
  EXPECT_NE(packets[0].previous_dropped, 0u);
}

TEST_F(TraceWriterV2ImplTest, StalledWriterDrainsOnEndpointThread) {
  CreateRingBufferEndpointWithReader(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);

  // No task runs here. The writer runs on the endpoint thread, so each wait
  // for space sends the drain request at once, through
  // NotifyReader(kWriterStalled).
  const std::string payload(200, 's');
  for (int i = 0; i < 12; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_EQ(writer->drop_count(), 0u);
  EXPECT_GT(num_drain_requests_, 0u);
  writer.reset();
  task_runner_.RunUntilIdle();

  EXPECT_EQ(ReadPackets().size(), 12u);
}

TEST_F(TraceWriterV2ImplTest, WriterDoesNotWaitBeforeReaderAttached) {
  CreateRingBufferEndpoint(/*num_chunks=*/4);
  auto writer = CreateWriter(BufferExhaustedPolicy::kStall);
  const std::string payload(200, 'o');
  for (int i = 0; i < 8 && writer->drop_count() == 0; ++i)
    WritePacket(writer.get(), payload);
  EXPECT_GT(writer->drop_count(), 0u);
  EXPECT_EQ(num_drain_requests_, 0u);
}

// --- Writer creation and WriterID lifetime ---

TEST_F(TraceWriterV2ImplTest, NullTraceWriterAfterDisconnect) {
  CreateRingBufferEndpoint(/*num_chunks=*/4);
  ring_buffer_endpoint_->Disconnect();
  auto disconnected_writer = CreateWriter();
  ASSERT_TRUE(disconnected_writer);
  EXPECT_EQ(disconnected_writer->writer_id(), 0u);
  WritePacket(disconnected_writer.get(), "discarded");
}

TEST_F(TraceWriterV2ImplTest, NoWriterIdAfterSmbArbiterShutdown) {
  CreateRingBufferEndpoint(/*num_chunks=*/4);
  ASSERT_TRUE(smb_arbiter_->TryShutdown());
  auto writer = CreateWriter();
  ASSERT_TRUE(writer);
  EXPECT_EQ(writer->writer_id(), 0u);
}

TEST_F(TraceWriterV2ImplTest, DestructionPublishesThenReleasesWriterId) {
  CreateRingBufferEndpoint(/*num_chunks=*/4);
  auto writer = CreateWriter();
  EXPECT_NE(writer->writer_id(), 0u);
  WritePacket(writer.get(), "last");

  // The live WriterID keeps the SMB arbiter, and so the endpoint, alive.
  EXPECT_FALSE(smb_arbiter_->TryShutdown());
  writer.reset();
  EXPECT_TRUE(smb_arbiter_->TryShutdown());

  // The destructor published the packet before it released the ID.
  ring_buffer_endpoint_->OnReaderAttached();
  task_runner_.RunUntilIdle();
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "last");
}

// --- Reader notifications and flush ---

TEST_F(TraceWriterV2ImplTest, NotificationsAreCoalesced) {
  CreateRingBufferEndpointWithReader(/*num_chunks=*/8);
  auto writer = CreateWriter();
  for (int i = 0; i < 5; ++i)
    WritePacket(writer.get(), "p");
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
}

TEST_F(TraceWriterV2ImplTest, FlushRunsCallbackAfterDrainRequest) {
  CreateRingBufferEndpointWithReader(/*num_chunks=*/4);
  auto writer = CreateWriter();
  WritePacket(writer.get(), "flushed");

  // No ack: the callback does not wait for CommitData.
  EXPECT_CALL(endpoint_, CommitData(_, _)).Times(0);
  std::vector<ReadPacket> packets_at_callback;
  bool flushed = false;
  writer->Flush([&] {
    flushed = true;
    packets_at_callback = ReadPackets();
  });
  EXPECT_FALSE(flushed);
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
  // The test endpoint drains when it gets the request. So the data is in the
  // trace buffer when the callback runs.
  ASSERT_EQ(packets_at_callback.size(), 1u);
  EXPECT_EQ(packets_at_callback[0].packet.for_testing().str(), "flushed");
}

// A writer set the merge flag but did not post its drain task yet. The flush
// must still send a drain request before its callback runs.
TEST_F(TraceWriterV2ImplTest, FlushDrainsWhileAnotherDrainIsNotPosted) {
  CreateRingBufferEndpointWithReader(/*num_chunks=*/4);
  test::ProducerRingBufferEndpointTestPeer::SetDrainTaskPending(
      ring_buffer_endpoint_.get());
  auto writer = CreateWriter();
  WritePacket(writer.get(), "flushed");

  std::vector<ReadPacket> packets_at_callback;
  writer->Flush([&] { packets_at_callback = ReadPackets(); });
  task_runner_.RunUntilIdle();
  ASSERT_EQ(packets_at_callback.size(), 1u);
  EXPECT_EQ(packets_at_callback[0].packet.for_testing().str(), "flushed");
}

TEST_F(TraceWriterV2ImplTest, FlushBeforeReaderAttachedRunsWithoutDrain) {
  CreateRingBufferEndpoint(/*num_chunks=*/4);
  auto writer = CreateWriter();
  WritePacket(writer.get(), "p");
  bool flushed = false;
  writer->Flush([&] { flushed = true; });
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
  EXPECT_EQ(num_drain_requests_, 0u);

  // The reader drains the data when it attaches.
  ring_buffer_endpoint_->OnReaderAttached();
  task_runner_.RunUntilIdle();
  EXPECT_EQ(num_drain_requests_, 1u);
  EXPECT_EQ(ReadPackets().size(), 1u);
}

TEST_F(TraceWriterV2ImplTest, FlushAfterDisconnectRunsWithoutDrain) {
  CreateRingBufferEndpoint(/*num_chunks=*/4);
  auto writer = CreateWriter();
  ring_buffer_endpoint_->Disconnect();
  bool flushed = false;
  writer->Flush([&] { flushed = true; });
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
  EXPECT_EQ(num_drain_requests_, 0u);
}

TEST_F(TraceWriterV2ImplTest, FlushRunsIfEndpointIsDestroyedFirst) {
  CreateRingBufferEndpoint(/*num_chunks=*/4);
  bool flushed = false;
  ring_buffer_endpoint_->Flush([&] { flushed = true; });
  reader_.reset();
  reader_ring_buffer_.reset();
  ring_buffer_endpoint_.reset();
  task_runner_.RunUntilIdle();
  EXPECT_TRUE(flushed);
}

// --- Creation ---

TEST_F(TraceWriterV2ImplTest, CreateRejectsInvalidLayout) {
  auto create = [&](std::unique_ptr<SharedMemory> memory, uint32_t chunk_size) {
    return ProducerRingBufferEndpoint::Create(&task_runner_, &endpoint_,
                                              smb_arbiter_.get(),
                                              std::move(memory), chunk_size);
  };
  EXPECT_FALSE(create(nullptr, kChunkSize));
  // 4096 bytes minus the header is not a whole number of chunks.
  EXPECT_FALSE(
      create(std::make_unique<InProcessSharedMemory>(4096), kChunkSize));
  // Three chunks is not a power of two.
  EXPECT_FALSE(create(CreateRingBufferMemory(3), kChunkSize));
  // Chunk size below the minimum.
  EXPECT_FALSE(create(CreateRingBufferMemory(4), 8));
  EXPECT_TRUE(create(CreateRingBufferMemory(4), kChunkSize));
}

}  // namespace
}  // namespace perfetto::tracing_v2
