/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <string.h>

#include <initializer_list>
#include <random>
#include <sstream>
#include <vector>

#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/base/test/vm_test_utils.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/test/fake_packet.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {

using ::testing::ContainerEq;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

class TraceBufferV2Test : public testing::Test {
 public:
  static constexpr uint8_t kContFromPrevChunk =
      SharedMemoryABI::ChunkHeader::kFirstPacketContinuesFromPrevChunk;
  static constexpr uint8_t kContOnNextChunk =
      SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk;
  static constexpr uint8_t kChunkNeedsPatching =
      SharedMemoryABI::ChunkHeader::kChunkNeedsPatching;

  void TearDown() override {
    // Test that the used_size() logic works and that all the data after that
    // is zero-filled.
    if (trace_buffer_) {
      const size_t used_size = trace_buffer_->used_size();
      ASSERT_LE(used_size, trace_buffer_->size());
      trace_buffer()->data_.EnsureCommitted(trace_buffer_->size());
      bool zero_padded = true;
      for (size_t i = used_size; i < trace_buffer_->size(); ++i) {
        bool is_zero = static_cast<char*>(trace_buffer()->data_.Get())[i] == 0;
        zero_padded = zero_padded && is_zero;
      }
      ASSERT_TRUE(zero_padded);
    }
  }

  FakeChunk CreateChunk(ProducerID p, WriterID w, ChunkID c) {
    return FakeChunk(trace_buffer_.get(), p, w, c);
  }

  void ResetBuffer(
      size_t size_,
      TraceBuffer::OverwritePolicy policy = TraceBuffer::kOverwrite) {
    trace_buffer_ = TraceBufferV2::Create(size_, policy);
    ASSERT_TRUE(trace_buffer_);
  }

  bool TryPatchChunkContents(ProducerID p,
                             WriterID w,
                             ChunkID c,
                             std::vector<TraceBuffer::Patch> patches,
                             bool other_patches_pending = false) {
    return trace_buffer_->TryPatchChunkContents(
        p, w, c, patches.data(), patches.size(), other_patches_pending);
  }

  static std::vector<FakePacketFragment> ReadPacket(
      const std::unique_ptr<TraceBuffer>& buf,
      TraceBuffer::PacketSequenceProperties* sequence_properties = nullptr,
      bool* previous_packet_dropped = nullptr) {
    std::vector<FakePacketFragment> fragments;
    TracePacket packet;
    TraceBuffer::PacketSequenceProperties ignored_sequence_properties{};
    bool ignored_previous_packet_dropped;
    if (!buf->ReadNextTracePacket(
            &packet,
            sequence_properties ? sequence_properties
                                : &ignored_sequence_properties,
            previous_packet_dropped ? previous_packet_dropped
                                    : &ignored_previous_packet_dropped)) {
      return fragments;
    }
    for (const Slice& slice : packet.slices())
      fragments.emplace_back(slice.start, slice.size);
    return fragments;
  }

  std::vector<FakePacketFragment> ReadPacket(
      TraceBuffer::PacketSequenceProperties* sequence_properties = nullptr,
      bool* previous_packet_dropped = nullptr) {
    return ReadPacket(trace_buffer_, sequence_properties,
                      previous_packet_dropped);
  }

  void AppendChunks(
      std::initializer_list<std::tuple<ProducerID, WriterID, ChunkID>> chunks) {
    for (const auto& c : chunks) {
      char seed =
          static_cast<char>(std::get<0>(c) + std::get<1>(c) + std::get<2>(c));
      CreateChunk(std::get<0>(c), std::get<1>(c), std::get<2>(c))
          .AddPacket(4, seed)
          .CopyIntoTraceBuffer();
    }
  }

  void SuppressClientDchecksForTesting() {
    trace_buffer()->suppress_client_dchecks_for_testing_ = true;
  }

  uint8_t* GetBufData(const TraceBuffer& buf) {
    return static_cast<const TraceBufferV2&>(buf).begin();
  }

  size_t size_to_end() { return trace_buffer()->size_to_end(); }

  TraceBufferV2* trace_buffer() {
    return static_cast<TraceBufferV2*>(trace_buffer_.get());
  }

 protected:
  std::unique_ptr<TraceBuffer> trace_buffer_;
};

// ----------------------
// Main TraceBufferV2 tests
// ----------------------

// Note for the test code: remember that the resulting size of a chunk is:
// SUM(packets) + 16 (that is sizeof(ChunkRecord)).
// Also remember that chunks are rounded up to 16. So, unless we are testing the
// rounding logic, might be a good idea to create chunks of that size.

TEST_F(TraceBufferV2Test, ReadWrite_EmptyBuffer) {
  ResetBuffer(4096);
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// On each iteration writes a fixed-size chunk and reads it back.
TEST_F(TraceBufferV2Test, ReadWrite_Simple) {
  ResetBuffer(64 * 1024);
  for (ChunkID chunk_id = 0; chunk_id < 1000; chunk_id++) {
    char seed = static_cast<char>(chunk_id);
    CreateChunk(ProducerID(1), WriterID(1), chunk_id)
        .AddPacket(42, seed)
        .CopyIntoTraceBuffer();
    trace_buffer()->BeginRead();
    ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(42, seed)));
    ASSERT_THAT(ReadPacket(), IsEmpty());
    EXPECT_EQ(chunk_id + 1u, trace_buffer()->stats().chunks_written());
    EXPECT_EQ(trace_buffer()->stats().chunks_written(),
              trace_buffer()->stats().chunks_read());
    EXPECT_LT(0u, trace_buffer()->stats().bytes_written());
    EXPECT_EQ(trace_buffer()->stats().bytes_written(),
              trace_buffer()->stats().bytes_read());
    EXPECT_EQ(0u, trace_buffer()->stats().padding_bytes_written());
    EXPECT_EQ(0u, trace_buffer()->stats().padding_bytes_cleared());
  }
}

TEST_F(TraceBufferV2Test, ReadWrite_OneChunkPerWriter) {
  for (int8_t num_writers = 1; num_writers <= 10; num_writers++) {
    ResetBuffer(4096);
    for (char i = 1; i <= num_writers; i++) {
      ASSERT_EQ(32u, CreateChunk(ProducerID(i), WriterID(i), ChunkID(i))
                         .AddPacket(32 - 16, i)
                         .CopyIntoTraceBuffer());
    }

    // The expected read sequence now is: c3, c4, c5.
    trace_buffer()->BeginRead();
    for (char i = 1; i <= num_writers; i++)
      ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32 - 16, i)));
    ASSERT_THAT(ReadPacket(), IsEmpty());
  }  // for(num_writers)
}

// Writes chunk that up filling the buffer precisely until the end, like this:
// [ c0: 512 ][ c1: 512 ][ c2: 1024 ][ c3: 2048 ]
// | ---------------- 4k buffer --------------- |
TEST_F(TraceBufferV2Test, ReadWrite_FillTillEnd) {
  ResetBuffer(4096);
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(512u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(i * 4))
                        .AddPacket(512 - 16, 'a')
                        .CopyIntoTraceBuffer());
    ASSERT_EQ(512u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(i * 4 + 1))
                        .AddPacket(512 - 16, 'b')
                        .CopyIntoTraceBuffer());
    ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(i * 4 + 2))
                         .AddPacket(1024 - 16, 'c')
                         .CopyIntoTraceBuffer());
    ASSERT_EQ(2048u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(i * 4 + 3))
                         .AddPacket(2048 - 16, 'd')
                         .CopyIntoTraceBuffer());

    // At this point the write pointer should have been reset at the beginning.
    ASSERT_EQ(4096u, size_to_end());

    trace_buffer()->BeginRead();
    ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(512 - 16, 'a')));
    ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(512 - 16, 'b')));
    ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1024 - 16, 'c')));
    ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2048 - 16, 'd')));
    ASSERT_THAT(ReadPacket(), IsEmpty());
  }
}

// Similar to the above, but this time leaves some gap at the end and then
// tries to add a chunk that doesn't fit to exercise the padding-at-end logic.
// Initial condition:
// [ c0: 128 ][ c1: 256 ][ c2: 512   ][ c3: 1024 ][ c4: 2048 ]{ 128 padding }
// | ------------------------------- 4k buffer ------------------------------ |
//
// At this point we try to insert a 512 Bytes chunk (c5). The result should be:
// [ c5: 512              ]{ padding }[c3: 1024 ][ c4: 2048 ]{ 128 padding }
// | ------------------------------- 4k buffer ------------------------------ |
TEST_F(TraceBufferV2Test, ReadWrite_Padding) {
  ResetBuffer(4096);
  ASSERT_EQ(128u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
                      .AddPacket(128 - 16, 'a')
                      .CopyIntoTraceBuffer());
  ASSERT_EQ(256u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
                      .AddPacket(256 - 16, 'b')
                      .CopyIntoTraceBuffer());
  ASSERT_EQ(512u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
                      .AddPacket(512 - 16, 'c')
                      .CopyIntoTraceBuffer());
  ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
                       .AddPacket(1024 - 16, 'd')
                       .CopyIntoTraceBuffer());
  ASSERT_EQ(2048u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(4))
                       .AddPacket(2048 - 16, 'e')
                       .CopyIntoTraceBuffer());

  // Now write c5 that will cause wrapping + padding.
  ASSERT_EQ(128u, size_to_end());
  ASSERT_EQ(512u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(5))
                      .AddPacket(512 - 16, 'f')
                      .CopyIntoTraceBuffer());
  ASSERT_EQ(4096u - 512, size_to_end());

  // The expected read sequence now is: c3, c4, c5.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1024 - 16, 'd')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2048 - 16, 'e')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(512 - 16, 'f')));
  ASSERT_THAT(ReadPacket(), IsEmpty());

  EXPECT_EQ(6u, trace_buffer()->stats().chunks_written());
  EXPECT_EQ(3u, trace_buffer()->stats().chunks_overwritten());
  EXPECT_EQ(3u, trace_buffer()->stats().chunks_read());
  EXPECT_EQ(4480u, trace_buffer()->stats().bytes_written());
  EXPECT_EQ(896u, trace_buffer()->stats().bytes_overwritten());
  EXPECT_EQ(3584u, trace_buffer()->stats().bytes_read());
  EXPECT_EQ(384u, trace_buffer()->stats().padding_bytes_written());
  EXPECT_EQ(0u, trace_buffer()->stats().padding_bytes_cleared());

  // Adding another chunk should clear some of the padding.
  ASSERT_EQ(128u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(6))
                      .AddPacket(128 - 16, 'g')
                      .CopyIntoTraceBuffer());
  EXPECT_EQ(384u, trace_buffer()->stats().padding_bytes_cleared());
}

// Like ReadWrite_Padding, but this time the padding introduced is the minimum
// allowed (16 bytes). This is to exercise edge cases in the padding logic.
// [c0: 2048               ][c1: 1024         ][c2: 1008       ][c3: 16]
// [c4: 2032            ][c5: 1040                ][c6 :16][c7: 1080   ]
TEST_F(TraceBufferV2Test, ReadWrite_MinimalPadding) {
  ResetBuffer(4096);

  ASSERT_EQ(2048u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
                       .AddPacket(2048 - 16, 'a')
                       .CopyIntoTraceBuffer());
  ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
                       .AddPacket(1024 - 16, 'b')
                       .CopyIntoTraceBuffer());
  ASSERT_EQ(1008u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
                       .AddPacket(1008 - 16, 'c')
                       .CopyIntoTraceBuffer());
  ASSERT_EQ(16u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
                     .CopyIntoTraceBuffer());

  ASSERT_EQ(4096u, size_to_end());

  ASSERT_EQ(2032u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(4))
                       .AddPacket(2032 - 16, 'd')
                       .CopyIntoTraceBuffer());
  ASSERT_EQ(1040u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(5))
                       .AddPacket(1040 - 16, 'e')
                       .CopyIntoTraceBuffer());
  ASSERT_EQ(16u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(6))
                     .CopyIntoTraceBuffer());
  ASSERT_EQ(1008u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(7))
                       .AddPacket(1008 - 16, 'f')
                       .CopyIntoTraceBuffer());

  ASSERT_EQ(4096u, size_to_end());

  // The expected read sequence now is: c3, c4, c5.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2032 - 16, 'd')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1040 - 16, 'e')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1008 - 16, 'f')));
  for (int i = 0; i < 3; i++)
    ASSERT_THAT(ReadPacket(), IsEmpty());
}

// NOTE: I had to change this test from V1 because it was assuming readback in
// producer,writer order, while instead now expects buffer order.
TEST_F(TraceBufferV2Test, ReadWrite_RandomChunksNoWrapping) {
  for (unsigned int seed = 1; seed <= 32; seed++) {
    std::minstd_rand0 rnd_engine(seed);
    ResetBuffer(4096 * (1 + rnd_engine() % 32));
    std::uniform_int_distribution<size_t> size_dist(18, 4096);
    std::uniform_int_distribution<ProducerID> prod_dist(1, kMaxProducerID);
    std::uniform_int_distribution<WriterID> wri_dist(1, kMaxWriterID);
    ChunkID chunk_id = 0;
    std::vector<std::tuple<ProducerID, WriterID, ChunkID, size_t>> expected;
    for (;;) {
      const size_t chunk_size = size_dist(rnd_engine);
      if (base::AlignUp<16>(chunk_size) >= size_to_end())
        break;
      ProducerID p = prod_dist(rnd_engine);
      WriterID w = wri_dist(rnd_engine);
      ChunkID c = chunk_id++;
      expected.emplace_back(std::make_tuple(p, w, c, chunk_size));
      ASSERT_EQ(chunk_size,
                CreateChunk(p, w, c)
                    .AddPacket(chunk_size - 16, static_cast<char>(chunk_size))
                    .CopyIntoTraceBuffer());
    }  // for(;;)
    trace_buffer()->BeginRead();
    for (const auto& it : expected) {
      const size_t chunk_size = std::get<3>(it);
      ASSERT_THAT(ReadPacket(),
                  ElementsAre(FakePacketFragment(
                      chunk_size - 16, static_cast<char>(chunk_size))));
    }
    ASSERT_THAT(ReadPacket(), IsEmpty());
  }
}

// Tests the case of writing a chunk that leaves just sizeof(ChunkRecord) at
// the end of the buffer.
TEST_F(TraceBufferV2Test, ReadWrite_WrappingCases) {
  ResetBuffer(4096);
  ASSERT_EQ(4080u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
                       .AddPacket(4080 - 16, 'a')
                       .CopyIntoTraceBuffer());
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4080 - 16, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());

  ASSERT_EQ(16u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
                     .CopyIntoTraceBuffer());
  ASSERT_EQ(2048u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
                       .AddPacket(2048 - 16, 'b')
                       .CopyIntoTraceBuffer());

  ASSERT_EQ(2048u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
                       .AddPacket(2048 - 16, 'c')
                       .CopyIntoTraceBuffer());
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2048 - 16, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2048 - 16, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Verify that empty packets are skipped.
TEST_F(TraceBufferV2Test, ReadWrite_EmptyPacket) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), 0)
      .AddPacket(42, 1)
      .AddPacket(1, 2)
      .AddPacket(42, 3)
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(42, 1)));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(42, 3)));
  ASSERT_THAT(ReadPacket(), IsEmpty());

  EXPECT_EQ(0u, trace_buffer()->stats().abi_violations());
}

// --------------------------------------
// Fragments stitching and skipping logic
// --------------------------------------

TEST_F(TraceBufferV2Test, Fragments_Simple) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a', kContFromPrevChunk)
      .AddPacket(20, 'b')
      .AddPacket(30, 'c')
      .AddPacket(10, 'd', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(20, 'e', kContFromPrevChunk)
      .AddPacket(30, 'f')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  // The (10, 'a') entry should be skipped because we don't have provided the
  // previous chunk, hence should be treated as a data loss.
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'c')));

  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'd'),
                                        FakePacketFragment(20, 'e')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'f')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Fragments_EdgeCases) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(2, 'a', kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(2, 'b', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Now add the missing fragment.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(2, 'c', kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2, 'b'),
                                        FakePacketFragment(2, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// The following tests verify that chunks received out-of-order are read in the
// correct order.
//
// Fragment order {0,2,1} for sequence {1,1}, without fragmenting packets.
TEST_F(TraceBufferV2Test, Fragments_OutOfOrderLastChunkIsMiddle) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(30, 'c')
      .CopyIntoTraceBuffer();
  EXPECT_EQ(0u, trace_buffer()->stats().chunks_committed_out_of_order());
  trace_buffer()->BeginRead();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(20, 'b')
      .CopyIntoTraceBuffer();
  EXPECT_EQ(1u, trace_buffer()->stats().chunks_committed_out_of_order());

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Fragment order {0,2,1} for sequence {1,1}, with fragmenting packets.
TEST_F(TraceBufferV2Test, Fragments_OutOfOrderLastChunkIsMiddleFragmentation) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(30, 'c', kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(20, 'b', kContFromPrevChunk | kContOnNextChunk)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a'),
                                        FakePacketFragment(20, 'b'),
                                        FakePacketFragment(30, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Fragment order {0,2,1,3} for sequence {1,1}, with fragmenting packets. Also
// verifies that another sequence isn't broken.
TEST_F(TraceBufferV2Test, Fragments_OutOfOrderLastChunkIsMaxFragmentation) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(30, 'c', kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(20, 'b', kContFromPrevChunk | kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
      .AddPacket(40, 'd')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a'),
                                        FakePacketFragment(20, 'b'),
                                        FakePacketFragment(30, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Fragment order {-2,1,-1,0} for sequence {1,1}, without fragmenting packets.
TEST_F(TraceBufferV2Test, Fragments_OutOfOrderWithIdOverflowADCB) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(kMaxChunkID - 1))
      .AddPacket(10, 'a')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(40, 'd')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a')));
  // ASSERT_THAT(ReadPacket(), IsEmpty());

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(30, 'c')
      .CopyIntoTraceBuffer();
  // trace_buffer()->BeginRead();
  // ASSERT_THAT(ReadPacket(), IsEmpty());

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(kMaxChunkID))
      .AddPacket(20, 'b')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Fragment order {-2,0,-1,1} for sequence {1,1}, without fragmenting packets.
TEST_F(TraceBufferV2Test, Fragments_OutOfOrderWithIdOverflowACBD) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(kMaxChunkID - 1))
      .AddPacket(10, 'a')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(30, 'c')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a')));
  // ASSERT_THAT(ReadPacket(), IsEmpty());

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(kMaxChunkID))
      .AddPacket(20, 'b')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'c')));
  // ASSERT_THAT(ReadPacket(), IsEmpty());

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(40, 'd')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Fragments_EmptyChunkBefore) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0)).CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(10, 'a')
      .AddPacket(20, 'b', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(30, 'c', kContFromPrevChunk)
      .AddPacket(40, 'd', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'b'),
                                        FakePacketFragment(30, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Fragments_EmptyChunkAfter) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .AddPacket(10, 'b', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1)).CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Set up a fragmented packet that happens to also have an empty chunk in the
// middle of the sequence. Test that it just gets skipped.
TEST_F(TraceBufferV2Test, Fragments_EmptyChunkInTheMiddle) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1)).CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(10, 'b', kContFromPrevChunk)
      .AddPacket(20, 'c')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a'),
                                        FakePacketFragment(10, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Generates sequences of fragmented packets of increasing length (|seq_len|),
// from [P0, P1a][P1y] to [P0, P1a][P1b][P1c]...[P1y]. Test that they are always
// read as one packet.
TEST_F(TraceBufferV2Test, Fragments_LongPackets) {
  for (unsigned seq_len = 1; seq_len <= 10; seq_len++) {
    ResetBuffer(4096);
    std::vector<FakePacketFragment> expected_fragments;
    expected_fragments.emplace_back(20, 'b');
    CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
        .AddPacket(10, 'a')
        .AddPacket(20, 'b', kContOnNextChunk)
        .CopyIntoTraceBuffer();
    for (unsigned i = 1; i <= seq_len; i++) {
      char prefix = 'b' + static_cast<char>(i);
      expected_fragments.emplace_back(20 + i, prefix);
      CreateChunk(ProducerID(1), WriterID(1), ChunkID(i))
          .AddPacket(20 + i, prefix, kContFromPrevChunk | kContOnNextChunk)
          .CopyIntoTraceBuffer();
    }
    expected_fragments.emplace_back(30, 'y');
    CreateChunk(ProducerID(1), WriterID(1), ChunkID(seq_len + 1))
        .AddPacket(30, 'y', kContFromPrevChunk)
        .AddPacket(50, 'z')
        .CopyIntoTraceBuffer();

    trace_buffer()->BeginRead();
    ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a')));
    ASSERT_THAT(ReadPacket(), ContainerEq(expected_fragments));
    ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'z')));
    ASSERT_THAT(ReadPacket(), IsEmpty());
  }
}

// Similar to Fragments_LongPacket, but covers also the case of ChunkID wrapping
// over its max value.
TEST_F(TraceBufferV2Test, Fragments_LongPacketWithWrappingID) {
  ResetBuffer(4096);
  std::vector<FakePacketFragment> expected_fragments;

  for (ChunkID chunk_id = static_cast<ChunkID>(-2); chunk_id <= 2; chunk_id++) {
    char prefix = static_cast<char>('c' + chunk_id);
    expected_fragments.emplace_back(10 + chunk_id, prefix);
    CreateChunk(ProducerID(1), WriterID(1), chunk_id)
        .AddPacket(10 + chunk_id, prefix, kContOnNextChunk)
        .CopyIntoTraceBuffer();
  }
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ContainerEq(expected_fragments));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Change from TraceBufferV1: here I had to swap the order of expected packets
// because now we respect buffer order rather than going by {producer,writer}.
TEST_F(TraceBufferV2Test, Fragments_PreserveUID) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .AddPacket(10, 'b', kContOnNextChunk)
      .SetUID(11)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(0))
      .AddPacket(10, 'c')
      .AddPacket(10, 'd')
      .SetUID(22)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(10, 'e', kContFromPrevChunk)
      .AddPacket(10, 'f')
      .SetUID(11)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  TraceBuffer::PacketSequenceProperties sequence_properties;
  ASSERT_THAT(ReadPacket(&sequence_properties),
              ElementsAre(FakePacketFragment(10, 'a')));
  ASSERT_EQ(static_cast<uid_t>(11), sequence_properties.producer_uid_trusted());

  ASSERT_THAT(
      ReadPacket(&sequence_properties),
      ElementsAre(FakePacketFragment(10, 'b'), FakePacketFragment(10, 'e')));
  ASSERT_EQ(static_cast<uid_t>(11), sequence_properties.producer_uid_trusted());

  ASSERT_THAT(ReadPacket(&sequence_properties),
              ElementsAre(FakePacketFragment(10, 'c')));
  ASSERT_EQ(static_cast<uid_t>(22), sequence_properties.producer_uid_trusted());

  ASSERT_THAT(ReadPacket(&sequence_properties),
              ElementsAre(FakePacketFragment(10, 'd')));
  ASSERT_EQ(static_cast<uid_t>(22), sequence_properties.producer_uid_trusted());

  ASSERT_THAT(ReadPacket(&sequence_properties),
              ElementsAre(FakePacketFragment(10, 'f')));
  ASSERT_EQ(static_cast<uid_t>(11), sequence_properties.producer_uid_trusted());

  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Fragments_DiscardedOnPacketSizeDropPacket) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  // Set up a fragmented packet in the first chunk, which continues in the
  // second chunk with kPacketSizeDropPacket size. The corrupted fragmented
  // packet should be skipped.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .AddPacket(10, 'b', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .SetFlags(kContFromPrevChunk)
      // Var-int encoded TraceWriterImpl::kPacketSizeDropPacket.
      .AddPacket({0xff, 0xff, 0xff, 0x7f})
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(10, 'd')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Fragments_IncompleteChunkNeedsPatching) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b', kContOnNextChunk | kChunkNeedsPatching)
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer()->BeginRead();
  // First packet should be read even if the chunk's last packet still needs
  // patching.
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// --------------------------
// Out of band patching tests
// --------------------------

TEST_F(TraceBufferV2Test, Patching_Simple) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(100, 'a')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(0))
      .AddPacket(9, 'b')
      .ClearBytes(5, 4)  // 5 := 4th payload byte. Byte 0 is the varint header.
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(3), WriterID(1), ChunkID(0))
      .AddPacket(100, 'c')
      .CopyIntoTraceBuffer();
  ASSERT_TRUE(TryPatchChunkContents(ProducerID(2), WriterID(1), ChunkID(0),
                                    {{5, {{'Y', 'M', 'C', 'A'}}}}));
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(100, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment("b00-YMCA", 8)));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(100, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Patching_SkipIfChunkDoesntExist) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(100, 'a')
      .CopyIntoTraceBuffer();
  ASSERT_FALSE(TryPatchChunkContents(ProducerID(1), WriterID(2), ChunkID(0),
                                     {{0, {{'X', 'X', 'X', 'X'}}}}));
  ASSERT_FALSE(TryPatchChunkContents(ProducerID(1), WriterID(1), ChunkID(1),
                                     {{0, {{'X', 'X', 'X', 'X'}}}}));
  ASSERT_FALSE(TryPatchChunkContents(ProducerID(1), WriterID(1), ChunkID(-1),
                                     {{0, {{'X', 'X', 'X', 'X'}}}}));
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(100, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Patching_AtBoundariesOfChunk) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(100, 'a', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(16, 'b', kContFromPrevChunk | kContOnNextChunk)
      .ClearBytes(1, 4)
      .ClearBytes(16 - 4, 4)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(100, 'c', kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  ASSERT_TRUE(TryPatchChunkContents(
      ProducerID(1), WriterID(1), ChunkID(1),
      {{1, {{'P', 'E', 'R', 'F'}}}, {16 - 4, {{'E', 'T', 'T', 'O'}}}}));
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(),
              ElementsAre(FakePacketFragment(100, 'a'),
                          FakePacketFragment("PERFb01-b02ETTO", 15),
                          FakePacketFragment(100, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Tests kChunkNeedsPatching logic: chunks that are marked as "pending patch"
// should not be read until the patch has happened.
TEST_F(TraceBufferV2Test, Patching_ReadWaitsForPatchComplete) {
  ResetBuffer(4096);

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(16, 'a', kChunkNeedsPatching | kContOnNextChunk)
      .ClearBytes(1, 4)  // 1 := 0th payload byte. Byte 0 is the varint header.
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(16, 'b', kContFromPrevChunk)
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(2), WriterID(1), ChunkID(0))
      .AddPacket(16, 'c')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(1))
      .AddPacket(16, 'd', kChunkNeedsPatching | kContOnNextChunk)
      .ClearBytes(1, 4)  // 1 := 0th payload byte. Byte 0 is the varint header.
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(2))
      .AddPacket(16, 'e', kContFromPrevChunk)
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(3), WriterID(1), ChunkID(0))
      .AddPacket(16, 'f', kChunkNeedsPatching | kContOnNextChunk)
      .ClearBytes(1, 8)  // 1 := 0th payload byte. Byte 0 is the varint header.
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(3), WriterID(1), ChunkID(1))
      .AddPacket(1, '\0', kContFromPrevChunk)
      .CopyIntoTraceBuffer();

  // The only thing that can be read right now is the 1st packet of the 2nd
  // sequence. All the rest is blocked waiting for patching.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(16, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Now patch the 2nd sequence and check that the sequence is unblocked.
  ASSERT_TRUE(TryPatchChunkContents(ProducerID(2), WriterID(1), ChunkID(1),
                                    {{1, {{'P', 'A', 'T', 'C'}}}}));
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(),
              ElementsAre(FakePacketFragment("PATCd01-d02-d03", 15),
                          FakePacketFragment(16, 'e')));
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Now patch the 3rd sequence, but in the first patch set
  // |other_patches_pending| to true, so that the sequence is unblocked only
  // after the 2nd patch.
  ASSERT_TRUE(TryPatchChunkContents(ProducerID(3), WriterID(1), ChunkID(0),
                                    {{1, {{'P', 'E', 'R', 'F'}}}},
                                    /*other_patches_pending=*/true));
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());

  ASSERT_TRUE(TryPatchChunkContents(ProducerID(3), WriterID(1), ChunkID(0),
                                    {{5, {{'E', 'T', 'T', 'O'}}}},
                                    /*other_patches_pending=*/false));
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(),
              ElementsAre(FakePacketFragment("PERFETTOf02-f03", 15)));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Tests that if we have pending patches and those chunks get overwritten,
// we still detect data loss properly.
TEST_F(TraceBufferV2Test, PendingPatchesDataLossOnOverwrite) {
  ResetBuffer(4096);

  // Create a fragmented packet that needs patching
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(1024, 'a', kContOnNextChunk | kChunkNeedsPatching)
      .CopyIntoTraceBuffer();

  // Create the continuation chunk
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(1024, 'b', kContFromPrevChunk)
      .CopyIntoTraceBuffer();

  // Verify the chunk is waiting for patches (can't be read)
  trace_buffer()->BeginRead();
  // Should be empty because chunk needs patching
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Now write large chunks to cause buffer wrap and overwrite the pending
  // chunks
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(2000, 'c')
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
      .AddPacket(2000, 'd')
      .CopyIntoTraceBuffer();

  // The pending chunks should have been overwritten. When we read the next
  // chunk in the sequence, we should see a data loss because chunks 0-1
  // (which were pending patches) were overwritten before being completed.
  bool previous_packet_dropped = false;
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(2000, 'c')));
  EXPECT_TRUE(previous_packet_dropped);  // Data loss should be detected

  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(2000, 'd')));
  EXPECT_FALSE(previous_packet_dropped);  // No data loss for this packet

  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// ---------------------
// Malicious input tests
// ---------------------

TEST_F(TraceBufferV2Test, Malicious_ZeroSizedChunk) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(32, 'a')
      .CopyIntoTraceBuffer();

  uint8_t valid_ptr = 0;
  trace_buffer()->CopyChunkUntrusted(
      ProducerID(1), ClientIdentity(uid_t(0), pid_t(0)), WriterID(1),
      ChunkID(1), 1 /* num packets */, 0 /* flags */, true /* chunk_complete */,
      &valid_ptr, sizeof(valid_ptr));

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(32, 'b')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32, 'b')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Attempting to write a chunk bigger than ChunkRecord::kMaxSize should end up
// in a no-op.
TEST_F(TraceBufferV2Test, Malicious_ChunkTooBig) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(4096, 'a')
      .AddPacket(2048, 'b')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Malicious_DeclareMorePacketsBeyondBoundaries) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(64, 'a')
      .IncrementNumPackets()
      .IncrementNumPackets()
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(2), ChunkID(0))
      .IncrementNumPackets()
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(3), ChunkID(0))
      .AddPacket(32, 'b')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(64, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32, 'b')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Malicious_ZeroVarintHeader) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  // Create a standalone chunk where the varint header is == 0.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(4, 'a')
      .ClearBytes(0, 1)
      .AddPacket(4, 'b')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(0))
      .AddPacket(4, 'c')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Forge a chunk where the first packet is valid but the second packet has a
// varint header that continues beyond the end of the chunk (and also beyond the
// end of the buffer).
TEST_F(TraceBufferV2Test, Malicious_OverflowingVarintHeader) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(4079, 'a')  // 4079 := 4096 - sizeof(ChunkRecord) - 1
      .AddPacket({0x82})  // 0x8*: that the varint continues on the next byte.
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4079, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Malicious_VarintHeaderTooBig) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();

  // Add a valid chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(32, 'a')
      .CopyIntoTraceBuffer();

  // Forge a packet which has a varint header that is just off by one.
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(0))
      .AddPacket({0x16, '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b',
                  'c', 'd', 'e', 'f'})
      .CopyIntoTraceBuffer();

  // Forge a packet which has a varint header that tries to hit an overflow.
  CreateChunk(ProducerID(3), WriterID(1), ChunkID(0))
      .AddPacket({0xff, 0xff, 0xff, 0x7f})
      .CopyIntoTraceBuffer();

  // Forge a packet which has a jumbo varint header: 0xff, 0xff .. 0x7f.
  std::vector<uint8_t> chunk;
  chunk.insert(chunk.end(), 128 - sizeof(internal::TBChunk), 0xff);
  chunk.back() = 0x7f;
  trace_buffer()->CopyChunkUntrusted(
      ProducerID(4), ClientIdentity(uid_t(0), pid_t(0)), WriterID(1),
      ChunkID(1), 1 /* num packets */, 0 /* flags*/, true /* chunk_complete*/,
      chunk.data(), chunk.size());

  // Add a valid chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(32, 'b')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32, 'b')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Similar to Malicious_VarintHeaderTooBig, but this time the full chunk
// contains an enormous varint number that tries to overflow.
TEST_F(TraceBufferV2Test, Malicious_JumboVarint) {
  ResetBuffer(64 * 1024);
  SuppressClientDchecksForTesting();

  std::vector<uint8_t> chunk;
  chunk.insert(chunk.end(), 64 * 1024 - sizeof(internal::TBChunk) * 2, 0xff);
  chunk.back() = 0x7f;
  for (int i = 0; i < 3; i++) {
    trace_buffer()->CopyChunkUntrusted(
        ProducerID(1), ClientIdentity(uid_t(0), pid_t(0)), WriterID(1),
        ChunkID(1), 1 /* num packets */, 0 /* flags */,
        true /* chunk_complete */, chunk.data(), chunk.size());
  }

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Like the Malicious_ZeroVarintHeader, but put the chunk in the middle of a
// sequence that would be otherwise valid. The zero-sized fragment should be
// skipped.
TEST_F(TraceBufferV2Test, Malicious_ZeroVarintHeaderInSequence) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(4, 'a', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(4, 'b', kContFromPrevChunk | kContOnNextChunk)
      .ClearBytes(0, 1)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(4, 'c', kContFromPrevChunk)
      .AddPacket(4, 'd')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
      .AddPacket(4, 'e')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(3))
      .AddPacket(5, 'f')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'a'),
                                        FakePacketFragment(4, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'd')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'e')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(5, 'f')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Similar to Malicious_ZeroVarintHeaderInSequence, but this time the zero-sized
// fragment is the last fragment for a chunk and is marked for continuation. The
// zero-sized fragment should be skipped.
TEST_F(TraceBufferV2Test, Malicious_ZeroVarintHeaderAtEndOfChunk) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(4, 'a')
      .AddPacket(4, 'b', kContOnNextChunk)
      .ClearBytes(4, 4)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(4, 'c', kContFromPrevChunk)
      .AddPacket(4, 'd')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(4, 'e')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(3))
      .AddPacket(4, 'f')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'd')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'e')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4, 'f')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Malicious_PatchOutOfBounds) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(2048, 'a')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(16, 'b')
      .CopyIntoTraceBuffer();
  size_t offsets[] = {13,          16,          size_t(-4),
                      size_t(-8),  size_t(-12), size_t(-16),
                      size_t(-20), size_t(-32), size_t(-1024)};
  for (size_t offset : offsets) {
    ASSERT_FALSE(TryPatchChunkContents(ProducerID(1), WriterID(1), ChunkID(1),
                                       {{offset, {{'0', 'd', 'a', 'y'}}}}));
  }
}

TEST_F(TraceBufferV2Test, Malicious_OverrideWithShorterChunkSize) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(2048, 'a')
      .CopyIntoTraceBuffer();
  // The service should ignore this override of the chunk since the chunk size
  // is different.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(1024, 'b')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2048, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Malicious_OverrideWithShorterChunkSizeAfterRead) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(30, 'a')
      .AddPacket(40, 'b')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'b')));

  // The service should ignore this override of the chunk since the chunk size
  // is different.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .AddPacket(10, 'b')
      .AddPacket(10, 'c')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Test that the service didn't get stuck in some indeterminate state.
  // Writing a valid chunk with a larger ID should make things work again.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(10, 'd')
      .AddPacket(10, 'e')
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'd')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'e')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Malicious_OverrideWithDifferentOffsetAfterRead) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(30, 'a')
      .AddPacket(40, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'b')));

  // The attacker in this case speculates on the fact that the read pointer is
  // @ 70 which is >> the size of the new chunk we overwrite.
  // The service will not discard this override since the chunk size is correct.
  // However, it should detect that the packet headers at the current read
  // offset are invalid and skip the read of this chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .AddPacket(10, 'b')
      .AddPacket(10, 'c')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Test that the service didn't get stuck in some indeterminate state.
  // Writing a valid chunk with a larger ID should make things work again.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(10, 'd')
      .AddPacket(10, 'e')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'd')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(10, 'e')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// -------------------
// Re-writing same chunk id
// -------------------

TEST_F(TraceBufferV2Test, Override_ReCommitBeforeRead) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(100, 'a')
      .AddPacket(100, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  EXPECT_EQ(0u, trace_buffer()->stats().chunks_rewritten());
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(100, 'a')
      .AddPacket(100, 'b')
      .AddPacket(100, 'c')
      .AddPacket(100, 'd')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  EXPECT_EQ(1u, trace_buffer()->stats().chunks_rewritten());
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(100, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(100, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(100, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(100, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Override_ReCommitAfterPartialRead) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .AddPacket(40, 'c')
      .AddPacket(50, 'd')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Override_ReCommitAfterFullRead) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .AddPacket(5, '_')  // The last frag of an incomplete chunk is ignored.
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));

  // Overriding a complete packet here would trigger a DCHECK because the packet
  // was already marked as complete.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .AddPacket(40, 'c')
      .AddPacket(50, 'd')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// See also the Malicious_Override* tests above.
TEST_F(TraceBufferV2Test, Override_ReCommitInvalid) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(40, 'c')
      .AddPacket(50, 'd')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'c')));

  // This should not happen when the producer behaves correctly, since it
  // shouldn't change the contents of chunk 0 after having allocated chunk 1.
  //
  // Since we've already started reading from chunk 1, TraceBufferV2 will
  // recognize this and discard the override.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'e')
      .AddPacket(60, 'f')
      .AddPacket(70, 'g')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Override_ReCommitReordered) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));

  // Recommit chunk 0 and add chunk 1, but do this out of order.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(50, 'd')
      .AddPacket(60, 'e')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .AddPacket(40, 'c')
      .PadTo(512)
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(60, 'e')));
}

TEST_F(TraceBufferV2Test, Override_ReCommitReorderedFragmenting) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));

  // Recommit chunk 0 and add chunk 1, but do this out of order.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(50, 'd', kContFromPrevChunk)
      .AddPacket(60, 'e')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .AddPacket(40, 'c', kContOnNextChunk)
      .PadTo(512)
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'c'),
                                        FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(60, 'e')));
}

TEST_F(TraceBufferV2Test, Override_ReCommitSameBeforeRead) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer();

  // Commit again the same chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer();

  // Then write some new content in a new chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(40, 'c')
      .AddPacket(50, 'd')
      .PadTo(512)
      .CopyIntoTraceBuffer();

  // The reader should keep reading from the new chunk.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Override_ReCommitSameAfterRead) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));

  // This re-commit should be ignored. We just re-committed an identical chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer();

  // Then write some new content in a new chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(40, 'c')
      .AddPacket(50, 'd')
      .PadTo(512)
      .CopyIntoTraceBuffer();

  // The reader should keep reading from the new chunk.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Override_ReCommitIncompleteAfterReadOutOfOrder) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));
  // The last packet in an incomplete chunk should be ignored as the producer
  // may not have completed writing it.
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Then write some new content in a new chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(40, 'c')
      .AddPacket(50, 'd')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  // The read still shouldn't be advancing past the incomplete chunk.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Recommit the original chunk with no changes but mark as complete.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/true);

  // Reading should resume from the now completed chunk.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(40, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Override_ReCommitIncompleteFragmenting) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b', kContOnNextChunk)
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));
  // The last packet in an incomplete chunk should be ignored as the producer
  // may not have completed writing it.
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Then write some new content in a new chunk.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(40, 'c', kContFromPrevChunk)
      .AddPacket(50, 'd')
      .PadTo(512)
      .CopyIntoTraceBuffer();
  // The read still shouldn't be advancing past the incomplete chunk.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Recommit the original chunk with no changes but mark as complete.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b', kContOnNextChunk)
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/true);

  // Reading should resume from the now completed chunk.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b'),
                                        FakePacketFragment(40, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, Override_EndOfBuffer) {
  ResetBuffer(3072);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(2048)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20, 'a')));
  // The last packet in an incomplete chunk should be ignored as the producer
  // may not have completed writing it.
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Recommit the original chunk with no changes but mark as complete.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a')
      .AddPacket(30, 'b')
      .PadTo(2048)
      .CopyIntoTraceBuffer(/*chunk_complete=*/true);

  // Reading should resume from the now completed chunk.
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'b')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, DiscardPolicy) {
  ResetBuffer(4096, TraceBufferV2::kDiscard);

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(32 - 16, 'a')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(4000 - 16, 'b')
      .CopyIntoTraceBuffer();
  // Leave 32 bytes free at the end of the buffer.

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32 - 16, 'a')));

  // This should still fit
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(20 - 16, 'c')
      .CopyIntoTraceBuffer();

  // Neither of these should fit.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
      .AddPacket(48 - 16, 'x')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(4))
      .AddPacket(48 - 16, 'x')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(4000 - 16, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(20 - 16, 'c')));
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // More writes should still be discarded.
  for (int i = 0; i < 3; i++) {
    CreateChunk(ProducerID(1), WriterID(i + 10), ChunkID(0))
        .AddPacket(64 - 16, 'X')
        .CopyIntoTraceBuffer();
  }
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

TEST_F(TraceBufferV2Test, NoDataLossIfReaderCatchesUp) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();

  for (WriterID i = 0; i < 3; i++) {
    CreateChunk(ProducerID(1), WriterID(i), ChunkID(0))
        .AddPacket(2000, 'a')
        .CopyIntoTraceBuffer();

    CreateChunk(ProducerID(1), WriterID(i), ChunkID(1))
        .AddPacket(1000, 'b')
        .CopyIntoTraceBuffer();

    bool previous_packet_dropped = false;
    trace_buffer()->BeginRead();
    ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
                ElementsAre(FakePacketFragment(2000, 'a')));
    ASSERT_FALSE(previous_packet_dropped);

    // This will wrap and get written @ wr_ = 0.
    CreateChunk(ProducerID(1), WriterID(i), ChunkID(2))
        .AddPacket(2000, 'c')
        .CopyIntoTraceBuffer();
    trace_buffer()->BeginRead();
    ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
                ElementsAre(FakePacketFragment(1000, 'b')));
    ASSERT_FALSE(previous_packet_dropped);

    CreateChunk(ProducerID(1), WriterID(i), ChunkID(3))
        .AddPacket(2000, 'd')
        .CopyIntoTraceBuffer();
    trace_buffer()->BeginRead();
    ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
                ElementsAre(FakePacketFragment(2000, 'c')));
    ASSERT_FALSE(previous_packet_dropped);

    ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
                ElementsAre(FakePacketFragment(2000, 'd')));
    ASSERT_FALSE(previous_packet_dropped);
    ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped), IsEmpty());
  }
}

TEST_F(TraceBufferV2Test, PacketDropOnOverwrite) {
  ResetBuffer(4096);
  SuppressClientDchecksForTesting();
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .CopyIntoTraceBuffer();

  bool previous_packet_dropped = false;
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(10, 'a')));
  ASSERT_FALSE(previous_packet_dropped);

  // Write two large chunks that don't fit into the buffer at the same time. We
  // will drop the former one before we can read it.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(2000, 'b')
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
      .AddPacket(3000, 'c')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(3000, 'c')));
  ASSERT_TRUE(previous_packet_dropped);
}

TEST_F(TraceBufferV2Test, Clone_NoFragments) {
  ResetBuffer(4096);
  const char kNumWriters = 3;
  for (char i = 'A'; i < 'A' + kNumWriters; i++) {
    ASSERT_EQ(32u, CreateChunk(ProducerID(0), WriterID(i), ChunkID(0))
                       .AddPacket(32 - 16, i)
                       .CopyIntoTraceBuffer());
  }

  // Now create a snapshot and make sure we always read all the packets.
  std::unique_ptr<TraceBuffer> snap = trace_buffer()->CloneReadOnly();
  trace_buffer_.reset();

  ASSERT_EQ(snap->used_size(), 32u * kNumWriters);
  snap->BeginRead();
  for (char i = 'A'; i < 'A' + kNumWriters; i++) {
    auto frags = ReadPacket(snap);
    ASSERT_THAT(frags, ElementsAre(FakePacketFragment(32 - 16, i)));
  }
  ASSERT_THAT(ReadPacket(snap), IsEmpty());
}

TEST_F(TraceBufferV2Test, Clone_FragmentsOutOfOrder) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .AddPacket(10, '_')
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(30, 'd')
      .CopyIntoTraceBuffer();

  {
    // Create a snapshot before the middle chunk is copied. Only 'a' should
    // be readable at this point.
    std::unique_ptr<TraceBuffer> snap = trace_buffer()->CloneReadOnly();
    snap->BeginRead();
    ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(10, 'a')));
    ASSERT_THAT(ReadPacket(snap), IsEmpty());
  }

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(20, 'c')
      .CopyIntoTraceBuffer();

  // Recommit (out of order) chunk 0, marking it as complete this time.
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(10, 'a')
      .AddPacket(10, 'b')
      .CopyIntoTraceBuffer();

  // Now all three packes should be readable.
  std::unique_ptr<TraceBuffer> snap = trace_buffer()->CloneReadOnly();
  snap->BeginRead();
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(10, 'a')));
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(10, 'b')));
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(20, 'c')));
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(30, 'd')));
  ASSERT_THAT(ReadPacket(snap), IsEmpty());
}

TEST_F(TraceBufferV2Test, Clone_WithPatches) {
  ResetBuffer(4096);
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(100, 'a')
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(2), WriterID(1), ChunkID(0))
      .AddPacket(9, 'b')
      .ClearBytes(5, 4)  // 5 := 4th payload byte. Byte 0 is the varint header
      .CopyIntoTraceBuffer();
  CreateChunk(ProducerID(3), WriterID(1), ChunkID(0))
      .AddPacket(100, 'c')
      .CopyIntoTraceBuffer();
  ASSERT_TRUE(TryPatchChunkContents(ProducerID(2), WriterID(1), ChunkID(0),
                                    {{5, {{'Y', 'M', 'C', 'A'}}}}));

  std::unique_ptr<TraceBuffer> snap = trace_buffer()->CloneReadOnly();
  snap->BeginRead();
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(100, 'a')));
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment("b00-YMCA", 8)));
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(100, 'c')));
  ASSERT_THAT(ReadPacket(snap), IsEmpty());
}

TEST_F(TraceBufferV2Test, Clone_Wrapping) {
  ResetBuffer(4096);
  const size_t kFrgSize = 1024 - 16;  // For perfect wrapping every 4 fragments
  for (WriterID i = 0; i < 6; i++) {
    CreateChunk(ProducerID(1), WriterID(i), ChunkID(0))
        .AddPacket(kFrgSize, static_cast<char>('a' + i))
        .CopyIntoTraceBuffer();
  }
  std::unique_ptr<TraceBuffer> snap = trace_buffer()->CloneReadOnly();
  ASSERT_EQ(snap->used_size(), snap->size());
  snap->BeginRead();
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(kFrgSize, 'c')));
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(kFrgSize, 'd')));
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(kFrgSize, 'e')));
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(kFrgSize, 'f')));
  ASSERT_THAT(ReadPacket(snap), IsEmpty());
}

TEST_F(TraceBufferV2Test, Clone_WrappingWithPadding) {
  ResetBuffer(4096);
  // First create one 2KB chunk, so the contents are [aaaaaaaa00000000].
  CreateChunk(ProducerID(1), WriterID(0), ChunkID(0))
      .AddPacket(2048, static_cast<char>('a'))
      .CopyIntoTraceBuffer();

  // Then write a 3KB chunk that fits in the buffer, but requires zero padding.
  // and restarting from the beginning, so the contents are [bbbbbbbbbbbb0000].
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(3192, static_cast<char>('b'))
      .CopyIntoTraceBuffer();

  std::unique_ptr<TraceBuffer> snap = trace_buffer()->CloneReadOnly();
  ASSERT_EQ(snap->used_size(), internal::TBChunk::OuterSize(3192u));
  snap->BeginRead();
  ASSERT_THAT(ReadPacket(snap), ElementsAre(FakePacketFragment(3192, 'b')));
  ASSERT_THAT(ReadPacket(snap), IsEmpty());
}

TEST_F(TraceBufferV2Test, Clone_CommitOnlyUsedSize) {
  const size_t kPages = 32;
  const size_t page_size = base::GetSysPageSize();
  ResetBuffer(page_size * kPages);
  CreateChunk(ProducerID(1), WriterID(0), ChunkID(0))
      .AddPacket(1024, static_cast<char>('a'))
      .CopyIntoTraceBuffer();

  using base::vm_test_utils::IsMapped;
  auto is_only_first_page_mapped = [&](const TraceBuffer& buf) {
    bool first_mapped = IsMapped(GetBufData(buf), page_size);
    bool rest_mapped = IsMapped(GetBufData(buf) + page_size, kPages - 1);
    return first_mapped && !rest_mapped;
  };

  // If the test doesn't work as expected until here, there is no point checking
  // that the same assumptions hold true on the cloned buffer. Various platforms
  // can legitimately pre-fetch memory even if we don't page fault (also asan).
  if (!is_only_first_page_mapped(*trace_buffer()))
    GTEST_SKIP() << "VM commit detection not supported";

  std::unique_ptr<TraceBuffer> snap = trace_buffer()->CloneReadOnly();
  ASSERT_EQ(snap->used_size(), trace_buffer()->used_size());
  ASSERT_TRUE(is_only_first_page_mapped(*snap));
}

TEST_F(TraceBufferV2Test, ChunkGaps_WithinSameReadCycle) {
  ResetBuffer(4096);

  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
                     .AddPacket(32 - 16, 'a')
                     .CopyIntoTraceBuffer());
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
                     .AddPacket(32 - 16, 'c')
                     .CopyIntoTraceBuffer());
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(4))
                     .AddPacket(32 - 16, 'd')
                     .CopyIntoTraceBuffer());
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(6))
                     .AddPacket(32 - 16, 'f')
                     .CopyIntoTraceBuffer());

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32 - 16, 'a')));

  bool previous_packet_dropped = false;
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32 - 16, 'c')));
  EXPECT_TRUE(previous_packet_dropped);

  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32 - 16, 'd')));
  EXPECT_FALSE(previous_packet_dropped);

  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32 - 16, 'f')));
  EXPECT_TRUE(previous_packet_dropped);
}

TEST_F(TraceBufferV2Test, ChunkGaps_AcrossReadCycles) {
  ResetBuffer(4096);

  // Write and consume a chunk.
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
                     .AddPacket(32 - 16, 'a')
                     .CopyIntoTraceBuffer());
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32 - 16, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Now write an consume another chunk keeping the sequence in order, and
  // ensure no data loss is reported.
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
                     .AddPacket(32 - 16, 'b')
                     .CopyIntoTraceBuffer());
  bool previous_packet_dropped = false;
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32 - 16, 'b')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
  EXPECT_FALSE(previous_packet_dropped);

  // Now write an consume another chunk, but create a gap in the chunk id.
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(4))
                     .AddPacket(32 - 16, 'd')
                     .CopyIntoTraceBuffer());
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32 - 16, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
  EXPECT_TRUE(previous_packet_dropped);

  // Now write an consume another chunk, but create a gap in the chunk id.
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(5))
                     .AddPacket(32 - 16, 'e')
                     .CopyIntoTraceBuffer());
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32 - 16, 'e')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
  EXPECT_FALSE(previous_packet_dropped);
}

// Regression test for a now-fixed long-standing issue about signalling a
// false positive data loss when using periodic reads (e.g. write_into_file).
// See b/268257546, https://github.com/google/perfetto/issues/114.
TEST_F(TraceBufferV2Test, ChunkGaps_EvenIfSequenceDisappears) {
  ResetBuffer(4096);

  // Write and consume a chunk.
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
                     .AddPacket(32 - 16, 'a')
                     .CopyIntoTraceBuffer());
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32 - 16, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());

  // Now write some large chunks from another sequence that will completely
  // obliterate the buffer.
  ASSERT_EQ(4096u, CreateChunk(ProducerID(42), WriterID(1), ChunkID(1))
                       .AddPacket(4096 - 16, '_')
                       .CopyIntoTraceBuffer());
  ASSERT_EQ(4096u, CreateChunk(ProducerID(42), WriterID(1), ChunkID(2))
                       .AddPacket(4096 - 16, '_')
                       .CopyIntoTraceBuffer());

  // This one is contiguous and shoudl't report any data loss.
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
                     .AddPacket(32 - 16, 'b')
                     .CopyIntoTraceBuffer());
  bool previous_packet_dropped = false;
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32 - 16, 'b')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
  EXPECT_FALSE(previous_packet_dropped);

  // Clobber the buffer again.
  ASSERT_EQ(4096u, CreateChunk(ProducerID(42), WriterID(1), ChunkID(3))
                       .AddPacket(4096 - 16, '_')
                       .CopyIntoTraceBuffer());
  ASSERT_EQ(4096u, CreateChunk(ProducerID(42), WriterID(1), ChunkID(4))
                       .AddPacket(4096 - 16, '_')
                       .CopyIntoTraceBuffer());

  // This one has a discontinutiy (2 -> 4) and should report a data loss.
  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(4))
                     .AddPacket(32 - 16, 'd')
                     .CopyIntoTraceBuffer());
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32 - 16, 'd')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
  EXPECT_TRUE(previous_packet_dropped);
}

TEST_F(TraceBufferV2Test, WrapAroundWithIncompleteChunk) {
  ResetBuffer(4096);

  // Commit C1, C2, C3 chunks of 1024 bytes each (1008 bytes payload + 16 bytes
  // header)
  ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
                       .AddPacket(1008, '1')
                       .CopyIntoTraceBuffer());

  // Mark C2 as incomplete - this chunk should be overwritten and not preserved
  ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
                       .AddPacket(1008, '2')
                       .CopyIntoTraceBuffer(/*chunk_complete=*/false));

  ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
                       .AddPacket(1008, '3')
                       .CopyIntoTraceBuffer());

  // Buffer now contains: [C1: 1024][C2: 1024 incomplete][C3: 1024][1024 free]

  // Write C4, C5, C6 to cause wrap around - these will overwrite C1, C2, and
  // start to overwrite C3 But since C2 is incomplete, C3 should be preserved
  ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(4))
                       .AddPacket(1008, '4')
                       .CopyIntoTraceBuffer());

  ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(5))
                       .AddPacket(1008, '5')
                       .CopyIntoTraceBuffer());

  ASSERT_EQ(1024u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(6))
                       .AddPacket(1008, '6')
                       .CopyIntoTraceBuffer());

  // Buffer should now contain: [C4: 1024][C5: 1024][C6: 1024][C3: 1024]
  // We should be able to read C3, C4, C5, C6 in that order

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1008, '3')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1008, '4')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1008, '5')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1008, '6')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Test ChunkID wraparound with complex fragmentation
TEST_F(TraceBufferV2Test, Fragments_ChunkIdMaxWraparoundFragmentation) {
  ResetBuffer(4096);
  std::vector<FakePacketFragment> expected;

  // Create a fragmented packet spanning ChunkID wraparound from UINT32_MAX to 2
  ChunkID start_id = static_cast<ChunkID>(-2);
  for (uint32_t i = 0; i < 5; ++i) {
    ChunkID chunk_id = start_id + i;
    uint8_t flags = 0;
    char data = static_cast<char>('a' + i);

    if (i == 0)
      flags = kContOnNextChunk;
    else if (i == 4)
      flags = kContFromPrevChunk;
    else
      flags = kContFromPrevChunk | kContOnNextChunk;

    CreateChunk(ProducerID(1), WriterID(1), chunk_id)
        .AddPacket(10, data, flags)
        .CopyIntoTraceBuffer();
    expected.emplace_back(10, data);
  }

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ContainerEq(expected));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Test buffer boundary alignment with fragmentation
TEST_F(TraceBufferV2Test, Alignment_ExactBufferBoundaryFragmentation) {
  ResetBuffer(4096);

  // Create a packet that fragments exactly at buffer boundaries
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(2032 - 16, 'a', kContOnNextChunk)
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(2048 - 16, 'b', kContFromPrevChunk)
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2032 - 16, 'a'),
                                        FakePacketFragment(2048 - 16, 'b')));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Test out-of-order patch application with fragmentation
TEST_F(TraceBufferV2Test, Patching_OutOfOrderPatchesWithFragmentation) {
  ResetBuffer(4096);

  // Create fragmented packet needing patches on multiple chunks
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(100, 'a', kContOnNextChunk | kChunkNeedsPatching)
      .ClearBytes(50, 4)
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(100, 'c', kContFromPrevChunk)
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(100, 'b',
                 kContFromPrevChunk | kContOnNextChunk | kChunkNeedsPatching)
      .ClearBytes(50, 4)
      .CopyIntoTraceBuffer();

  // Apply patches out of order
  ASSERT_TRUE(TryPatchChunkContents(ProducerID(1), WriterID(1), ChunkID(1),
                                    {{50, {{'B', 'B', 'B', 'B'}}}}));

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), IsEmpty());  // Still blocked by chunk 0

  ASSERT_TRUE(TryPatchChunkContents(ProducerID(1), WriterID(1), ChunkID(0),
                                    {{50, {{'A', 'A', 'A', 'A'}}}}));

  trace_buffer()->BeginRead();
  // The patches should have been applied, changing the actual payload content
  auto packet_frags = ReadPacket();
  ASSERT_EQ(packet_frags.size(), 3u);
  // Verify patches were actually applied by checking the modified payload
  // content The patches AAAA and BBBB should be visible in the payload
  EXPECT_NE(packet_frags[0].payload().find("AAAA"), std::string::npos);
  EXPECT_NE(packet_frags[1].payload().find("BBBB"), std::string::npos);
}

// Test recommit from incomplete to complete with fragmentation
TEST_F(TraceBufferV2Test, Recommit_IncompleteToCompleteWithFragments) {
  ResetBuffer(4096);

  // Create incomplete chunk
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(50, 'a')
      .AddPacket(50, 'b')
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'a')));
  ASSERT_THAT(ReadPacket(), IsEmpty());  // Blocked by incomplete chunk

  // Recommit as complete with 'c' fragment that continues to next chunk
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(50, 'a')
      .AddPacket(50, 'b')
      .AddPacket(30, 'c')
      .SetFlags(kContOnNextChunk)
      .PadTo(512)
      .CopyIntoTraceBuffer(/*chunk_complete=*/true);

  // Add continuation chunk with fragmented packet spanning across chunks
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(40, 'd')
      .SetFlags(kContFromPrevChunk | kContOnNextChunk)
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(20, 'e')
      .SetFlags(kContFromPrevChunk)
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(50, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(30, 'c'),
                                        FakePacketFragment(40, 'd'),
                                        FakePacketFragment(20, 'e')));
}

// Test DISCARD mode with fragmented packet at buffer limit
TEST_F(TraceBufferV2Test, DiscardMode_FragmentedPacketAtBoundary) {
  ResetBuffer(4096, TraceBuffer::kDiscard);

  // Fill most of buffer - leave just enough space for part of a fragmented
  // packet
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(2000, 'a')
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
      .AddPacket(1500, 'b')
      .CopyIntoTraceBuffer();

  // Add chunk with multiple fragments, last one continuing to next chunk
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(200, 'c')
      .AddPacket(150, 'd')
      .AddPacket(100, 'e')  // This fragment continues to next chunk
      .SetFlags(kContOnNextChunk)
      .CopyIntoTraceBuffer();

  // This continuation should be discarded as it would overflow the buffer
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))
      .AddPacket(500, 'f')
      .SetFlags(kContFromPrevChunk)
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(2000, 'a')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(1500, 'b')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(200, 'c')));
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(150, 'd')));
  // The fragmented packet 'e'+'f' should be incomplete due to discard
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Test maximum fragment count in a single packet
TEST_F(TraceBufferV2Test, Fragments_LargeFragment) {
  ResetBuffer(8192);
  std::vector<FakePacketFragment> expected;

  // Create a packet fragmented across 10 chunks
  for (uint32_t i = 0; i < 10; ++i) {
    uint8_t flags = 0;
    char data = static_cast<char>('a' + i);

    if (i == 0)
      flags = kContOnNextChunk;
    else if (i == 9)
      flags = kContFromPrevChunk;
    else
      flags = kContFromPrevChunk | kContOnNextChunk;

    CreateChunk(ProducerID(1), WriterID(1), ChunkID(i))
        .AddPacket(50, data, flags)
        .CopyIntoTraceBuffer();
    expected.emplace_back(50, data);
  }

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ContainerEq(expected));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Test empty chunks in long fragmentation chain
TEST_F(TraceBufferV2Test, Fragments_EmptyChunksInLongChain) {
  ResetBuffer(4096);
  std::vector<FakePacketFragment> expected;

  // Create fragmented packet with empty chunks in between
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(20, 'a', kContOnNextChunk)
      .CopyIntoTraceBuffer();
  expected.emplace_back(20, 'a');

  // Empty chunk in the middle
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(1)).CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(2))
      .AddPacket(20, 'b', kContFromPrevChunk | kContOnNextChunk)
      .CopyIntoTraceBuffer();
  expected.emplace_back(20, 'b');

  // Another empty chunk
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(3)).CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(4))
      .AddPacket(20, 'c', kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  expected.emplace_back(20, 'c');

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ContainerEq(expected));
  ASSERT_THAT(ReadPacket(), IsEmpty());
}

// Test sequence gap detection across ChunkID wraparound
TEST_F(TraceBufferV2Test, SequenceGaps_DetectionWithChunkIdWrap) {
  ResetBuffer(4096);

  // Normal sequence
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(kMaxChunkID - 1))
      .AddPacket(32, 'a')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32, 'a')));

  // Continuation across wraparound - no gap
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(kMaxChunkID))
      .AddPacket(32, 'b')
      .CopyIntoTraceBuffer();

  CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
      .AddPacket(32, 'c')
      .CopyIntoTraceBuffer();

  bool previous_packet_dropped = false;
  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32, 'b')));
  EXPECT_FALSE(previous_packet_dropped);

  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32, 'c')));
  EXPECT_FALSE(previous_packet_dropped);

  // Now create a gap across wraparound
  CreateChunk(ProducerID(1), WriterID(1), ChunkID(3))  // Gap: missing 1,2
      .AddPacket(32, 'd')
      .CopyIntoTraceBuffer();

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(nullptr, &previous_packet_dropped),
              ElementsAre(FakePacketFragment(32, 'd')));
  EXPECT_TRUE(previous_packet_dropped);  // Gap should be detected
}

// We try to write a 36 byte chunk with a 32 byte chunk, which leaves just a
// 4 byte gap. That gap is not enough for a TBChunk header, without deleting
// also c1.
// This test today passes because we force the TBChunk alignment at 16 bytes
// rather than 4 (see TODO in TBChunk::OuterSize()). If we put this back to 4
// this test will break, until we figure out how to deal with this corner case.
// Before: [c0: 36     ][c1: 4060                                   ]
// After:  [c2: 32   ]
// Note that the same could happen at the end of the buffer: imagine 36 byte
// chunk that starts precisely @ 4096 - 36, and then get overwritten by one of
// 32 bytes.
TEST_F(TraceBufferV2Test, Overwrite_SizeDiffLessThanChunkHeader) {
  ResetBuffer(4096);

  size_t c1_size = 36;
  ASSERT_EQ(c1_size, CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
                         .AddPacket(c1_size - 16, 'a')
                         .CopyIntoTraceBuffer());
  size_t pad_size = 4096 - internal::TBChunk::OuterSize(c1_size - 16);
  ASSERT_EQ(pad_size, CreateChunk(ProducerID(1), WriterID(1), ChunkID(1))
                          .AddPacket(pad_size - 16, 'b')
                          .CopyIntoTraceBuffer());
  ASSERT_EQ(4096u, size_to_end());

  ASSERT_EQ(32u, CreateChunk(ProducerID(1), WriterID(1), ChunkID(0))
                     .AddPacket(32 - 16, 'c')
                     .CopyIntoTraceBuffer());

  trace_buffer()->BeginRead();
  ASSERT_THAT(ReadPacket(), ElementsAre(FakePacketFragment(32 - 16, 'c')));
}

}  // namespace perfetto
