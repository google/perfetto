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

// Tests TraceWriterV2Impl: packet encoding, buffer exhaustion and the order of
// destruction. The arbiter has its own tests in
// producer_ring_buffer_arbiter_unittest.cc.

#include "src/tracing/v2/trace_writer_v2_impl.h"

#include <string>

#include "src/tracing/v2/producer_ring_buffer_test_fixture.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

using TraceWriterV2ImplTest = ProducerRingBufferTest;

// --- Packet encoding ---

TEST_F(TraceWriterV2ImplTest, PacketsRoundTripThroughTraceBuffer) {
  SetupV2InstanceWithReader(/*num_chunks=*/8);
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
  SetupV2InstanceWithReader(/*num_chunks=*/4);
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

// --- Destruction ---

TEST_F(TraceWriterV2ImplTest, DestructionPublishesThenReleasesWriterId) {
  SetupV2Instance(/*num_chunks=*/4);
  auto writer = CreateWriter();
  EXPECT_NE(writer->writer_id(), 0u);
  WritePacket(writer.get(), "last");

  // The live WriterID keeps the SMB arbiter, and so the ring buffer arbiter,
  // alive.
  EXPECT_FALSE(smb_arbiter_->TryShutdown());
  writer.reset();
  EXPECT_TRUE(smb_arbiter_->TryShutdown());

  // The destructor published the packet before it released the ID. The
  // service drains once when it attaches the ring buffer.
  AcceptAttach();
  Drain();
  const auto packets = ReadPackets();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].packet.for_testing().str(), "last");
}

}  // namespace
}  // namespace perfetto::tracing_v2
