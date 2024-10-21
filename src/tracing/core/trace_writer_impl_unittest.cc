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

#include <vector>

#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"
#include "src/tracing/test/aligned_buffer_test.h"
#include "src/tracing/test/mock_producer_endpoint.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

using ChunkHeader = SharedMemoryABI::ChunkHeader;
using ShmemMode = SharedMemoryABI::ShmemMode;
using ::protozero::ScatteredStreamWriter;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::MockFunction;
using ::testing::Ne;
using ::testing::NiceMock;
using ::testing::Not;
using ::testing::NotNull;
using ::testing::Optional;
using ::testing::SizeIs;
using ::testing::ValuesIn;

class TraceWriterImplTest : public AlignedBufferTest {
 public:
  struct PatchKey {
    uint32_t writer_id;
    uint32_t chunk_id;
    bool operator<(const PatchKey& other) const {
      return std::tie(writer_id, chunk_id) <
             std::tie(other.writer_id, other.chunk_id);
    }
  };
  void SetUp() override {
    default_layout_ =
        SharedMemoryArbiterImpl::default_page_layout_for_testing();
    SharedMemoryArbiterImpl::set_default_layout_for_testing(
        SharedMemoryABI::PageLayout::kPageDiv4);
    AlignedBufferTest::SetUp();
    task_runner_.reset(new base::TestTaskRunner());
    arbiter_.reset(new SharedMemoryArbiterImpl(
        buf(), buf_size(), ShmemMode::kDefault, page_size(),
        &mock_producer_endpoint_, task_runner_.get()));
    ON_CALL(mock_producer_endpoint_, CommitData)
        .WillByDefault([&](const CommitDataRequest& req,
                           MockProducerEndpoint::CommitDataCallback cb) {
          last_commit_ = req;
          last_commit_callback_ = cb;
          for (const CommitDataRequest::ChunkToPatch& c :
               req.chunks_to_patch()) {
            patches_[PatchKey{c.writer_id(), c.chunk_id()}] = c.patches();
          }
        });
  }

  void TearDown() override {
    arbiter_.reset();
    task_runner_.reset();
    SharedMemoryArbiterImpl::set_default_layout_for_testing(default_layout_);
  }

  std::vector<uint8_t> CopyPayloadAndApplyPatches(
      SharedMemoryABI::Chunk& chunk) const {
    std::vector<uint8_t> copy(chunk.payload_begin(),
                              chunk.payload_begin() + chunk.payload_size());
    ChunkHeader::Packets p = chunk.header()->packets.load();

    auto it = patches_.find(PatchKey{chunk.header()->writer_id.load(),
                                     chunk.header()->chunk_id.load()});
    if (it == patches_.end()) {
      EXPECT_FALSE(p.flags & ChunkHeader::kChunkNeedsPatching);
      return copy;
    }
    EXPECT_TRUE(p.flags & ChunkHeader::kChunkNeedsPatching);

    for (const CommitDataRequest::ChunkToPatch::Patch& patch : it->second) {
      if (patch.offset() + patch.data().size() > copy.size()) {
        ADD_FAILURE() << "Patch out of bounds";
        continue;
      }
      for (size_t i = 0; i < patch.data().size(); i++) {
        copy[patch.offset() + i] =
            reinterpret_cast<const uint8_t*>(patch.data().data())[i];
      }
    }
    return copy;
  }

  // Extracts trace packets from the shared memory buffer, and returns copies of
  // them (after applying the patches received). The producer that writes to the
  // shared memory (i.e. the trace writer) must be destroyed.
  std::vector<std::string> GetPacketsFromShmemAndPatches() {
    std::vector<std::string> packets;
    SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
    bool was_fragmenting = false;
    for (size_t page_idx = 0; page_idx < abi->num_pages(); page_idx++) {
      uint32_t page_layout = abi->GetPageLayout(page_idx);
      size_t num_chunks = SharedMemoryABI::GetNumChunksForLayout(page_layout);
      for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
        SharedMemoryABI::ChunkState chunk_state =
            abi->GetChunkState(page_idx, chunk_idx);
        if (chunk_state != SharedMemoryABI::kChunkFree &&
            chunk_state != SharedMemoryABI::kChunkComplete) {
          ADD_FAILURE() << "Page " << page_idx << " chunk " << chunk_idx
                        << " unexpected state: " << chunk_state;
          continue;
        }
        SharedMemoryABI::Chunk chunk =
            abi->TryAcquireChunkForReading(page_idx, chunk_idx);
        if (!chunk.is_valid())
          continue;
        ChunkHeader::Packets p = chunk.header()->packets.load();

        EXPECT_EQ(
            was_fragmenting,
            static_cast<bool>(p.flags &
                              ChunkHeader::kFirstPacketContinuesFromPrevChunk));

        std::vector<uint8_t> payload = CopyPayloadAndApplyPatches(chunk);

        const uint8_t* read_ptr = payload.data();
        const uint8_t* const end_read_ptr = payload.data() + payload.size();

        size_t num_fragments = p.count;
        for (; num_fragments && read_ptr < end_read_ptr; num_fragments--) {
          uint64_t len;
          read_ptr =
              protozero::proto_utils::ParseVarInt(read_ptr, end_read_ptr, &len);
          if (!was_fragmenting || packets.empty()) {
            packets.push_back(std::string());
          }
          was_fragmenting = false;
          if (read_ptr + len > end_read_ptr) {
            ADD_FAILURE() << "Page " << page_idx << " chunk " << chunk_idx
                          << " malformed chunk";
          }
          packets.back().append(reinterpret_cast<const char*>(read_ptr),
                                static_cast<size_t>(len));
          read_ptr += len;
        }
        EXPECT_EQ(num_fragments, 0u);
        was_fragmenting =
            p.flags & ChunkHeader::kLastPacketContinuesOnNextChunk;
      }
    }
    // Ignore empty packets (like tracing service does).
    packets.erase(
        std::remove_if(packets.begin(), packets.end(),
                       [](const std::string& p) { return p.empty(); }),
        packets.end());
    return packets;
  }

  struct ChunkInABI {
    size_t page_idx;
    uint32_t page_layout;
    size_t chunk_idx;
  };
  std::optional<ChunkInABI> GetFirstChunkBeingWritten() {
    SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
    for (size_t page_idx = 0; page_idx < abi->num_pages(); page_idx++) {
      uint32_t page_layout = abi->GetPageLayout(page_idx);
      size_t num_chunks = SharedMemoryABI::GetNumChunksForLayout(page_layout);
      for (size_t chunk_idx = 0; chunk_idx < num_chunks; chunk_idx++) {
        SharedMemoryABI::ChunkState chunk_state =
            abi->GetChunkState(page_idx, chunk_idx);
        if (chunk_state != SharedMemoryABI::kChunkBeingWritten) {
          continue;
        }
        return ChunkInABI{page_idx, page_layout, chunk_idx};
      }
    }
    return std::nullopt;
  }

  static std::optional<std::vector<std::string>> GetChunkFragments(
      size_t packets_count,
      const void* chunk_payload,
      size_t chunk_payload_size) {
    std::vector<std::string> fragments;
    const uint8_t* read_ptr = static_cast<const uint8_t*>(chunk_payload);
    const uint8_t* const end_read_ptr = read_ptr + chunk_payload_size;

    for (size_t num_fragments = packets_count;
         num_fragments && read_ptr < end_read_ptr; num_fragments--) {
      uint64_t len;
      read_ptr =
          protozero::proto_utils::ParseVarInt(read_ptr, end_read_ptr, &len);
      if (read_ptr + len > end_read_ptr) {
        return std::nullopt;
      }
      fragments.push_back(std::string(reinterpret_cast<const char*>(read_ptr),
                                      static_cast<size_t>(len)));
      read_ptr += len;
    }
    return std::make_optional(std::move(fragments));
  }

  SharedMemoryABI::PageLayout default_layout_;
  CommitDataRequest last_commit_;
  ProducerEndpoint::CommitDataCallback last_commit_callback_;
  std::map<PatchKey, std::vector<CommitDataRequest::ChunkToPatch::Patch>>
      patches_;
  NiceMock<MockProducerEndpoint> mock_producer_endpoint_;

  std::unique_ptr<base::TestTaskRunner> task_runner_;
  std::unique_ptr<SharedMemoryArbiterImpl> arbiter_;
};

size_t const kPageSizes[] = {4096, 65536};
INSTANTIATE_TEST_SUITE_P(PageSize, TraceWriterImplTest, ValuesIn(kPageSizes));

TEST_P(TraceWriterImplTest, NewTracePacket) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);
  const size_t kNumPackets = 32;
  for (size_t i = 0; i < kNumPackets; i++) {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str(
        std::string("foobar " + std::to_string(i)));
  }

  // Destroying the TraceWriteImpl should cause the last packet to be finalized
  // and the chunk to be put back in the kChunkComplete state.
  writer.reset();

  std::vector<std::string> packets = GetPacketsFromShmemAndPatches();
  ASSERT_THAT(packets, SizeIs(kNumPackets));
  for (size_t i = 0; i < kNumPackets; i++) {
    protos::gen::TracePacket packet;
    EXPECT_TRUE(packet.ParseFromString(packets[i]));
    EXPECT_EQ(packet.for_testing().str(), "foobar " + std::to_string(i));
    if (i == 0) {
      EXPECT_TRUE(packet.first_packet_on_sequence());
    } else {
      EXPECT_FALSE(packet.first_packet_on_sequence());
    }
  }
}

TEST_P(TraceWriterImplTest, NewTracePacketLargePackets) {
  const BufferID kBufId = 42;
  const size_t chunk_size = page_size() / 4;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str(std::string("PACKET_1") +
                                       std::string(chunk_size, 'x'));
  }
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str(std::string("PACKET_2") +
                                       std::string(chunk_size, 'x'));
  }

  // Destroying the TraceWriteImpl should cause the last packet to be finalized
  // and the chunk to be put back in the kChunkComplete state.
  writer.reset();

  std::vector<std::string> packets = GetPacketsFromShmemAndPatches();
  ASSERT_THAT(packets, SizeIs(2));
  {
    protos::gen::TracePacket packet;
    EXPECT_TRUE(packet.ParseFromString(packets[0]));
    EXPECT_EQ(packet.for_testing().str(),
              std::string("PACKET_1") + std::string(chunk_size, 'x'));
  }
  {
    protos::gen::TracePacket packet;
    EXPECT_TRUE(packet.ParseFromString(packets[1]));
    EXPECT_EQ(packet.for_testing().str(),
              std::string("PACKET_2") + std::string(chunk_size, 'x'));
  }
}

// A prefix corresponding to first_packet_on_sequence = true in a serialized
// TracePacket proto.
constexpr char kFirstPacketOnSequenceFlagPrefix[] = {static_cast<char>(0xB8),
                                                     0x5, 0x1, 0x0};

TEST_P(TraceWriterImplTest, NewTracePacketTakeWriter) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);
  const size_t kNumPackets = 32;
  for (size_t i = 0; i < kNumPackets; i++) {
    ScatteredStreamWriter* sw = writer->NewTracePacket().TakeStreamWriter();
    std::string raw_proto_bytes =
        std::string("RAW_PROTO_BYTES_") + std::to_string(i);
    sw->WriteBytes(reinterpret_cast<const uint8_t*>(raw_proto_bytes.data()),
                   raw_proto_bytes.size());
    writer->FinishTracePacket();
  }

  // Destroying the TraceWriteImpl should cause the last packet to be finalized
  // and the chunk to be put back in the kChunkComplete state.
  writer.reset();

  std::vector<std::string> packets = GetPacketsFromShmemAndPatches();
  ASSERT_THAT(packets, SizeIs(kNumPackets));
  for (size_t i = 0; i < kNumPackets; i++) {
    std::string expected = "RAW_PROTO_BYTES_" + std::to_string(i);
    if (i == 0) {
      expected = kFirstPacketOnSequenceFlagPrefix + expected;
    }
    EXPECT_EQ(packets[i], expected);
  }
}

#if defined(GTEST_HAS_DEATH_TEST)
using TraceWriterImplDeathTest = TraceWriterImplTest;
INSTANTIATE_TEST_SUITE_P(PageSize,
                         TraceWriterImplDeathTest,
                         ValuesIn(kPageSizes));

TEST_P(TraceWriterImplDeathTest, NewTracePacketTakeWriterNoFinish) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  TraceWriterImpl::TracePacketHandle handle = writer->NewTracePacket();

  // Avoid a secondary DCHECK failure from ~TraceWriterImpl() =>
  // Message::Finalize() due to the stream writer being modified behind the
  // Message's back. This turns the Finalize() call into a no-op.
  handle->set_size_field(nullptr);

  ScatteredStreamWriter* sw = handle.TakeStreamWriter();
  std::string raw_proto_bytes = std::string("RAW_PROTO_BYTES");
  sw->WriteBytes(reinterpret_cast<const uint8_t*>(raw_proto_bytes.data()),
                 raw_proto_bytes.size());

  EXPECT_DEATH({ writer->NewTracePacket(); }, "");
}
#endif  // defined(GTEST_HAS_DEATH_TEST)

TEST_P(TraceWriterImplTest, AnnotatePatch) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);
  ScatteredStreamWriter* sw = writer->NewTracePacket().TakeStreamWriter();
  std::string raw_proto_bytes = std::string("RAW_PROTO_BYTES");
  sw->WriteBytes(reinterpret_cast<const uint8_t*>(raw_proto_bytes.data()),
                 raw_proto_bytes.size());

  uint8_t* patch1 =
      sw->ReserveBytes(ScatteredStreamWriter::Delegate::kPatchSize);
  ASSERT_THAT(patch1, NotNull());
  patch1[0] = 0;
  patch1[1] = 0;
  patch1[2] = 0;
  patch1[3] = 0;
  const uint8_t* old_chunk_pointer = patch1;
  patch1 = sw->AnnotatePatch(patch1);
  EXPECT_NE(patch1, old_chunk_pointer);
  ASSERT_THAT(patch1, NotNull());

  sw->WriteByte('X');

  uint8_t* patch2 =
      sw->ReserveBytes(ScatteredStreamWriter::Delegate::kPatchSize);
  ASSERT_THAT(patch2, NotNull());
  patch2[0] = 0;
  patch2[1] = 0;
  patch2[2] = 0;
  patch2[3] = 0;
  old_chunk_pointer = patch2;
  patch2 = sw->AnnotatePatch(patch2);
  EXPECT_NE(patch2, old_chunk_pointer);
  ASSERT_THAT(patch2, NotNull());

  const size_t chunk_size = page_size() / 4;
  std::string large_string(chunk_size, 'x');

  sw->WriteBytes(reinterpret_cast<const uint8_t*>(large_string.data()),
                 large_string.size());

  uint8_t* patch3 =
      sw->ReserveBytes(ScatteredStreamWriter::Delegate::kPatchSize);
  ASSERT_THAT(patch3, NotNull());
  patch3[0] = 0;
  patch3[1] = 0;
  patch3[2] = 0;
  patch3[3] = 0;
  old_chunk_pointer = patch3;
  patch3 = sw->AnnotatePatch(patch3);
  EXPECT_NE(patch3, old_chunk_pointer);
  ASSERT_THAT(patch3, NotNull());

  sw->WriteBytes(reinterpret_cast<const uint8_t*>(large_string.data()),
                 large_string.size());

  patch1[0] = 0x11;
  patch1[1] = 0x11;
  patch1[2] = 0x11;
  patch1[3] = 0x11;

  patch2[0] = 0x22;
  patch2[1] = 0x22;
  patch2[2] = 0x22;
  patch2[3] = 0x22;

  patch3[0] = 0x33;
  patch3[1] = 0x33;
  patch3[2] = 0x33;
  patch3[3] = 0x33;

  writer->FinishTracePacket();

  // Destroying the TraceWriteImpl should cause the last packet to be finalized
  // and the chunk to be put back in the kChunkComplete state.
  writer.reset();

  std::vector<std::string> packets = GetPacketsFromShmemAndPatches();
  EXPECT_THAT(
      packets,
      ElementsAre(
          kFirstPacketOnSequenceFlagPrefix + std::string("RAW_PROTO_BYTES") +
          std::string("\x11\x11\x11\x11") + std::string("X") +
          std::string("\x22\x22\x22\x22") + std::string(chunk_size, 'x') +
          std::string("\x33\x33\x33\x33") + std::string(chunk_size, 'x')));
}

TEST_P(TraceWriterImplTest, MixManualTakeAndMessage) {
  const BufferID kBufId = 42;
  const size_t chunk_size = page_size() / 4;
  const std::string large_string(chunk_size, 'x');

  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  {
    ScatteredStreamWriter* sw = writer->NewTracePacket().TakeStreamWriter();
    std::string packet1 = std::string("PACKET_1_");
    sw->WriteBytes(reinterpret_cast<const uint8_t*>(packet1.data()),
                   packet1.size());
    uint8_t* patch =
        sw->ReserveBytes(ScatteredStreamWriter::Delegate::kPatchSize);
    ASSERT_THAT(patch, NotNull());
    patch[0] = 0;
    patch[1] = 0;
    patch[2] = 0;
    patch[3] = 0;
    const uint8_t* old_chunk_pointer = patch;
    patch = sw->AnnotatePatch(patch);
    EXPECT_NE(patch, old_chunk_pointer);
    ASSERT_THAT(patch, NotNull());
    sw->WriteBytes(reinterpret_cast<const uint8_t*>(large_string.data()),
                   large_string.size());
    patch[0] = 0xFF;
    patch[1] = 0xFF;
    patch[2] = 0xFF;
    patch[3] = 0xFF;
    writer->FinishTracePacket();
  }

  {
    auto msg = writer->NewTracePacket();
    std::string packet2 = std::string("PACKET_2_");
    msg->AppendRawProtoBytes(packet2.data(), packet2.size());
    auto* nested = msg->BeginNestedMessage<protozero::Message>(1);
    nested->AppendRawProtoBytes(large_string.data(), large_string.size());
  }

  {
    ScatteredStreamWriter* sw = writer->NewTracePacket().TakeStreamWriter();
    std::string packet3 = std::string("PACKET_3_");
    sw->WriteBytes(reinterpret_cast<const uint8_t*>(packet3.data()),
                   packet3.size());
    uint8_t* patch =
        sw->ReserveBytes(ScatteredStreamWriter::Delegate::kPatchSize);
    ASSERT_THAT(patch, NotNull());
    patch[0] = 0;
    patch[1] = 0;
    patch[2] = 0;
    patch[3] = 0;
    const uint8_t* old_chunk_pointer = patch;
    patch = sw->AnnotatePatch(patch);
    EXPECT_NE(patch, old_chunk_pointer);
    ASSERT_THAT(patch, NotNull());
    sw->WriteBytes(reinterpret_cast<const uint8_t*>(large_string.data()),
                   large_string.size());
    patch[0] = 0xFF;
    patch[1] = 0xFF;
    patch[2] = 0xFF;
    patch[3] = 0xFF;
    writer->FinishTracePacket();
  }

  // Destroying the TraceWriteImpl should cause the last packet to be finalized
  // and the chunk to be put back in the kChunkComplete state.
  writer.reset();

  uint8_t buf[protozero::proto_utils::kMessageLengthFieldSize];
  protozero::proto_utils::WriteRedundantVarInt(
      static_cast<uint32_t>(large_string.size()), buf,
      protozero::proto_utils::kMessageLengthFieldSize);
  std::string encoded_size(reinterpret_cast<char*>(buf), sizeof(buf));

  std::vector<std::string> packets = GetPacketsFromShmemAndPatches();
  EXPECT_THAT(
      packets,
      ElementsAre(kFirstPacketOnSequenceFlagPrefix + std::string("PACKET_1_") +
                      std::string("\xFF\xFF\xFF\xFF") +
                      std::string(chunk_size, 'x'),
                  std::string("PACKET_2_") + std::string("\x0A") +
                      encoded_size + std::string(chunk_size, 'x'),
                  std::string("PACKET_3_") + std::string("\xFF\xFF\xFF\xFF") +
                      std::string(chunk_size, 'x')));
}

TEST_P(TraceWriterImplTest, MessageHandleDestroyedPacketScrapable) {
  const BufferID kBufId = 42;

  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  auto packet = writer->NewTracePacket();
  packet->set_for_testing()->set_str("packet1");

  std::optional<ChunkInABI> chunk_in_abi = GetFirstChunkBeingWritten();
  ASSERT_TRUE(chunk_in_abi.has_value());

  auto* abi = arbiter_->shmem_abi_for_testing();
  SharedMemoryABI::Chunk chunk =
      abi->GetChunkUnchecked(chunk_in_abi->page_idx, chunk_in_abi->page_layout,
                             chunk_in_abi->chunk_idx);
  ASSERT_TRUE(chunk.is_valid());

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);

  packet = protozero::MessageHandle<protos::pbzero::TracePacket>();

  // After destroying the message handle, the chunk header should have an
  // inflated packet count.
  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);

  writer.reset();

  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkComplete);
  EXPECT_THAT(GetChunkFragments(1, chunk.payload_begin(), chunk.payload_size()),
              Optional(ElementsAre(Not(IsEmpty()))));
}

TEST_P(TraceWriterImplTest, FinishTracePacketScrapable) {
  const BufferID kBufId = 42;

  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  {
    protos::pbzero::TestEvent test_event;
    protozero::MessageArena arena;
    ScatteredStreamWriter* sw = writer->NewTracePacket().TakeStreamWriter();
    uint8_t data[protozero::proto_utils::kMaxTagEncodedSize];
    uint8_t* data_end = protozero::proto_utils::WriteVarInt(
        protozero::proto_utils::MakeTagLengthDelimited(
            protos::pbzero::TracePacket::kForTestingFieldNumber),
        data);
    sw->WriteBytes(data, static_cast<size_t>(data_end - data));
    test_event.Reset(sw, &arena);
    test_event.set_size_field(
        sw->ReserveBytes(protozero::proto_utils::kMessageLengthFieldSize));
    test_event.set_str("payload1");
  }

  std::optional<ChunkInABI> chunk_in_abi = GetFirstChunkBeingWritten();
  ASSERT_TRUE(chunk_in_abi.has_value());

  auto* abi = arbiter_->shmem_abi_for_testing();
  SharedMemoryABI::Chunk chunk =
      abi->GetChunkUnchecked(chunk_in_abi->page_idx, chunk_in_abi->page_layout,
                             chunk_in_abi->chunk_idx);
  ASSERT_TRUE(chunk.is_valid());

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);

  writer->FinishTracePacket();

  // After a call to FinishTracePacket, the chunk header should have an inflated
  // packet count.
  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);
  EXPECT_THAT(GetChunkFragments(1, chunk.payload_begin(), chunk.payload_size()),
              Optional(ElementsAre(Not(IsEmpty()))));

  // An extra call to FinishTracePacket should have no effect.
  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);
  EXPECT_THAT(GetChunkFragments(1, chunk.payload_begin(), chunk.payload_size()),
              Optional(ElementsAre(Not(IsEmpty()))));

  writer.reset();

  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkComplete);
  EXPECT_THAT(GetChunkFragments(2, chunk.payload_begin(), chunk.payload_size()),
              Optional(ElementsAre(Not(IsEmpty()), IsEmpty())));
}

TEST_P(TraceWriterImplTest,
       MessageHandleDestroyedAndFinishTracePacketScrapable) {
  const BufferID kBufId = 42;

  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  auto packet = writer->NewTracePacket();
  packet->set_for_testing()->set_str("packet1");

  std::optional<ChunkInABI> chunk_in_abi = GetFirstChunkBeingWritten();
  ASSERT_TRUE(chunk_in_abi.has_value());

  auto* abi = arbiter_->shmem_abi_for_testing();
  SharedMemoryABI::Chunk chunk =
      abi->GetChunkUnchecked(chunk_in_abi->page_idx, chunk_in_abi->page_layout,
                             chunk_in_abi->chunk_idx);
  ASSERT_TRUE(chunk.is_valid());

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);
  packet = protozero::MessageHandle<protos::pbzero::TracePacket>();

  // After destroying the message handle, the chunk header should have an
  // inflated packet count.
  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);

  writer->FinishTracePacket();

  // An extra call to FinishTracePacket should have no effect.
  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);
  EXPECT_THAT(GetChunkFragments(1, chunk.payload_begin(), chunk.payload_size()),
              Optional(ElementsAre(Not(IsEmpty()))));

  writer.reset();

  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkComplete);
  EXPECT_THAT(GetChunkFragments(2, chunk.payload_begin(), chunk.payload_size()),
              Optional(ElementsAre(Not(IsEmpty()), IsEmpty())));
}

TEST_P(TraceWriterImplTest, MessageHandleDestroyedPacketFullChunk) {
  const BufferID kBufId = 42;

  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  auto packet = writer->NewTracePacket();
  protos::pbzero::TestEvent* test_event = packet->set_for_testing();
  std::string chunk_filler(test_event->stream_writer()->bytes_available(),
                           '\0');
  test_event->AppendRawProtoBytes(chunk_filler.data(), chunk_filler.size());

  std::optional<ChunkInABI> chunk_in_abi = GetFirstChunkBeingWritten();
  ASSERT_TRUE(chunk_in_abi.has_value());

  auto* abi = arbiter_->shmem_abi_for_testing();
  SharedMemoryABI::Chunk chunk =
      abi->GetChunkUnchecked(chunk_in_abi->page_idx, chunk_in_abi->page_layout,
                             chunk_in_abi->chunk_idx);
  ASSERT_TRUE(chunk.is_valid());

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);
  // Finish the TracePacket: since there's no space for an empty packet, the
  // trace writer should immediately mark the chunk as completed.
  packet = protozero::MessageHandle<protos::pbzero::TracePacket>();

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkComplete);

  writer.reset();

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkComplete);
}

TEST_P(TraceWriterImplTest, FinishTracePacketFullChunk) {
  const BufferID kBufId = 42;

  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  {
    protos::pbzero::TestEvent test_event;
    protozero::MessageArena arena;
    ScatteredStreamWriter* sw = writer->NewTracePacket().TakeStreamWriter();
    uint8_t data[protozero::proto_utils::kMaxTagEncodedSize];
    uint8_t* data_end = protozero::proto_utils::WriteVarInt(
        protozero::proto_utils::MakeTagLengthDelimited(
            protos::pbzero::TracePacket::kForTestingFieldNumber),
        data);
    sw->WriteBytes(data, static_cast<size_t>(data_end - data));
    test_event.Reset(sw, &arena);
    test_event.set_size_field(
        sw->ReserveBytes(protozero::proto_utils::kMessageLengthFieldSize));
    std::string chunk_filler(sw->bytes_available(), '\0');
    test_event.AppendRawProtoBytes(chunk_filler.data(), chunk_filler.size());
  }

  std::optional<ChunkInABI> chunk_in_abi = GetFirstChunkBeingWritten();
  ASSERT_TRUE(chunk_in_abi.has_value());

  auto* abi = arbiter_->shmem_abi_for_testing();
  SharedMemoryABI::Chunk chunk =
      abi->GetChunkUnchecked(chunk_in_abi->page_idx, chunk_in_abi->page_layout,
                             chunk_in_abi->chunk_idx);
  ASSERT_TRUE(chunk.is_valid());

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkBeingWritten);

  // Finish the TracePacket: since there's no space for an empty packet, the
  // trace writer should immediately mark the chunk as completed, instead of
  // inflating the count.
  writer->FinishTracePacket();

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkComplete);

  writer.reset();

  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_EQ(abi->GetChunkState(chunk_in_abi->page_idx, chunk_in_abi->chunk_idx),
            SharedMemoryABI::ChunkState::kChunkComplete);
}

TEST_P(TraceWriterImplTest, FragmentingPacketWithProducerAndServicePatching) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  // Write a packet that's guaranteed to span more than a single chunk, but
  // less than two chunks.
  auto packet = writer->NewTracePacket();
  size_t chunk_size = page_size() / 4;
  std::string large_string(chunk_size, 'x');
  packet->set_for_testing()->set_str(large_string);

  // First chunk should be committed.
  arbiter_->FlushPendingCommitDataRequests();
  ASSERT_THAT(last_commit_.chunks_to_move(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_move()[0].page(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].chunk(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].target_buffer(), kBufId);
  EXPECT_THAT(last_commit_.chunks_to_patch(), SizeIs(0));

  // We will simulate a batching cycle by first setting the batching period to
  // a very large value and then force-flushing when we are done writing data.
  arbiter_->SetDirectSMBPatchingSupportedByService();
  ASSERT_TRUE(arbiter_->EnableDirectSMBPatching());
  arbiter_->SetBatchCommitsDuration(UINT32_MAX);

  // Write a second packet that's guaranteed to span more than a single chunk.
  // Starting a new trace packet should cause the patches for the first packet
  // (i.e. for the first chunk) to be queued for sending to the service. They
  // cannot be applied locally because the first chunk was already committed.
  packet->Finalize();
  auto packet2 = writer->NewTracePacket();
  packet2->set_for_testing()->set_str(large_string);

  // Starting a new packet yet again should cause the patches for the second
  // packet (i.e. for the second chunk) to be applied in the producer, because
  // the second chunk has not been committed yet.
  packet2->Finalize();
  auto packet3 = writer->NewTracePacket();

  // Simulate the end of the batching period, which should trigger a commit to
  // the service.
  arbiter_->FlushPendingCommitDataRequests();

  SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();

  // The first allocated chunk should be complete but need patching, since the
  // packet extended past the chunk and no patches for the packet size or
  // string field size were applied yet.
  ASSERT_EQ(abi->GetChunkState(0u, 0u), SharedMemoryABI::kChunkComplete);
  auto chunk = abi->TryAcquireChunkForReading(0u, 0u);
  ASSERT_TRUE(chunk.is_valid());
  EXPECT_EQ(chunk.header()->packets.load().count, 1u);
  EXPECT_TRUE(chunk.header()->packets.load().flags &
              SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
  EXPECT_TRUE(chunk.header()->packets.load().flags &
              SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);

  // Verify that a patch for the first chunk was sent to the service.
  ASSERT_THAT(last_commit_.chunks_to_patch(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_patch()[0].writer_id(), writer->writer_id());
  EXPECT_EQ(last_commit_.chunks_to_patch()[0].target_buffer(), kBufId);
  EXPECT_EQ(last_commit_.chunks_to_patch()[0].chunk_id(),
            chunk.header()->chunk_id.load());
  EXPECT_FALSE(last_commit_.chunks_to_patch()[0].has_more_patches());
  EXPECT_THAT(last_commit_.chunks_to_patch()[0].patches(), SizeIs(1));

  // Verify that the second chunk was committed.
  ASSERT_THAT(last_commit_.chunks_to_move(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_move()[0].page(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].chunk(), 1u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].target_buffer(), kBufId);

  // The second chunk should be in a complete state and should not need
  // patching, as the patches to it should have been applied in the producer.
  ASSERT_EQ(abi->GetChunkState(0u, 1u), SharedMemoryABI::kChunkComplete);
  auto chunk2 = abi->TryAcquireChunkForReading(0u, 1u);
  ASSERT_TRUE(chunk2.is_valid());
  EXPECT_EQ(chunk2.header()->packets.load().count, 2);
  EXPECT_TRUE(chunk2.header()->packets.load().flags &
              SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);
  EXPECT_FALSE(chunk2.header()->packets.load().flags &
               SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
}

TEST_P(TraceWriterImplTest, FragmentingPacketWithoutEnablingProducerPatching) {
  // We will simulate a batching cycle by first setting the batching period to
  // a very large value and will force flush to simulate a flush happening
  // when we believe it should - in this case when a patch is encountered.
  //
  // Note: direct producer-side patching should be disabled by default.
  arbiter_->SetBatchCommitsDuration(UINT32_MAX);

  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  // Write a packet that's guaranteed to span more than a single chunk.
  auto packet = writer->NewTracePacket();
  size_t chunk_size = page_size() / 4;
  std::string large_string(chunk_size, 'x');
  packet->set_for_testing()->set_str(large_string);

  // Starting a new packet should cause the first chunk and its patches to be
  // committed to the service.
  packet->Finalize();
  auto packet2 = writer->NewTracePacket();
  arbiter_->FlushPendingCommitDataRequests();

  // The first allocated chunk should be complete but need patching, since the
  // packet extended past the chunk and no patches for the packet size or
  // string field size were applied in the producer.
  SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
  ASSERT_EQ(abi->GetChunkState(0u, 0u), SharedMemoryABI::kChunkComplete);
  auto chunk = abi->TryAcquireChunkForReading(0u, 0u);
  ASSERT_TRUE(chunk.is_valid());
  EXPECT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_TRUE(chunk.header()->packets.load().flags &
              SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
  EXPECT_TRUE(chunk.header()->packets.load().flags &
              SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);

  // The first chunk was committed.
  ASSERT_THAT(last_commit_.chunks_to_move(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_move()[0].page(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].chunk(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].target_buffer(), kBufId);

  // The patches for the first chunk were committed.
  ASSERT_THAT(last_commit_.chunks_to_patch(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_patch()[0].writer_id(), writer->writer_id());
  EXPECT_EQ(last_commit_.chunks_to_patch()[0].target_buffer(), kBufId);
  EXPECT_EQ(last_commit_.chunks_to_patch()[0].chunk_id(),
            chunk.header()->chunk_id.load());
  EXPECT_FALSE(last_commit_.chunks_to_patch()[0].has_more_patches());
  EXPECT_THAT(last_commit_.chunks_to_patch()[0].patches(), SizeIs(1));
}

// Sets up a scenario in which the SMB is exhausted and TraceWriter fails to
// get a new chunk while fragmenting a packet. Verifies that data is dropped
// until the SMB is freed up and TraceWriter can get a new chunk.
TEST_P(TraceWriterImplTest, FragmentingPacketWhileBufferExhausted) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer =
      arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);

  // Write a small first packet, so that |writer| owns a chunk.
  auto packet = writer->NewTracePacket();
  EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                   ->drop_packets_for_testing());
  // 3 bytes for the first_packet_on_sequence flag.
  EXPECT_EQ(packet->Finalize(), 3u);

  // Grab all the remaining chunks in the SMB in new writers.
  std::array<std::unique_ptr<TraceWriter>, kNumPages * 4 - 1> other_writers;
  for (size_t i = 0; i < other_writers.size(); i++) {
    other_writers[i] =
        arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);
    auto other_writer_packet = other_writers[i]->NewTracePacket();
    EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(other_writers[i].get())
                     ->drop_packets_for_testing());
  }

  // Write a packet that's guaranteed to span more than a single chunk,
  // causing |writer| to attempt to acquire a new chunk but fail to do so.
  auto packet2 = writer->NewTracePacket();
  size_t chunk_size = page_size() / 4;
  std::string large_string(chunk_size, 'x');
  packet2->set_for_testing()->set_str(large_string);

  EXPECT_TRUE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                  ->drop_packets_for_testing());

  // First chunk should be committed.
  arbiter_->FlushPendingCommitDataRequests();
  ASSERT_THAT(last_commit_.chunks_to_move(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_move()[0].page(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].chunk(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].target_buffer(), kBufId);
  EXPECT_THAT(last_commit_.chunks_to_patch(), SizeIs(0));

  // It should not need patching and not have the continuation flag set.
  SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
  ASSERT_EQ(SharedMemoryABI::kChunkComplete, abi->GetChunkState(0u, 0u));
  auto chunk = abi->TryAcquireChunkForReading(0u, 0u);
  ASSERT_TRUE(chunk.is_valid());
  EXPECT_EQ(chunk.header()->packets.load().count, 2);
  EXPECT_FALSE(chunk.header()->packets.load().flags &
               SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
  EXPECT_FALSE(chunk.header()->packets.load().flags &
               SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);

  // Writing more data while in garbage mode succeeds. This data is dropped.
  packet2->Finalize();
  auto packet3 = writer->NewTracePacket();
  packet3->set_for_testing()->set_str(large_string);

  // Release the |writer|'s first chunk as free, so that it can grab it again.
  abi->ReleaseChunkAsFree(std::move(chunk));

  // Starting a new packet should cause TraceWriter to attempt to grab a new
  // chunk again, because we wrote enough data to wrap the garbage chunk.
  packet3->Finalize();
  auto packet4 = writer->NewTracePacket();

  // Grabbing the chunk should have succeeded.
  EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                   ->drop_packets_for_testing());

  // The first packet in the chunk should have the previous_packet_dropped
  // flag set, so shouldn't be empty.
  EXPECT_GT(packet4->Finalize(), 0u);

  // Flushing the writer causes the chunk to be released again.
  writer->Flush();
  EXPECT_THAT(last_commit_.chunks_to_move(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_move()[0].page(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].chunk(), 0u);
  EXPECT_THAT(last_commit_.chunks_to_patch(), SizeIs(0));

  // Chunk should contain only |packet4| and not have any continuation flag
  // set.
  ASSERT_EQ(abi->GetChunkState(0u, 0u), SharedMemoryABI::kChunkComplete);
  chunk = abi->TryAcquireChunkForReading(0u, 0u);
  ASSERT_TRUE(chunk.is_valid());
  ASSERT_EQ(chunk.header()->packets.load().count, 1);
  EXPECT_FALSE(chunk.header()->packets.load().flags &
               SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
  EXPECT_FALSE(
      chunk.header()->packets.load().flags &
      SharedMemoryABI::ChunkHeader::kFirstPacketContinuesFromPrevChunk);
  EXPECT_FALSE(chunk.header()->packets.load().flags &
               SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);
}

// Verifies that a TraceWriter that is flushed before the SMB is full and then
// acquires a garbage chunk later recovers and writes a
// previous_packet_dropped marker into the trace.
TEST_P(TraceWriterImplTest, FlushBeforeBufferExhausted) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer =
      arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);

  // Write a small first packet and flush it, so that |writer| no longer owns
  // any chunk.
  auto packet = writer->NewTracePacket();
  EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                   ->drop_packets_for_testing());
  // 3 bytes for the first_packet_on_sequence flag.
  EXPECT_EQ(packet->Finalize(), 3u);

  // Flush the first chunk away.
  writer->Flush();

  // First chunk should be committed. Don't release it as free just yet.
  arbiter_->FlushPendingCommitDataRequests();
  ASSERT_THAT(last_commit_.chunks_to_move(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_move()[0].page(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].chunk(), 0u);

  // Grab all the remaining chunks in the SMB in new writers.
  std::array<std::unique_ptr<TraceWriter>, kNumPages * 4 - 1> other_writers;
  for (size_t i = 0; i < other_writers.size(); i++) {
    other_writers[i] =
        arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);
    auto other_writer_packet = other_writers[i]->NewTracePacket();
    EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(other_writers[i].get())
                     ->drop_packets_for_testing());
  }

  // Write another packet, causing |writer| to acquire a garbage chunk.
  auto packet2 = writer->NewTracePacket();
  EXPECT_TRUE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                  ->drop_packets_for_testing());

  // Writing more data while in garbage mode succeeds. This data is dropped.
  // Make sure that we fill the garbage chunk, so that |writer| tries to
  // re-acquire a valid chunk for the next packet.
  size_t chunk_size = page_size() / 4;
  std::string large_string(chunk_size, 'x');
  packet2->set_for_testing()->set_str(large_string);
  packet2->Finalize();

  // Next packet should still be in the garbage chunk.
  auto packet3 = writer->NewTracePacket();
  EXPECT_TRUE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                  ->drop_packets_for_testing());

  // Release the first chunk as free, so |writer| can acquire it again.
  SharedMemoryABI* abi = arbiter_->shmem_abi_for_testing();
  ASSERT_EQ(SharedMemoryABI::kChunkComplete, abi->GetChunkState(0u, 0u));
  auto chunk = abi->TryAcquireChunkForReading(0u, 0u);
  ASSERT_TRUE(chunk.is_valid());
  abi->ReleaseChunkAsFree(std::move(chunk));

  // Fill the garbage chunk, so that the writer attempts to grab another chunk
  // for |packet4|.
  packet3->set_for_testing()->set_str(large_string);
  packet3->Finalize();

  // Next packet should go into the reacquired chunk we just released.
  auto packet4 = writer->NewTracePacket();
  EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                   ->drop_packets_for_testing());

  // The first packet in the chunk should have the previous_packet_dropped
  // flag set, so shouldn't be empty.
  EXPECT_GT(packet4->Finalize(), 0u);

  // Flushing the writer causes the chunk to be released again.
  writer->Flush();
  ASSERT_THAT(last_commit_.chunks_to_move(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_move()[0].page(), 0u);
  EXPECT_EQ(last_commit_.chunks_to_move()[0].chunk(), 0u);
  EXPECT_THAT(last_commit_.chunks_to_patch(), SizeIs(0));

  // Chunk should contain only |packet4| and not have any continuation flag
  // set.
  ASSERT_EQ(SharedMemoryABI::kChunkComplete, abi->GetChunkState(0u, 0u));
  chunk = abi->TryAcquireChunkForReading(0u, 0u);
  ASSERT_TRUE(chunk.is_valid());
  ASSERT_EQ(chunk.header()->packets.load().count, 1);
  ASSERT_FALSE(chunk.header()->packets.load().flags &
               SharedMemoryABI::ChunkHeader::kChunkNeedsPatching);
  ASSERT_FALSE(
      chunk.header()->packets.load().flags &
      SharedMemoryABI::ChunkHeader::kFirstPacketContinuesFromPrevChunk);
  ASSERT_FALSE(chunk.header()->packets.load().flags &
               SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk);
}

// Regression test that verifies that flushing a TraceWriter while a
// fragmented packet still has uncommitted patches doesn't hit a DCHECK /
// crash the writer thread.
TEST_P(TraceWriterImplTest, FlushAfterFragmentingPacketWhileBufferExhausted) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer =
      arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);

  // Write a small first packet, so that |writer| owns a chunk.
  auto packet = writer->NewTracePacket();
  EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                   ->drop_packets_for_testing());
  // 3 bytes for the first_packet_on_sequence flag.
  EXPECT_EQ(packet->Finalize(), 3u);

  // Grab all but one of the remaining chunks in the SMB in new writers.
  std::array<std::unique_ptr<TraceWriter>, kNumPages * 4 - 2> other_writers;
  for (size_t i = 0; i < other_writers.size(); i++) {
    other_writers[i] =
        arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);
    auto other_writer_packet = other_writers[i]->NewTracePacket();
    EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(other_writers[i].get())
                     ->drop_packets_for_testing());
  }

  // Write a packet that's guaranteed to span more than a two chunks, causing
  // |writer| to attempt to acquire two new chunks, but fail to acquire the
  // second.
  auto packet2 = writer->NewTracePacket();
  size_t chunk_size = page_size() / 4;
  std::string large_string(chunk_size * 2, 'x');
  packet2->set_for_testing()->set_str(large_string);

  EXPECT_TRUE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                  ->drop_packets_for_testing());

  // First two chunks should be committed.
  arbiter_->FlushPendingCommitDataRequests();
  ASSERT_THAT(last_commit_.chunks_to_move(), SizeIs(2));

  // Flushing should succeed, even though some patches are still in the
  // writer's patch list.
  packet2->Finalize();
  writer->Flush();
}

TEST_P(TraceWriterImplTest, GarbageChunkWrap) {
  const BufferID kBufId = 42;

  // Grab all chunks in the SMB in new writers.
  std::array<std::unique_ptr<TraceWriter>, kNumPages * 4> other_writers;
  for (size_t i = 0; i < other_writers.size(); i++) {
    other_writers[i] =
        arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);
    auto other_writer_packet = other_writers[i]->NewTracePacket();
    EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(other_writers[i].get())
                     ->drop_packets_for_testing());
  }

  // `writer` will only get garbage chunks, since the SMB is exhausted.
  std::unique_ptr<TraceWriter> writer =
      arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);

  const size_t chunk_size = page_size() / 4;
  std::string half_chunk_string(chunk_size / 2, 'x');

  // Fill the first half of the garbage chunk.
  {
    auto packet = writer->NewTracePacket();
    EXPECT_TRUE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                    ->drop_packets_for_testing());
    packet->set_for_testing()->set_str(half_chunk_string);
  }

  // Fill the second half of the garbage chunk and more. This will call
  // GetNewBuffer() and restart from the beginning of the garbage chunk.
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str(half_chunk_string);
  }

  // Check that TraceWriterImpl can write at the beginning of the garbage chunk
  // without any problems.
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str("str");
  }
}

TEST_P(TraceWriterImplTest, AnnotatePatchWhileBufferExhausted) {
  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer =
      arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);

  // Write a small first packet, so that |writer| owns a chunk.
  ScatteredStreamWriter* sw = writer->NewTracePacket().TakeStreamWriter();
  sw->WriteBytes(reinterpret_cast<const uint8_t*>("X"), 1);
  writer->FinishTracePacket();
  EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                   ->drop_packets_for_testing());

  // Grab all but one of the remaining chunks in the SMB in new writers.
  std::array<std::unique_ptr<TraceWriter>, kNumPages * 4 - 2> other_writers;
  for (size_t i = 0; i < other_writers.size(); i++) {
    other_writers[i] =
        arbiter_->CreateTraceWriter(kBufId, BufferExhaustedPolicy::kDrop);
    auto other_writer_packet = other_writers[i]->NewTracePacket();
    EXPECT_FALSE(reinterpret_cast<TraceWriterImpl*>(other_writers[i].get())
                     ->drop_packets_for_testing());
  }

  // Write a packet that's guaranteed to span more than a two chunks, causing
  // |writer| to attempt to acquire two new chunks, but fail to acquire the
  // second.
  sw = writer->NewTracePacket().TakeStreamWriter();
  size_t chunk_size = page_size() / 4;
  std::string large_string(chunk_size * 2, 'x');
  sw->WriteBytes(reinterpret_cast<const uint8_t*>(large_string.data()),
                 large_string.size());

  EXPECT_TRUE(reinterpret_cast<TraceWriterImpl*>(writer.get())
                  ->drop_packets_for_testing());

  uint8_t* patch1 =
      sw->ReserveBytes(ScatteredStreamWriter::Delegate::kPatchSize);
  ASSERT_THAT(patch1, NotNull());
  patch1[0] = 0;
  patch1[1] = 0;
  patch1[2] = 0;
  patch1[3] = 0;
  patch1 = sw->AnnotatePatch(patch1);
  EXPECT_THAT(patch1, IsNull());

  // First two chunks should be committed.
  arbiter_->FlushPendingCommitDataRequests();
  ASSERT_THAT(last_commit_.chunks_to_move(), SizeIs(2));

  // Flushing should succeed, even though some patches are still in the
  // writer's patch list.
  writer->FinishTracePacket();
  writer->Flush();
}

TEST_P(TraceWriterImplTest, Flush) {
  MockFunction<void()> flush_cb;

  const BufferID kBufId = 42;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);
  {
    auto packet = writer->NewTracePacket();
    packet->set_for_testing()->set_str("foobar");
  }

  EXPECT_CALL(flush_cb, Call).Times(0);
  ASSERT_FALSE(last_commit_callback_);
  writer->Flush(flush_cb.AsStdFunction());
  ASSERT_TRUE(last_commit_callback_);
  EXPECT_CALL(flush_cb, Call).Times(1);
  last_commit_callback_();
}

TEST_P(TraceWriterImplTest, NestedMsgsPatches) {
  const BufferID kBufId = 42;
  const uint32_t kNestedFieldId = 1;
  const uint32_t kStringFieldId = 2;
  const uint32_t kIntFieldId = 3;
  std::unique_ptr<TraceWriter> writer = arbiter_->CreateTraceWriter(kBufId);

  size_t chunk_size = page_size() / 4;
  std::string large_string(chunk_size, 'x');

  auto packet = writer->NewTracePacket();
  auto* nested1 =
      packet->BeginNestedMessage<protozero::Message>(kNestedFieldId);
  auto* nested2 =
      nested1->BeginNestedMessage<protozero::Message>(kNestedFieldId);
  auto* nested3 =
      nested2->BeginNestedMessage<protozero::Message>(kNestedFieldId);
  uint8_t* const old_nested_1_size_field = nested1->size_field();
  uint8_t* const old_nested_2_size_field = nested2->size_field();
  uint8_t* const old_nested_3_size_field = nested3->size_field();
  EXPECT_THAT(old_nested_1_size_field, NotNull());
  EXPECT_THAT(old_nested_2_size_field, NotNull());
  EXPECT_THAT(old_nested_3_size_field, NotNull());

  // Append a small field, which will fit in the current chunk.
  nested3->AppendVarInt<uint64_t>(kIntFieldId, 1);

  // The `size_field`s still point to the same old location, inside the chunk.
  EXPECT_EQ(nested1->size_field(), old_nested_1_size_field);
  EXPECT_EQ(nested2->size_field(), old_nested_2_size_field);
  EXPECT_EQ(nested3->size_field(), old_nested_3_size_field);

  // Append a large string, which will not fit in the current chunk.
  nested3->AppendString(kStringFieldId, large_string);

  // The `size_field`s will now point to different locations (patches).
  EXPECT_THAT(nested1->size_field(),
              AllOf(Ne(old_nested_1_size_field), NotNull()));
  EXPECT_THAT(nested2->size_field(),
              AllOf(Ne(old_nested_2_size_field), NotNull()));
  EXPECT_THAT(nested3->size_field(),
              AllOf(Ne(old_nested_3_size_field), NotNull()));

  packet->Finalize();
  writer->Flush();

  arbiter_->FlushPendingCommitDataRequests();

  ASSERT_THAT(last_commit_.chunks_to_patch(), SizeIs(1));
  EXPECT_EQ(last_commit_.chunks_to_patch()[0].writer_id(), writer->writer_id());
  EXPECT_EQ(last_commit_.chunks_to_patch()[0].target_buffer(), kBufId);
  EXPECT_FALSE(last_commit_.chunks_to_patch()[0].has_more_patches());
  EXPECT_THAT(last_commit_.chunks_to_patch()[0].patches(), SizeIs(3));
}

// TODO(primiano): add multi-writer test.

}  // namespace
}  // namespace perfetto
