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

#include "src/tracing/service/trace_buffer_v3.h"

#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <zstd.h>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protovm/vm.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/test/fake_packet.h"
#include "src/tracing/test/protovm_test_program.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;

namespace {

using internal::BundledBuffer;
using internal::DecodeArena;

constexpr uint32_t kLossPresent =
    protos::pbzero::TracePacket::DATA_LOSS_PRESENT;
constexpr uint32_t kLossReadGap =
    protos::pbzero::TracePacket::DATA_LOSS_READ_GAP;
constexpr uint32_t kLossCorrupted =
    protos::pbzero::TracePacket::DATA_LOSS_CHUNK_CORRUPTED;
constexpr uint32_t kLossOverwrite =
    protos::pbzero::TracePacket::DATA_LOSS_OVERWRITE;
constexpr uint32_t kLossSmbFull =
    protos::pbzero::TracePacket::DATA_LOSS_SMB_FULL;

constexpr uint8_t kContFromPrevChunk =
    SharedMemoryABI::ChunkHeader::kFirstPacketContinuesFromPrevChunk;
constexpr uint8_t kContOnNextChunk =
    SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk;
constexpr uint8_t kChunkNeedsPatching =
    SharedMemoryABI::ChunkHeader::kChunkNeedsPatching;

// A valid TracePacket with for_testing.str = |str|. If |producer_loss| is not
// zero, the packet also has previous_packet_dropped, as a TraceWriter sets it.
// Without loss, |str| up to 125 bytes takes str.size() + 5 bytes in the packet.
std::string Packet(const std::string& str, uint32_t producer_loss = 0) {
  protozero::HeapBuffered<protos::pbzero::TracePacket> packet;
  packet->set_for_testing()->set_str(str);
  if (producer_loss)
    packet->set_previous_packet_dropped(producer_loss);
  return packet.SerializeAsString();
}

// The same packet in the proto-group encoding of SMB v2 writers.
std::string PacketV2(const std::string& str) {
  return std::string("\xa3\x38\x0a") + static_cast<char>(str.size()) + str +
         '\x04';
}

// A TracePacket that the ProtoVM of CreateProtoVmProgram() merges.
std::string PatchPacket(const std::string& string_to_merge,
                        std::optional<int32_t> int_to_merge = std::nullopt) {
  protozero::HeapBuffered<protos::pbzero::TracePacket> packet;
  auto* patch = packet->set_for_testing()->set_protovm_patch();
  patch->set_string_to_merge(string_to_merge);
  if (int_to_merge)
    patch->set_int_to_merge(*int_to_merge);
  return packet.SerializeAsString();
}

// |size| pseudo-random bytes for packets that compress poorly.
std::string RandomBytes(size_t size, uint32_t seed) {
  std::minstd_rand rnd(seed);
  std::string bytes(size, '\0');
  for (char& c : bytes)
    c = static_cast<char>(rnd());
  return bytes;
}

// One record of a bundle: [sequence_index][packet_size][packet].
std::string Record(uint32_t sequence_index, const std::string& packet) {
  uint8_t header[20];
  uint8_t* end = protozero::proto_utils::WriteVarInt(sequence_index, header);
  end = protozero::proto_utils::WriteVarInt(packet.size(), end);
  return std::string(reinterpret_cast<const char*>(header),
                     static_cast<size_t>(end - header)) +
         packet;
}

// A packet as the consumer gets it.
struct TestPacket {
  std::string str;
  ProducerID producer_id = 0;
  WriterID writer_id = 0;
  uid_t uid = 0;

  // previous_packet_dropped in the packet bytes, and the value that
  // ReadNextTracePacket() returned for the service to append.
  uint32_t loss_in_packet = 0;
  uint32_t loss_from_buffer = 0;

  // What a trace parser sees. The service appends |loss_from_buffer| if it
  // is not zero, and the parser keeps the last value of the field.
  uint32_t loss() const {
    return loss_from_buffer ? loss_from_buffer : loss_in_packet;
  }
};

std::vector<std::string> Strs(const std::vector<TestPacket>& packets) {
  std::vector<std::string> strs;
  for (const TestPacket& packet : packets)
    strs.push_back(packet.str);
  return strs;
}

// Reads one packet. If |raw| is not null it gets the TracePacket, whose slices
// point into the buffer.
bool ReadOne(TraceBuffer* buf, TestPacket* out, TracePacket* raw = nullptr) {
  TracePacket packet;
  TraceBuffer::PacketSequenceProperties properties{};
  uint32_t loss_from_buffer = 0;
  if (!buf->ReadNextTracePacket(&packet, &properties, &loss_from_buffer))
    return false;
  for (const Slice& slice : packet.slices())
    EXPECT_LE(slice.size, TraceBuffer::kMaxTracePacketSliceSize);

  protos::gen::TracePacket proto;
  EXPECT_TRUE(proto.ParseFromString(packet.GetRawBytesForTesting()));
  *out = TestPacket();
  out->str = proto.for_testing().has_protovm_patch()
                 ? proto.for_testing().protovm_patch().string_to_merge()
                 : proto.for_testing().str();
  out->producer_id = properties.producer_id_trusted;
  out->writer_id = properties.writer_id;
  out->uid = properties.producer_uid_trusted();
  out->loss_in_packet = proto.previous_packet_dropped();
  out->loss_from_buffer = loss_from_buffer;
  if (raw)
    *raw = std::move(packet);
  return true;
}

// Reads all the packets that the buffer has, in one read batch.
std::vector<TestPacket> ReadAll(TraceBuffer* buf) {
  std::vector<TestPacket> packets;
  buf->BeginRead();
  for (TestPacket packet; ReadOne(buf, &packet);)
    packets.push_back(std::move(packet));
  return packets;
}

}  // namespace

class TraceBufferV3Test : public testing::Test {
 public:
  using Params = TraceBufferV3::Params;
  using BundleHeader = TraceBufferV3::BundleHeader;

  static Params DefaultParams(size_t size) {
    return TraceBufferV3::DefaultParams(size);
  }

  // Every packet becomes its own bundle, and every EndWriteBatch() after a
  // chunk starts a pass. The tests of the bundled buffer use this to place
  // bundles one at a time.
  static Params OneBundlePerPacket() {
    Params params{};
    params.staging_bytes = 4096;
    params.bundle_target_bytes = 1;
    params.decode_block_bytes = 4096;
    return params;
  }

  // Sets the target above the amount the tests write, so packets stay in
  // staging until a read or an explicit BundleNow().
  static Params BundleOnlyOnRead() {
    Params params{};
    params.staging_bytes = 8192;
    params.bundle_target_bytes = 1024 * 1024;
    params.decode_block_bytes = 4096;
    return params;
  }

  void ResetBuffer(
      size_t size,
      const Params& params,
      TraceBuffer::OverwritePolicy policy = TraceBuffer::kOverwrite) {
    trace_buffer_ = TraceBufferV3::CreateInternal(size, policy, params);
    ASSERT_TRUE(trace_buffer_);
  }

  FakeChunk CreateChunk(ProducerID p, WriterID w, ChunkID c) {
    FakeChunk chunk(trace_buffer_.get(), p, w, c);
    chunk.SetUID(1000 + p);
    return chunk;
  }

  // Copies one complete chunk that holds |packets|. Returns the size that
  // the chunk takes in staging.
  size_t WriteChunk(ProducerID p,
                    WriterID w,
                    ChunkID c,
                    const std::vector<std::string>& packets) {
    FakeChunk chunk = CreateChunk(p, w, c);
    for (const std::string& packet : packets)
      chunk.AddFragment(packet);
    return base::AlignUp(chunk.CopyIntoTraceBuffer(), 16);
  }

  // Writes |packet| as one write batch for sequence {1, 1}. The ChunkIDs
  // follow each other, so the sequence has no gap.
  void WriteBatch(const std::string& packet) {
    WriteChunk(1, 1, next_chunk_id_++, {packet});
    trace_buffer_->EndWriteBatch();
  }

  // Runs the EndWriteBatch() bundling pass regardless of the target. With
  // BundleOnlyOnRead() a test then decides which packets share a bundle.
  void BundleNow() { trace_buffer_->BundleAllFromStaging(); }

  // Writes the first 1000 bytes of a packet with 1100 'a's as chunk 0 of
  // {1, |writer|}. The chunk takes 1 KiB in staging and stays there until
  // WriteEndOfSplitPacket() writes the rest of the packet as chunk 1.
  void WriteStartOfSplitPacket(WriterID writer) {
    const std::string packet = Packet(std::string(1100, 'a'));
    ASSERT_EQ(CreateChunk(1, writer, 0)
                  .AddFragment(packet.substr(0, 1000), kContOnNextChunk)
                  .CopyIntoTraceBuffer(),
              1018u);
  }
  void WriteEndOfSplitPacket(WriterID writer) {
    const std::string packet = Packet(std::string(1100, 'a'));
    CreateChunk(1, writer, 1)
        .AddFragment(packet.substr(1000), kContFromPrevChunk)
        .CopyIntoTraceBuffer();
  }

  bool CopyChunkV2(ProducerID p,
                   WriterID w,
                   const std::vector<std::string>& fragments,
                   bool first_continues_from_prev = false,
                   bool last_continues_on_next = false) {
    std::vector<protozero::ConstBytes> bytes;
    for (const std::string& fragment : fragments) {
      bytes.push_back(
          {reinterpret_cast<const uint8_t*>(fragment.data()), fragment.size()});
    }
    return trace_buffer_->CopyChunkV2Untrusted(
        {p, ClientIdentity(1000 + p, 2000 + p), w}, bytes.data(), bytes.size(),
        first_continues_from_prev, last_continues_on_next);
  }

  std::vector<TestPacket> ReadAll() {
    return perfetto::ReadAll(trace_buffer_.get());
  }

  // The stored size of the first bundle that a buffer with |params| builds
  // from one chunk with |packets|. Tests use it to choose a capacity that
  // puts a bundle at an exact offset. The size depends on what zstd makes of
  // the packets, so no test hardcodes it.
  static size_t BundleSize(const Params& params,
                           const std::vector<std::string>& packets) {
    auto probe = TraceBufferV3::CreateInternal(1024 * 1024,
                                               TraceBuffer::kOverwrite, params);
    FakeChunk chunk(probe.get(), 1, 1, 0);
    chunk.SetUID(1001);
    for (const std::string& packet : packets)
      chunk.AddFragment(packet);
    chunk.CopyIntoTraceBuffer();
    probe->BundleOneFromStaging();
    return probe->used_size();
  }

  // Appends a bundle with the given records to the bundled buffer, without
  // staging. The sequences must already be in a stored bundle.
  // |uncompressed_size| and |frame| replace the real values if set, to make a
  // bundle that the reader must reject.
  void AppendRawBundle(const std::vector<ProducerAndWriterID>& ids,
                       const std::string& records,
                       std::optional<uint32_t> uncompressed_size = std::nullopt,
                       std::optional<std::string> frame = std::nullopt) {
    if (!frame) {
      frame = std::string(ZSTD_compressBound(records.size()), '\0');
      const size_t frame_size = ZSTD_compress(
          frame->data(), frame->size(), records.data(), records.size(), 1);
      ASSERT_FALSE(ZSTD_isError(frame_size));
      frame->resize(frame_size);
    }
    BundleHeader header{};
    header.bundle_size = static_cast<uint32_t>(
        sizeof(header) + ids.size() * sizeof(ProducerAndWriterID) +
        frame->size());
    header.uncompressed_size =
        uncompressed_size.value_or(static_cast<uint32_t>(records.size()));
    header.num_sequences = static_cast<uint32_t>(ids.size());
    ASSERT_LE(header.bundle_size, bundled().free_size());
    bundled().Append(&header, sizeof(header));
    bundled().Append(ids.data(), ids.size() * sizeof(ProducerAndWriterID));
    bundled().Append(frame->data(), frame->size());
    for (ProducerAndWriterID id : ids)
      ++trace_buffer_->sequences_.at(id).bundle_refs;
    // Lets a header that is wrong by a few bytes reach the decoder.
    trace_buffer_->max_bundle_uncompressed_size_ =
        std::max(trace_buffer_->max_bundle_uncompressed_size_,
                 static_cast<uint32_t>(records.size() + 16));
  }

  // Returns the string and integer last merged by the ProtoVM of |buf|.
  static protos::gen::TestEvent::ProtoVmIncrementalState ProtoVmState(
      TraceBuffer* buf) {
    const auto& instances = buf->protovms()->instances();
    EXPECT_EQ(instances.size(), 1u);
    protozero::HeapBuffered<protos::pbzero::TracePacket> state;
    instances[0].instance->SerializeIncrementalState(state.get());
    protos::gen::TracePacket proto;
    EXPECT_TRUE(proto.ParseFromString(state.SerializeAsString()));
    return proto.for_testing().protovm_incremental_state();
  }

  BundledBuffer& bundled() { return trace_buffer_->bundled_buf_; }
  TraceBufferV2* staging() { return trace_buffer_->staging_buf_.get(); }
  void SuppressClientDchecksForTesting() {
    staging()->suppress_client_dchecks_for_testing_ = true;
  }
  bool has_pending_bundle() { return !!trace_buffer_->pending_bundle_; }
  size_t num_bundled_sequences() { return trace_buffer_->sequences_.size(); }
  size_t num_staging_sequences() { return staging()->sequences_.size(); }
  size_t num_empty_staging_sequences() { return staging()->empty_sequences_; }

  BundleHeader OldestHeader() {
    std::vector<ProducerAndWriterID> ids;
    return trace_buffer_->ReadOldestBundleMetadata(&ids);
  }

  // The bytes of the temporary allocations of DecodeArena.
  size_t decode_temporary_bytes() {
    return trace_buffer_->decode_arena_.GetMemoryUsageBytes() -
           2 * trace_buffer_->params_.decode_block_bytes;
  }

  const TraceStats::BufferStats& stats() { return trace_buffer_->stats(); }

 protected:
  std::unique_ptr<TraceBufferV3> trace_buffer_;
  ChunkID next_chunk_id_ = 0;
};

// ---------------------------------------------------------------------------
// BundledBuffer and DecodeArena
// ---------------------------------------------------------------------------

// The buffer is a byte ring. Data that reaches the end of the memory
// continues at offset 0, for writes and for reads.
TEST(BundledBufferTest, AppendAndReadAcrossTheEnd) {
  BundledBuffer buf;
  ASSERT_TRUE(buf.Initialize(10));
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.free_size(), 10u);

  buf.Append("abcdefgh", 8);
  EXPECT_EQ(buf.used_size(), 8u);
  EXPECT_EQ(buf.free_size(), 2u);
  buf.Consume(6);

  //   0                   10
  //   | 4 5 . . . . g h 2 3 |      "2345" continues at offset 0.
  //         ^ wr_   ^ rd_
  buf.Append("2345", 4);
  EXPECT_EQ(buf.used_size(), 6u);

  char out[7] = {};
  buf.CopyOut(0, 6, out);
  EXPECT_STREQ(out, "gh2345");

  // "gh23" ends at the end of the memory. "2345" does not.
  EXPECT_NE(buf.PeekContiguous(0, 4), nullptr);
  EXPECT_EQ(buf.PeekContiguous(2, 4), nullptr);
  ASSERT_NE(buf.PeekContiguous(4, 2), nullptr);
  EXPECT_EQ(memcmp(buf.PeekContiguous(4, 2), "45", 2), 0);

  // The whole capacity is usable.
  buf.Append("6789", 4);
  EXPECT_EQ(buf.free_size(), 0u);
  char all[11] = {};
  buf.CopyOut(0, 10, all);
  EXPECT_STREQ(all, "gh23456789");

  // An empty buffer starts again at offset 0.
  buf.Consume(10);
  EXPECT_TRUE(buf.empty());
  buf.Append("0123456789", 10);
  ASSERT_NE(buf.PeekContiguous(0, 10), nullptr);
  buf.Consume(10);
  buf.ReleaseUnusedPages();
  buf.Append("ab", 2);
  char two[3] = {};
  buf.CopyOut(0, 2, two);
  EXPECT_STREQ(two, "ab");
}

// A clone has the stored bytes, in order, and the capacity of its source.
TEST(BundledBufferTest, CloneCopiesStoredBytes) {
  BundledBuffer src;
  ASSERT_TRUE(src.Initialize(8192));
  std::string filler(8190, 'x');
  src.Append(filler.data(), filler.size());
  src.Consume(filler.size() - 2);
  src.Append("0123456789", 10);  // Stored: "xx0123456789", wrapped.
  ASSERT_EQ(src.PeekContiguous(0, 12), nullptr);

  BundledBuffer clone;
  ASSERT_TRUE(clone.InitializeAsCloneOf(src));
  EXPECT_EQ(clone.capacity(), 8192u);
  EXPECT_EQ(clone.used_size(), 12u);
  char out[13] = {};
  clone.CopyOut(0, 12, out);
  EXPECT_STREQ(out, "xx0123456789");

  // The source can change. The clone has its own memory.
  src.Consume(12);
  src.Append("zzzzzzzzzzzz", 12);
  clone.CopyOut(0, 12, out);
  EXPECT_STREQ(out, "xx0123456789");

  // The clone can store as much as its source.
  clone.Consume(12);
  std::string big(8192, 'y');
  clone.Append(big.data(), big.size());
  EXPECT_EQ(clone.used_size(), 8192u);
  EXPECT_EQ(clone.free_size(), 0u);

  // A clone of an empty buffer is empty and can store bytes.
  BundledBuffer empty_src;
  ASSERT_TRUE(empty_src.Initialize(4096));
  BundledBuffer empty_clone;
  ASSERT_TRUE(empty_clone.InitializeAsCloneOf(empty_src));
  EXPECT_TRUE(empty_clone.empty());
  EXPECT_EQ(empty_clone.capacity(), 4096u);
  empty_clone.Append("ab", 2);
  EXPECT_EQ(empty_clone.used_size(), 2u);
}

// Allocations keep their address until ResetExcept(). A request that fits
// in neither block gets a temporary allocation, which ResetExcept() frees.
TEST(DecodeArenaTest, BlocksAreReusedAndTemporariesFreed) {
  DecodeArena arena(100);
  const size_t two_blocks = 200;
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);

  uint8_t* a = arena.Allocate(60);  // Block 0.
  uint8_t* b = arena.Allocate(60);  // Block 1. It does not fit after a.
  uint8_t* c = arena.Allocate(40);  // Block 0, after a.
  EXPECT_EQ(c, a + 60);
  EXPECT_NE(b, a);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);

  uint8_t* d = arena.Allocate(60);   // Temporary. No block has room.
  uint8_t* e = arena.Allocate(500);  // Temporary. Larger than a block.
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks + 60 + 500);
  memset(a, 'a', 60);
  memset(b, 'b', 60);
  memset(c, 'c', 40);
  memset(d, 'd', 60);
  memset(e, 'e', 500);
  EXPECT_EQ(a[59], 'a');
  EXPECT_EQ(c[0], 'c');

  // Keep |c|. Its block stays as it is, the other block is empty again and
  // the temporaries are gone.
  arena.ResetExcept(c);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);
  EXPECT_EQ(c[39], 'c');
  EXPECT_EQ(arena.Allocate(100), b);

  // Keep a temporary allocation. Both blocks are empty again.
  uint8_t* f = arena.Allocate(300);
  arena.ResetExcept(f);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks + 300);
  EXPECT_EQ(arena.Allocate(100), a);

  arena.ResetExcept(nullptr);
  arena.ReleaseEmptyBlocks();
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);
  EXPECT_NE(arena.Allocate(10), nullptr);
}

// ---------------------------------------------------------------------------
// Input formats and the write batch
// ---------------------------------------------------------------------------

TEST_F(TraceBufferV3Test, EmptyBuffer) {
  ResetBuffer(4096, OneBundlePerPacket());
  EXPECT_THAT(ReadAll(), IsEmpty());
  trace_buffer_->EndWriteBatch();
  trace_buffer_->MaybeCompact();
  EXPECT_THAT(ReadAll(), IsEmpty());
  EXPECT_FALSE(trace_buffer_->has_data());
  EXPECT_EQ(trace_buffer_->size(), 4096u);
  EXPECT_EQ(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(trace_buffer_->buf_type(), TraceBuffer::kV3);
  EXPECT_EQ(stats().buffer_size(), 4096u);
  EXPECT_EQ(stats().bundles_written(), 0u);
}

// Packets below the target stay in staging. A read bundles them and
// returns them with the identity of their writer.
TEST_F(TraceBufferV3Test, ReadBundlesWhatIsLeftInStaging) {
  ResetBuffer(4096, BundleOnlyOnRead());
  WriteChunk(1, 1, 0, {Packet("a"), Packet("b")});
  WriteChunk(2, 3, 0, {Packet("c")});
  trace_buffer_->EndWriteBatch();
  EXPECT_TRUE(trace_buffer_->has_data());
  EXPECT_EQ(stats().bundles_written(), 0u);
  EXPECT_EQ(trace_buffer_->used_size(), 0u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "b", "c"));
  EXPECT_EQ(packets[0].producer_id, 1u);
  EXPECT_EQ(packets[0].writer_id, 1u);
  EXPECT_EQ(packets[0].uid, 1001u);
  EXPECT_EQ(packets[2].producer_id, 2u);
  EXPECT_EQ(packets[2].writer_id, 3u);
  EXPECT_EQ(packets[2].uid, 1002u);
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss(), 0u);

  // One bundle went through the bundled buffer and nothing stays behind.
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(num_bundled_sequences(), 0u);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// EndWriteBatch() bundles once enough chunk bytes arrived. The packets then are
// in the bundled buffer and staging is empty.
TEST_F(TraceBufferV3Test, EndWriteBatchBundlesAfterEnoughStagingBytes) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 128;
  ResetBuffer(4096, params);

  // Each chunk takes 64 bytes in staging, 16 + 1 + 45 rounded up.
  ASSERT_EQ(WriteChunk(1, 1, 0, {Packet(std::string(40, 'a'))}), 64u);
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 0u);
  EXPECT_EQ(staging()->used_size(), 64u);

  ASSERT_EQ(WriteChunk(1, 1, 1, {Packet(std::string(40, 'b'))}), 64u);
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_GT(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(staging()->used_size(), 0u);
  EXPECT_EQ(num_bundled_sequences(), 1u);

  // Both records, 47 bytes each, are in one bundle below the 128 byte target.
  EXPECT_EQ(OldestHeader().uncompressed_size, 94u);
  EXPECT_EQ(OldestHeader().num_sequences, 1u);
  EXPECT_EQ(OldestHeader().bundle_size, trace_buffer_->used_size());

  // The counter restarts after a pass.
  WriteChunk(1, 1, 2, {Packet(std::string(40, 'c'))});
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 1u);

  EXPECT_THAT(Strs(ReadAll()),
              ElementsAre(std::string(40, 'a'), std::string(40, 'b'),
                          std::string(40, 'c')));
}

// A scrape copies chunk 2 (packet '7') before chunk 1 (packet '6'). Chunk 2
// crosses the target, but nothing is read until EndWriteBatch(), so both chunks
// are bundled in ChunkID order without loss.
TEST_F(TraceBufferV3Test, NoReadBeforeEndWriteBatch) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 64;
  ResetBuffer(4096, params);

  WriteBatch(Packet(std::string(40, '5')));  // Chunk 0.
  ASSERT_EQ(stats().bundles_written(), 1u);
  ASSERT_EQ(stats().staging_chunks_read(), 1u);

  WriteChunk(1, 1, 2, {Packet(std::string(40, '7'))});
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(stats().staging_chunks_read(), 1u);
  WriteChunk(1, 1, 1, {Packet(std::string(40, '6'))});
  EXPECT_EQ(stats().staging_chunks_read(), 1u);

  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().staging_chunks_read(), 3u);
  EXPECT_EQ(stats().chunks_committed_out_of_order(), 1u);
  EXPECT_EQ(stats().chunks_discarded(), 0u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, '5'), std::string(40, '6'),
                          std::string(40, '7')));
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss(), 0u);
}

// A packet split over two chunks waits in staging until its first chunk is
// patched. Packets before it and packets from other writers can be bundled.
TEST_F(TraceBufferV3Test, FragmentsWaitForPatches) {
  ResetBuffer(4096, OneBundlePerPacket());
  const std::string split = Packet("XXXX-and-the-rest");
  const size_t cut = 10;

  // Chunk 0: [a] [first part of the split packet], which needs a patch.
  // Chunk 1: [second part] [b].
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("a"))
      .AddFragment(split.substr(0, cut), kContOnNextChunk | kChunkNeedsPatching)
      .CopyIntoTraceBuffer();
  CreateChunk(1, 1, 1)
      .AddFragment(split.substr(cut), kContFromPrevChunk)
      .AddFragment(Packet("b"))
      .CopyIntoTraceBuffer();
  // Another writer is not held back.
  WriteChunk(1, 2, 0, {Packet("other")});
  trace_buffer_->EndWriteBatch();
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("a", "other"));
  EXPECT_EQ(stats().staging_readaheads_failed(), 2u);
  EXPECT_EQ(stats().readaheads_failed(), 0u);

  // The patch arrives in a later batch. It replaces "XXXX" in the string.
  // The fragment starts after the 7 bytes of [a], its own size byte and the
  // 5 bytes that precede the string.
  ASSERT_TRUE(trace_buffer_->TryPatchChunkContents(
      1, 1, 0,
      std::vector<TraceBuffer::Patch>{{7 + 1 + 5, {'r', 'e', 'a', 'd'}}}.data(),
      1, /*other_patches_pending=*/false));
  trace_buffer_->EndWriteBatch();
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("read-and-the-rest", "b"));
  EXPECT_EQ(stats().staging_readaheads_succeeded(), 1u);
  EXPECT_EQ(stats().patches_succeeded(), 1u);
}

// Chunks and their patches arrive in one CommitData. EndWriteBatch() comes
// after both, so the patched packet is in the bundle of that batch.
TEST_F(TraceBufferV3Test, EndWriteBatchAfterChunksAndPatches) {
  ResetBuffer(4096, OneBundlePerPacket());
  const std::string split = Packet("XXXX-patched");
  CreateChunk(1, 1, 0)
      .AddFragment(split.substr(0, 9), kContOnNextChunk | kChunkNeedsPatching)
      .CopyIntoTraceBuffer();
  CreateChunk(1, 1, 1)
      .AddFragment(split.substr(9), kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  ASSERT_TRUE(trace_buffer_->TryPatchChunkContents(
      1, 1, 0,
      std::vector<TraceBuffer::Patch>{{1 + 5, {'d', 'o', 'n', 'e'}}}.data(), 1,
      /*other_patches_pending=*/false));
  EXPECT_EQ(stats().bundles_written(), 0u);
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(staging()->used_size(), 0u);
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("done-patched"));
}

// A scraped chunk that the writer has not finished gives its complete
// packets. The rest follows when the producer commits the chunk.
TEST_F(TraceBufferV3Test, IncompleteScrapedChunk) {
  ResetBuffer(4096, OneBundlePerPacket());
  // The writer is still on the third packet, so the scrape takes two.
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("a"))
      .AddFragment(Packet("b"))
      .AddFragment(Packet("unfinished"))
      .PadTo(256)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer_->EndWriteBatch();
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("a", "b"));

  CreateChunk(1, 1, 0)
      .AddFragment(Packet("a"))
      .AddFragment(Packet("b"))
      .AddFragment(Packet("c"))
      .CopyIntoTraceBuffer();
  trace_buffer_->EndWriteBatch();
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("c"));
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(stats().chunks_discarded(), 0u);
}

// The scrape copies complete chunks 5 and 6 before their CommitData arrives.
// EndWriteBatch() bundles them, which erases the copies in staging. Staging
// then drops the late commits and counts them in chunks_discarded, as a
// standalone TraceBufferV2 does after a read. The packets are returned once and
// without loss.
TEST_F(TraceBufferV3Test, LateCommitAfterScrapeAndBundling) {
  ResetBuffer(4096, OneBundlePerPacket());
  // TraceBufferV2 has a DCHECK for a chunk that is not newer than the last
  // one it read.
  SuppressClientDchecksForTesting();
  WriteChunk(1, 1, 5, {Packet("P")});
  WriteChunk(1, 1, 6, {Packet("Q")});
  trace_buffer_->EndWriteBatch();
  ASSERT_EQ(stats().bundles_written(), 2u);
  const uint64_t bytes_written = stats().bytes_written();

  WriteChunk(1, 1, 5, {Packet("P")});
  WriteChunk(1, 1, 6, {Packet("Q")});
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().chunks_discarded(), 2u);
  EXPECT_EQ(stats().chunks_written(), 2u);
  EXPECT_EQ(stats().bytes_written(), bytes_written);
  EXPECT_EQ(stats().bundles_written(), 2u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("P", "Q"));
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[1].loss(), 0u);

  // The sequence continues without a gap.
  WriteChunk(1, 1, 7, {Packet("R")});
  trace_buffer_->EndWriteBatch();
  packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("R"));
  EXPECT_EQ(packets[0].loss(), 0u);
}

// SMB v2 chunks go through staging too. A read returns ordinary protobuf.
TEST_F(TraceBufferV3Test, SmbV2Chunks) {
  ResetBuffer(4096, OneBundlePerPacket());
  ASSERT_TRUE(trace_buffer_->supports_tracing_v2());
  ASSERT_TRUE(CopyChunkV2(1, 1, {PacketV2("whole")}));
  // A packet in two fragments, one per batch of the writer.
  const std::string split = PacketV2("in-two-fragments");
  ASSERT_TRUE(CopyChunkV2(1, 1, {split.substr(0, 8)}, false, true));
  ASSERT_TRUE(CopyChunkV2(1, 1, {split.substr(8)}, true, false));
  // An SMB v1 writer of the same producer.
  WriteChunk(1, 2, 0, {Packet("v1")});
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 3u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("whole", "in-two-fragments", "v1"));
  EXPECT_EQ(packets[0].producer_id, 1u);
  EXPECT_EQ(packets[0].writer_id, 1u);
  EXPECT_EQ(packets[0].uid, 1001u);

  // A gap in the SMB v2 sequence is reported on the packet after it.
  trace_buffer_->RecordChunkV2DataLoss(1, 1);
  ASSERT_TRUE(CopyChunkV2(1, 1, {PacketV2("after-gap")}));
  trace_buffer_->EndWriteBatch();
  packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("after-gap"));
  EXPECT_EQ(packets[0].loss_in_packet, kLossPresent | kLossReadGap);
  EXPECT_EQ(packets[0].loss_from_buffer, 0u);

  trace_buffer_->RecordAbiViolation();
  EXPECT_EQ(stats().abi_violations(), 1u);
}

// ---------------------------------------------------------------------------
// Bundle size
// ---------------------------------------------------------------------------

// The packet that reaches or crosses the target stays whole in the bundle
// that it ends. One pass builds as many bundles as staging has packets for.
TEST_F(TraceBufferV3Test, PacketThatCrossesTheTargetStaysWhole) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 100;
  ResetBuffer(4096, params);

  // Each record takes 47 bytes, 2 for its header and 45 for the packet. The
  // third one crosses 100, so the first bundle has three records. The second
  // bundle gets the other two and ends because staging has no more packets.
  std::vector<std::string> packets;
  for (char c : {'a', 'b', 'c', 'd', 'e'})
    packets.push_back(Packet(std::string(40, c)));
  WriteChunk(1, 1, 0, packets);
  trace_buffer_->EndWriteBatch();

  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_EQ(stats().bundle_bytes_uncompressed(), 5 * 47u);
  EXPECT_EQ(stats().bundle_bytes_compressed(), trace_buffer_->used_size());
  EXPECT_EQ(OldestHeader().uncompressed_size, 3 * 47u);

  trace_buffer_->BeginRead();
  TestPacket packet;
  for (int i = 0; i < 3; i++)
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, std::string(40, 'c'));
  EXPECT_EQ(OldestHeader().uncompressed_size, 2 * 47u);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, std::string(40, 'e'));
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(stats().bytes_read(), 5 * 47u);
}

// Records that add up to exactly the target end the bundle.
TEST_F(TraceBufferV3Test, ExactTarget) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 94;
  ResetBuffer(4096, params);
  std::vector<std::string> packets;
  for (char c : {'a', 'b', 'c', 'd'})
    packets.push_back(Packet(std::string(40, c)));
  WriteChunk(1, 1, 0, packets);
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_EQ(OldestHeader().uncompressed_size, 94u);
  EXPECT_EQ(Strs(ReadAll()).size(), 4u);
}

// One bundle with packets from a few bytes to several kilobytes.
// ZstdFrameCodec gathers small inputs in a 4096-byte buffer before it passes
// them to zstd, so the sizes around 4096 must come back unchanged too.
TEST_F(TraceBufferV3Test, PacketsOfManySizesInOneBundle) {
  Params params = BundleOnlyOnRead();
  params.staging_bytes = 1024 * 1024;
  ResetBuffer(1024 * 1024, params);

  std::vector<size_t> sizes = {0, 1, 2, 100, 127, 128, 2000, 4000};
  for (size_t size = 4070; size <= 4110; size++)
    sizes.push_back(size);
  for (size_t size : {5000u, 8191u, 8192u, 8193u, 12000u, 40000u, 3u, 4096u})
    sizes.push_back(size);

  std::vector<std::string> expected;
  for (size_t i = 0; i < sizes.size(); i++) {
    expected.push_back(RandomBytes(sizes[i], static_cast<uint32_t>(i + 1)));
    WriteChunk(1, 1, static_cast<ChunkID>(i), {Packet(expected.back())});
  }
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 0u);

  EXPECT_EQ(Strs(ReadAll()), expected);
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(stats().bundles_corrupted(), 0u);
}

// A sequence that waits for a fragment makes no progress. A pass that finds
// nothing to read still resets the counter, so EndWriteBatch() does not start a
// pass again until enough new chunks arrive.
TEST_F(TraceBufferV3Test, BlockedStagingDoesNotCausePassPerEndWriteBatch) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 64;
  ResetBuffer(4096, params);

  // The first fragment of a packet whose second fragment never arrives.
  ASSERT_EQ(CreateChunk(1, 1, 0)
                .AddFragment(std::string(40, 'x'), kContOnNextChunk)
                .CopyIntoTraceBuffer(),
            57u);  // Takes 64 bytes in staging.
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().staging_readaheads_failed(), 1u);
  EXPECT_EQ(stats().bundles_written(), 0u);

  // No new chunks, so no pass.
  trace_buffer_->EndWriteBatch();
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().staging_readaheads_failed(), 1u);

  // The next 64 bytes start a pass. The other writer is bundled and the
  // blocked chunk stays.
  WriteChunk(1, 2, 0, {Packet(std::string(40, 'b'))});
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().staging_readaheads_failed(), 2u);
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(staging()->used_size(), 64u);
  EXPECT_THAT(Strs(ReadAll()), ElementsAre(std::string(40, 'b')));
}

// ---------------------------------------------------------------------------
// The bundled buffer: wrapping, overwrites and capacity
// ---------------------------------------------------------------------------

// Three bundles A, B, C of the same sequence in a buffer that holds two.
// C overwrites A and starts |free_at_end| bytes before the end of the memory.
//
//   0                                              capacity
//   | A              | B              | free_at_end |
//   | C end ...      | B              | C start     |
//
// free_at_end   C continues at offset 0 in the middle of
//   6           its 12 byte header
//   14          its 4 byte list of sequences
//   20          its zstd frame
//   0           nothing: C fits exactly where A was
class TraceBufferV3WrapTest : public TraceBufferV3Test,
                              public testing::WithParamInterface<size_t> {};

TEST_P(TraceBufferV3WrapTest, BundleContinuesAtStartOfMemory) {
  const size_t free_at_end = GetParam();
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  const std::string b = Packet(std::string(40, 'b'));
  const std::string c = Packet(std::string(40, 'c'));
  const size_t size_a = BundleSize(params, {a});
  const size_t size_b = BundleSize(params, {b});
  const size_t size_c = BundleSize(params, {c});
  ASSERT_GT(size_c, 20u);
  ASSERT_LE(size_c, size_a + free_at_end);

  ResetBuffer(size_a + size_b + free_at_end, params);
  WriteBatch(a);
  WriteBatch(b);
  EXPECT_EQ(trace_buffer_->used_size(), size_a + size_b);
  EXPECT_EQ(stats().bundles_overwritten(), 0u);

  // Only A makes room. Removing more than that would hide a wrap bug behind
  // a second overwrite.
  WriteBatch(c);
  EXPECT_EQ(stats().bundles_overwritten(), 1u);
  EXPECT_EQ(trace_buffer_->used_size(), size_b + size_c);
  EXPECT_EQ(stats().bundles_written(), 3u);
  EXPECT_EQ(stats().bundle_bytes_compressed(), size_a + size_b + size_c);

  // B reports the loss of A, once.
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, 'b'), std::string(40, 'c')));
  EXPECT_EQ(packets[0].loss_from_buffer, kLossPresent | kLossOverwrite);
  EXPECT_EQ(packets[1].loss(), 0u);
  EXPECT_EQ(trace_buffer_->used_size(), 0u);

  // The whole capacity is usable again.
  WriteBatch(a);
  WriteBatch(b);
  EXPECT_EQ(trace_buffer_->used_size(), size_a + size_b);
  EXPECT_EQ(stats().bundles_overwritten(), 1u);
  EXPECT_EQ(Strs(ReadAll()).size(), 2u);
}

INSTANTIATE_TEST_SUITE_P(FreeAtEnd,
                         TraceBufferV3WrapTest,
                         testing::Values(6u, 14u, 20u, 0u));

// Bundles of different sizes move the wrap point through different parts of
// their headers and payloads in a small buffer. The consumer reads
// now and then, and must get every packet that was not overwritten, in order.
TEST_F(TraceBufferV3Test, ManyWrapsKeepPacketsIntact) {
  ResetBuffer(331, OneBundlePerPacket());
  std::minstd_rand rnd(7);
  uint32_t next_to_write = 0;
  uint32_t next_to_read = 0;
  uint32_t num_lost = 0;

  // Packet |index| is "<index>-<random bytes>". The bytes come from |index|,
  // so the reader can tell that it got the whole packet.
  auto read_and_check = [&] {
    for (const TestPacket& packet : ReadAll()) {
      const std::optional<uint32_t> maybe_index =
          base::StringToUInt32(packet.str.substr(0, packet.str.find('-')));
      ASSERT_TRUE(maybe_index.has_value());
      const uint32_t index = *maybe_index;
      const std::string prefix = std::to_string(index) + "-";
      ASSERT_GE(index, next_to_read);
      ASSERT_EQ(packet.str,
                prefix + RandomBytes(packet.str.size() - prefix.size(), index));
      // A packet reports loss if and only if packets before it are missing.
      EXPECT_EQ(packet.loss() != 0, index != next_to_read) << index;
      num_lost += index - next_to_read;
      next_to_read = index + 1;
    }
  };

  for (int round = 0; round < 400; round++) {
    const uint32_t num_writes = 1 + rnd() % 4;
    for (uint32_t i = 0; i < num_writes; i++) {
      const uint32_t index = next_to_write++;
      const size_t num_random_bytes = rnd() % 60;
      WriteBatch(Packet(std::to_string(index) + "-" +
                        RandomBytes(num_random_bytes, index)));
    }
    EXPECT_LE(trace_buffer_->used_size(), 331u);
    if (rnd() % 3 == 0)
      read_and_check();
  }
  read_and_check();
  EXPECT_EQ(next_to_read, next_to_write);
  EXPECT_GT(num_lost, 100u);
  EXPECT_EQ(stats().bundles_overwritten(), num_lost);
  EXPECT_EQ(stats().bundles_too_large(), 0u);
  EXPECT_EQ(stats().bundles_corrupted(), 0u);
}

// A bundle that needs the room of several older bundles overwrites exactly
// that many.
TEST_F(TraceBufferV3Test, OverwritesSeveralBundles) {
  const Params params = OneBundlePerPacket();
  const std::string small = Packet(std::string(40, 's'));
  const std::string big = Packet(RandomBytes(120, 1));
  const size_t small_size = BundleSize(params, {small});
  const size_t big_size = BundleSize(params, {big});
  // The number of small bundles that |big| needs the room of.
  const size_t num_overwritten = (big_size + small_size - 1) / small_size;
  ASSERT_GE(num_overwritten, 2u);

  ResetBuffer((num_overwritten + 1) * small_size, params);
  for (size_t i = 0; i < num_overwritten + 1; i++)
    WriteBatch(small);
  EXPECT_EQ(bundled().free_size(), 0u);

  WriteBatch(big);
  EXPECT_EQ(stats().bundles_overwritten(), num_overwritten);
  EXPECT_EQ(trace_buffer_->used_size(), small_size + big_size);
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, std::string(40, 's'));
  EXPECT_EQ(packets[0].loss(), kLossPresent | kLossOverwrite);
  EXPECT_EQ(packets[1].str, RandomBytes(120, 1));
  EXPECT_EQ(packets[1].loss(), 0u);
}

// A bundle larger than the whole bundled buffer is dropped. The older bundles
// stay because overwriting them would not make the new one fit.
// The loss goes on the next packet that leaves staging, P4. It must not go
// on P1, which is older than the dropped packets.
TEST_F(TraceBufferV3Test, BundleLargerThanTheBuffer) {
  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    const Params params = OneBundlePerPacket();
    const std::string p1 = Packet(std::string(40, '1'));
    const std::string p4 = Packet(std::string(40, '4'));
    const size_t small_size = BundleSize(params, {p1});
    const size_t capacity = 3 * small_size;
    // The producer flagged loss on P2 itself. Dropping P2 must not hide it.
    const std::string p2 =
        Packet(RandomBytes(capacity, 2), kLossPresent | kLossSmbFull);
    ASSERT_GT(BundleSize(params, {p2}), capacity);

    next_chunk_id_ = 0;
    ResetBuffer(capacity, params, policy);
    WriteBatch(p1);
    WriteBatch(p2);
    EXPECT_EQ(stats().bundles_too_large(), 1u);
    EXPECT_EQ(stats().bundles_overwritten(), 0u);
    EXPECT_EQ(stats().bundles_written(), 1u);
    EXPECT_EQ(trace_buffer_->used_size(), small_size);
    EXPECT_FALSE(has_pending_bundle());
    EXPECT_EQ(num_bundled_sequences(), 1u);

    WriteBatch(p4);
    std::vector<TestPacket> packets = ReadAll();
    ASSERT_THAT(Strs(packets),
                ElementsAre(std::string(40, '1'), std::string(40, '4')));
    EXPECT_EQ(packets[0].loss(), 0u);
    EXPECT_EQ(packets[1].loss_in_packet, kLossPresent | kLossSmbFull);
    EXPECT_EQ(packets[1].loss_from_buffer, 0u);
    EXPECT_EQ(num_bundled_sequences(), 0u);
  }
}

// In kDiscard, a bundle that fits the buffer but not its free space is kept.
// It waits as the pending bundle and is read after the stored bundles and
// before staging. From then on EndWriteBatch() does not bundle, but staging
// still accepts chunks until it is full.
TEST_F(TraceBufferV3Test, DiscardKeepsPendingBundle) {
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  const std::string b = Packet(std::string(40, 'b'));
  const std::string c = Packet(std::string(40, 'c'));
  const std::string d = Packet(std::string(40, 'd'));
  const size_t size_ab = BundleSize(params, {a}) + BundleSize(params, {b});
  const size_t size_c = BundleSize(params, {c});
  ResetBuffer(size_ab + 4, params, TraceBuffer::kDiscard);

  WriteBatch(a);
  WriteBatch(b);
  WriteBatch(c);  // Fits the buffer, not the 4 bytes that are free.
  EXPECT_TRUE(has_pending_bundle());
  EXPECT_EQ(trace_buffer_->used_size(), size_ab);
  EXPECT_EQ(stats().bundles_written(), 3u);
  EXPECT_EQ(stats().bundle_bytes_compressed(), size_ab + size_c);
  EXPECT_EQ(stats().bundles_overwritten(), 0u);
  EXPECT_EQ(stats().bundles_too_large(), 0u);
  EXPECT_EQ(num_bundled_sequences(), 1u);
  // The pass stopped right after it read |c|. Staging erases a chunk when it
  // reads past it, so the chunk of |c| is still there, with nothing to read.
  EXPECT_EQ(staging()->used_size(), 64u);

  // The next chunk is accepted and stays in staging.
  WriteBatch(d);
  EXPECT_EQ(stats().bundles_written(), 3u);
  EXPECT_EQ(stats().staging_chunks_read(), 3u);
  EXPECT_EQ(staging()->used_size(), 128u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, 'a'), std::string(40, 'b'),
                          std::string(40, 'c'), std::string(40, 'd')));
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss(), 0u);
  EXPECT_FALSE(has_pending_bundle());
  EXPECT_EQ(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(stats().bundles_written(), 4u);

  // The read emptied both buffers, but EndWriteBatch() still does not bundle
  // and staging did not get its space back. It still uses 64 bytes for each
  // chunk since the last pass, starting with the chunk of |c|, and fills up
  // after 4096 bytes in total.
  uint64_t chunks_accepted = 1;  // The chunk of |d|.
  while (stats().chunks_discarded() == 0) {
    WriteBatch(a);
    chunks_accepted++;
    ASSERT_LE(chunks_accepted, 100u);
  }
  chunks_accepted--;  // The last one was discarded.
  EXPECT_EQ(chunks_accepted, 4096u / 64 - 1);
  EXPECT_EQ(stats().bundles_written(), 4u);

  // All the accepted packets are still readable. More reads change nothing.
  EXPECT_EQ(ReadAll().size(), chunks_accepted - 1);
  WriteBatch(a);
  EXPECT_THAT(ReadAll(), IsEmpty());
  trace_buffer_->MaybeCompact();
  WriteBatch(a);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// In kDiscard, staging fills up inside one write batch while the bundled
// buffer is empty. EndWriteBatch() then stops bundling and later chunks are
// rejected. A read returns what was accepted.
TEST_F(TraceBufferV3Test, DiscardStopsWhenStagingIsFull) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 2048;
  ResetBuffer(64 * 1024, params, TraceBuffer::kDiscard);

  // Each chunk takes 1040 bytes, so three fit in the 4096 bytes of staging.
  const std::string big = Packet(std::string(1000, 'x'));
  for (ChunkID chunk_id = 0; chunk_id < 5; chunk_id++)
    WriteChunk(1, 1, chunk_id, {big});
  EXPECT_EQ(stats().chunks_written(), 3u);
  EXPECT_EQ(stats().chunks_discarded(), 2u);

  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 0u);
  EXPECT_EQ(trace_buffer_->used_size(), 0u);

  // The third record crosses the 2048-byte target, so all three go in one
  // bundle.
  EXPECT_EQ(ReadAll().size(), 3u);
  EXPECT_EQ(stats().bundles_written(), 1u);

  WriteChunk(1, 1, 5, {Packet("late")});
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().chunks_discarded(), 3u);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// While kDiscard still accepts writes, each pass returns the space used by
// bundled chunks to staging. So the buffer holds much more than the
// size of staging, and a consumer that keeps up can read it all.
TEST_F(TraceBufferV3Test, DiscardReusesStagingWhileBundling) {
  ResetBuffer(64 * 1024, OneBundlePerPacket(), TraceBuffer::kDiscard);
  size_t num_read = 0;
  for (int i = 0; i < 1000; i++) {
    WriteBatch(Packet(std::string(40, 'a')));
    if (i % 100 == 99)
      num_read += ReadAll().size();
  }
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  EXPECT_FALSE(has_pending_bundle());
  EXPECT_EQ(num_read, 1000u);
  EXPECT_EQ(stats().bytes_written(), 1000u * 64);
}

// Each pass compacts staging while the buffer accepts writes, in both
// policies. zstd has copied the packets it read, so their chunks are free.
// The remaining chunks move to the start of staging. Here they hold 8 KiB
// of fragments waiting for the rest of their packets.
TEST_F(TraceBufferV3Test, CompactsStagingAfterEachPass) {
  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    SCOPED_TRACE(policy == TraceBuffer::kDiscard ? "kDiscard" : "kOverwrite");
    Params params = OneBundlePerPacket();
    params.staging_bytes = 64 * 1024;
    ResetBuffer(64 * 1024, params, policy);

    for (WriterID writer = 10; writer < 18; writer++)
      WriteStartOfSplitPacket(writer);
    trace_buffer_->EndWriteBatch();
    EXPECT_EQ(staging()->used_size(), 8u * 1024);
    const uint64_t compactions = stats().compactions();

    // Each batch is a pass that bundles one chunk of 64 bytes.
    for (ChunkID chunk_id = 0; chunk_id < 10; chunk_id++) {
      WriteChunk(1, 1, chunk_id, {Packet(std::string(40, 'a'))});
      trace_buffer_->EndWriteBatch();
      EXPECT_EQ(staging()->used_size(), 8u * 1024);
    }
    EXPECT_EQ(stats().compactions(), compactions + 10);

    // The chunks that moved are intact. Their packets complete and are read
    // after the 10 packets that were bundled before them.
    for (WriterID writer = 10; writer < 18; writer++)
      WriteEndOfSplitPacket(writer);
    trace_buffer_->EndWriteBatch();
    const std::vector<TestPacket> packets = ReadAll();
    ASSERT_EQ(packets.size(), 10u + 8);
    for (size_t i = 0; i < packets.size(); i++) {
      EXPECT_EQ(packets[i].str, std::string(i < 10 ? 40 : 1100, 'a'));
      EXPECT_EQ(packets[i].loss(), 0u);
    }
    EXPECT_EQ(stats().chunks_overwritten(), 0u);
    EXPECT_EQ(stats().chunks_discarded(), 0u);
  }
}

// ---------------------------------------------------------------------------
// Reads and the memory of the returned slices
// ---------------------------------------------------------------------------

// One batch reads five bundles. The packets of the first bundles stay valid
// while later bundles are decoded into the second block and then into a
// temporary allocation once both blocks are full.
TEST_F(TraceBufferV3Test, SlicesStayValidForTheBatch) {
  Params params = OneBundlePerPacket();
  params.decode_block_bytes = 100;  // Two records of 47 bytes.
  ResetBuffer(4096, params);
  std::vector<std::string> expected;
  for (char c : {'a', 'b', 'c', 'd', 'e'}) {
    expected.push_back(Packet(std::string(40, c)));
    WriteBatch(expected.back());
  }

  trace_buffer_->BeginRead();
  std::vector<TracePacket> packets(5);
  for (TracePacket& packet : packets) {
    TestPacket ignored;
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &ignored, &packet));
  }
  EXPECT_EQ(decode_temporary_bytes(), 47u);
  for (size_t i = 0; i < packets.size(); i++)
    EXPECT_EQ(packets[i].GetRawBytesForTesting(), expected[i]);

  // The next batch needs none of it.
  trace_buffer_->BeginRead();
  EXPECT_EQ(decode_temporary_bytes(), 0u);
}

// A batch can stop in the middle of a bundle. The next batch continues in the
// same decoded bytes, even with writes in between.
TEST_F(TraceBufferV3Test, ReadResumesInsideBundle) {
  ResetBuffer(4096, BundleOnlyOnRead());
  WriteChunk(1, 1, 0, {Packet("p1"), Packet("p2"), Packet("p3"), Packet("p4")});
  trace_buffer_->EndWriteBatch();

  trace_buffer_->BeginRead();
  TestPacket packet;
  TracePacket p2;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet, &p2));
  EXPECT_EQ(packet.str, "p2");
  ASSERT_EQ(p2.slices().size(), 1u);
  const uint8_t* p2_end =
      static_cast<const uint8_t*>(p2.slices()[0].start) + p2.slices()[0].size;
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(num_bundled_sequences(), 1u);

  // A write batch between the two read tasks. It stays in staging.
  WriteChunk(1, 1, 1, {Packet("p5")});
  trace_buffer_->EndWriteBatch();

  trace_buffer_->BeginRead();
  TracePacket p3;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet, &p3));
  EXPECT_EQ(packet.str, "p3");
  // The record of p3 follows p2 in the same memory, so nothing was decoded
  // again. Its two varints take 2 bytes.
  EXPECT_EQ(static_cast<const uint8_t*>(p3.slices()[0].start), p2_end + 2);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "p4");
  EXPECT_EQ(stats().bundles_written(), 1u);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "p5");
  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
}

// A packet larger than a decode block gets a temporary allocation and is
// split into IPC slices. Once its bundle is fully read, BeginRead() frees
// the temporary allocation.
TEST_F(TraceBufferV3Test, LargePacketUsesTemporaryStorage) {
  Params params = BundleOnlyOnRead();
  params.staging_bytes = 1024 * 1024;
  ResetBuffer(1024 * 1024, params);
  const size_t initial_memory = trace_buffer_->GetMemoryUsageBytes();

  // 300000 bytes, in fragments of at most one chunk.
  const std::string large = Packet(RandomBytes(300000, 3));
  const size_t kFragmentSize = 30000;
  ChunkID chunk_id = 0;
  for (size_t offset = 0; offset < large.size(); offset += kFragmentSize) {
    uint8_t flags = 0;
    if (offset > 0)
      flags |= kContFromPrevChunk;
    if (offset + kFragmentSize < large.size())
      flags |= kContOnNextChunk;
    CreateChunk(1, 1, chunk_id++)
        .AddFragment(large.substr(offset, kFragmentSize), flags)
        .CopyIntoTraceBuffer();
  }
  WriteChunk(1, 1, chunk_id, {Packet("small")});
  trace_buffer_->EndWriteBatch();

  trace_buffer_->BeginRead();
  TracePacket large_packet;
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet, &large_packet));
  EXPECT_EQ(large_packet.GetRawBytesForTesting(), large);
  // The decoded packet is contiguous, so the buffer splits it for the IPC.
  EXPECT_EQ(large_packet.slices().size(), 3u);
  EXPECT_GT(decode_temporary_bytes(), large.size());
  EXPECT_GT(trace_buffer_->GetMemoryUsageBytes(), initial_memory);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "small");
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(large_packet.GetRawBytesForTesting(), large);

  trace_buffer_->BeginRead();
  EXPECT_EQ(decode_temporary_bytes(), 0u);
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), initial_memory);
}

// The consumer has read a part of the oldest bundle when a write batch
// overwrites it. Only the sequences with unread packets lose data, and the
// next read continues with the next bundle.
TEST_F(TraceBufferV3Test, OverwriteOfPartlyReadBundle) {
  const Params params = BundleOnlyOnRead();
  // Bundle 1: writer 1 has s1 and s2, writer 2 has t1.
  auto write_bundle_1 = [this] {
    WriteChunk(1, 1, 0, {Packet("s1"), Packet("s2")});
    WriteChunk(1, 2, 0, {Packet("t1")});
    BundleNow();
  };
  ResetBuffer(4096, params);
  write_bundle_1();
  ASSERT_EQ(stats().bundles_written(), 1u);
  const size_t bundle_1_size = trace_buffer_->used_size();

  // A buffer that bundle 1 fills, except for a few bytes.
  ResetBuffer(bundle_1_size + 8, params);
  write_bundle_1();
  ASSERT_EQ(trace_buffer_->used_size(), bundle_1_size);

  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "s2");
  // The consumer task ends here. Writer 1 was read completely, but t1 was
  // not.

  // Bundle 2 arrives between two read tasks and overwrites bundle 1.
  WriteChunk(1, 1, 1, {Packet("s3"), Packet("s4")});
  WriteChunk(1, 2, 1, {Packet("t2")});
  BundleNow();
  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_EQ(stats().bundles_overwritten(), 1u);
  EXPECT_EQ(stats().bundles_too_large(), 0u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("s3", "s4", "t2"));
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[1].loss(), 0u);
  EXPECT_EQ(packets[2].loss_from_buffer, kLossPresent | kLossOverwrite);
  EXPECT_EQ(num_bundled_sequences(), 0u);
}

// ---------------------------------------------------------------------------
// Data loss
// ---------------------------------------------------------------------------

// Loss that staging detects is written into the packet before compression,
// together with the bits that the producer had set in the same packet.
TEST_F(TraceBufferV3Test, StagingLossIsAddedBeforeCompression) {
  ResetBuffer(4096, OneBundlePerPacket());
  WriteChunk(1, 1, 0, {Packet("a")});
  // Chunk 1 is missing. The producer also flags its own loss on "c".
  WriteChunk(1, 1, 2, {Packet("c", kLossPresent | kLossSmbFull), Packet("d")});
  // A producer flag without loss in staging stays as it is.
  WriteChunk(1, 1, 3, {Packet("e", kLossPresent | kLossSmbFull)});
  trace_buffer_->EndWriteBatch();

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "c", "d", "e"));
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[1].loss_in_packet,
            kLossPresent | kLossReadGap | kLossSmbFull);
  EXPECT_EQ(packets[2].loss(), 0u);
  EXPECT_EQ(packets[3].loss_in_packet, kLossPresent | kLossSmbFull);
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss_from_buffer, 0u);
}

// The loss of an overwritten bundle is combined with the loss that the next
// packet of the sequence already has, and is reported once.
TEST_F(TraceBufferV3Test, BundleLossIsCombinedWithPacketLoss) {
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  const std::string b =
      Packet(std::string(40, 'b'), kLossPresent | kLossSmbFull);
  const std::string c = Packet(std::string(40, 'c'));
  const size_t size_b = BundleSize(params, {b});
  ASSERT_LE(BundleSize(params, {a}), BundleSize(params, {c}));
  ResetBuffer(size_b + BundleSize(params, {c}), params);
  WriteBatch(a);
  WriteBatch(b);
  WriteBatch(c);  // Overwrites the bundle of |a|.
  ASSERT_EQ(stats().bundles_overwritten(), 1u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].loss_in_packet, kLossPresent | kLossSmbFull);
  EXPECT_EQ(packets[0].loss_from_buffer,
            kLossPresent | kLossOverwrite | kLossSmbFull);
  EXPECT_EQ(packets[1].loss(), 0u);
}

// The sequence has no packet left in a bundle when its bundle is
// overwritten. The loss then waits in staging for the next packet.
TEST_F(TraceBufferV3Test, BundleLossOfSequenceWithNoBundleLeft) {
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  const size_t size_a = BundleSize(params, {a});
  // Room for one bundle. The last packet is a few bytes longer than |a|,
  // because it carries the loss.
  ResetBuffer(size_a + 16, params);
  WriteBatch(a);
  ASSERT_EQ(num_bundled_sequences(), 1u);

  // Another writer overwrites the only bundle of {1, 1}.
  WriteChunk(2, 1, 0, {Packet(std::string(40, 'x'))});
  trace_buffer_->EndWriteBatch();
  ASSERT_EQ(stats().bundles_overwritten(), 1u);
  EXPECT_EQ(num_bundled_sequences(), 1u);  // Only {2, 1}.

  WriteBatch(Packet(std::string(40, 'b')));  // Overwrites the bundle of {2, 1}.
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].str, std::string(40, 'b'));
  EXPECT_EQ(packets[0].loss_in_packet, kLossPresent | kLossOverwrite);
  EXPECT_EQ(packets[0].loss_from_buffer, 0u);
}

// In kOverwrite, a batch larger than staging overwrites chunks there before
// EndWriteBatch() can bundle them. That is ordinary loss on the sequence, even
// if the bundled buffer has room.
TEST_F(TraceBufferV3Test, StagingOverwriteIsReported) {
  Params params = OneBundlePerPacket();
  ResetBuffer(64 * 1024, params);
  WriteBatch(Packet("first"));

  // One batch of 80 chunks of 64 bytes into 4096 bytes of staging.
  for (int i = 0; i < 80; i++)
    WriteChunk(1, 1, next_chunk_id_++, {Packet(std::string(40, 'x'))});
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().chunks_overwritten(), 16u);
  EXPECT_EQ(stats().bundles_overwritten(), 0u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_EQ(packets.size(), 1u + 64);
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[1].loss_in_packet, kLossPresent | kLossOverwrite);
  for (size_t i = 2; i < packets.size(); i++)
    EXPECT_EQ(packets[i].loss(), 0u);
}

// ---------------------------------------------------------------------------
// Invalid packets and corrupted bundles
// ---------------------------------------------------------------------------

// A packet that fails validation is dropped when it leaves staging and
// counted once. The loss that staging reported with it moves to the next
// valid packet of the sequence.
TEST_F(TraceBufferV3Test, InvalidPacketsAreDroppedOnce) {
  ResetBuffer(4096, OneBundlePerPacket());
  ASSERT_TRUE(trace_buffer_->returns_validated_packets());
  WriteChunk(1, 1, 0, {Packet("a")});

  // After a gap come a packet that sets trusted_uid and a truncated packet.
  // The truncated packet announces 6 bytes of for_testing but has only 3.
  // The loss field takes 3 bytes, so if it were appended first, a parser
  // would read it as the rest of for_testing and the packet would pass
  // validation.
  const std::string sets_trusted_uid = "\x18\x01";
  const std::string truncated = "\xa2\x38\x06\x0a\x01\x41";
  WriteChunk(1, 1, 2, {sets_trusted_uid, truncated, Packet("b")});
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(trace_buffer_->packets_dropped_by_validation(), 2u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "b"));
  EXPECT_EQ(packets[1].loss_in_packet, kLossPresent | kLossReadGap);
  EXPECT_EQ(trace_buffer_->packets_dropped_by_validation(), 2u);
  EXPECT_EQ(stats().bundle_bytes_uncompressed(), 2 * (2 + 6u) + 3);

  // A chunk with only invalid packets makes no bundle.
  WriteChunk(1, 1, 3, {sets_trusted_uid});
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_EQ(trace_buffer_->packets_dropped_by_validation(), 3u);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// The reader checks what it decodes. A bundle that it cannot decode is
// dropped and counted once, its sequences report loss, and the read continues
// with the next bundle.
TEST_F(TraceBufferV3Test, CorruptedBundlesAreDropped) {
  const ProducerAndWriterID id = MkProducerAndWriterID(1, 1);
  const std::string good_record = Record(0, Packet("in-bad-bundle"));

  struct {
    const char* name;
    std::string records;
    std::optional<uint32_t> uncompressed_size;
    std::optional<std::string> frame;
    // The packets of the bad bundle that are returned before the error.
    size_t num_packets_before_error;
  } cases[] = {
      {"frame is not zstd", good_record, std::nullopt, std::string(30, 'z'), 0},
      {"frame is empty", good_record, std::nullopt, std::string(), 0},
      {"frame decodes to less than the header says", good_record,
       static_cast<uint32_t>(good_record.size() + 1), std::nullopt, 0},
      {"frame decodes to more than the header says", good_record,
       static_cast<uint32_t>(good_record.size() - 1), std::nullopt, 0},
      {"header asks for 4 GiB", good_record, 0xffffffffu, std::nullopt, 0},
      {"header says no records", good_record, 0u, std::nullopt, 0},
      {"sequence index out of range", good_record + Record(1, Packet("x")),
       std::nullopt, std::nullopt, 1},
      {"packet ends after the bundle",
       good_record + std::string("\x00\x7f", 2) + "short", std::nullopt,
       std::nullopt, 1},
      {"packet size varint is cut off", good_record + std::string("\x00", 1),
       std::nullopt, std::nullopt, 1},
      {"sequence index varint is too long",
       good_record + std::string(11, '\xff') + Packet("x"), std::nullopt,
       std::nullopt, 1},
      {"empty packet", good_record + std::string("\x00\x00", 2), std::nullopt,
       std::nullopt, 1},
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    next_chunk_id_ = 0;
    ResetBuffer(4096, OneBundlePerPacket());
    WriteBatch(Packet("before"));
    AppendRawBundle({id}, test_case.records, test_case.uncompressed_size,
                    test_case.frame);
    WriteBatch(Packet("after"));
    ASSERT_EQ(num_bundled_sequences(), 1u);

    std::vector<TestPacket> packets = ReadAll();
    std::vector<std::string> expected = {"before"};
    if (test_case.num_packets_before_error)
      expected.push_back("in-bad-bundle");
    expected.push_back("after");
    ASSERT_THAT(Strs(packets), ElementsAreArray(expected));
    EXPECT_EQ(packets.back().loss_from_buffer, kLossPresent | kLossCorrupted);
    EXPECT_EQ(stats().bundles_corrupted(), 1u);
    EXPECT_EQ(stats().bundles_overwritten(), 0u);
    EXPECT_EQ(trace_buffer_->packets_dropped_by_validation(), 0u);
    EXPECT_EQ(decode_temporary_bytes(), 0u);

    // No reference to the sequence leaked, and the buffer still works.
    EXPECT_EQ(num_bundled_sequences(), 0u);
    EXPECT_EQ(trace_buffer_->used_size(), 0u);
    WriteBatch(Packet("next"));
    packets = ReadAll();
    ASSERT_THAT(Strs(packets), ElementsAre("next"));
    EXPECT_EQ(packets[0].loss(), 0u);
  }
}

// ---------------------------------------------------------------------------
// Sequences
// ---------------------------------------------------------------------------

// Staging keeps the state of the last 1024 sequences that have no chunk and
// prunes older ones. A sequence whose packets are in a bundle has no chunk in
// staging either, but its state must stay to detect gaps and duplicate
// chunks of that writer.
TEST_F(TraceBufferV3Test, SequencesInBundlesAreNotPruned) {
  ResetBuffer(1024 * 1024, OneBundlePerPacket());
  const WriterID kNumWriters = 2000;
  for (WriterID writer = 1; writer <= kNumWriters; writer++) {
    WriteChunk(1, writer, 0, {Packet(std::to_string(writer))});
    trace_buffer_->EndWriteBatch();
  }

  // All the chunks were bundled. No sequence counts as empty, so no state was
  // pruned, although far more than 1024 sequences have no chunk.
  EXPECT_EQ(staging()->used_size(), 0u);
  EXPECT_EQ(num_staging_sequences(), kNumWriters);
  EXPECT_EQ(num_empty_staging_sequences(), 0u);
  EXPECT_EQ(num_bundled_sequences(), kNumWriters);

  // Every writer skips a chunk. Each gap is detected, because the state of
  // each sequence is still there.
  for (WriterID writer = 1; writer <= kNumWriters; writer++) {
    WriteChunk(1, writer, 2, {Packet(std::to_string(writer) + "-gap")});
    trace_buffer_->EndWriteBatch();
  }
  size_t num_gaps = 0;
  size_t num_packets = 0;
  for (const TestPacket& packet : ReadAll()) {
    num_packets++;
    num_gaps += packet.loss() == (kLossPresent | kLossReadGap);
  }
  EXPECT_EQ(num_packets, 2u * kNumWriters);
  EXPECT_EQ(num_gaps, kNumWriters);

  // Everything was read. Now the sequences are empty, and the next write
  // prunes all but the last 1024.
  EXPECT_EQ(num_bundled_sequences(), 0u);
  EXPECT_EQ(num_empty_staging_sequences(), kNumWriters);
  WriteChunk(1, 1, 3, {Packet("x")});
  EXPECT_EQ(num_staging_sequences(), 1024u + 1);
  trace_buffer_->EndWriteBatch();
  EXPECT_EQ(Strs(ReadAll()).size(), 1u);
}

// The sequences of the pending bundle of a kDiscard buffer are kept too.
TEST_F(TraceBufferV3Test, SequencesOfPendingBundleAreKept) {
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  ResetBuffer(BundleSize(params, {a}) + 4, params, TraceBuffer::kDiscard);
  WriteBatch(a);
  WriteChunk(1, 2, 0, {Packet(std::string(40, 'b'))});
  trace_buffer_->EndWriteBatch();
  ASSERT_TRUE(has_pending_bundle());
  EXPECT_EQ(num_bundled_sequences(), 2u);
  EXPECT_EQ(num_empty_staging_sequences(), 0u);

  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(num_bundled_sequences(), 1u);
  EXPECT_EQ(num_empty_staging_sequences(), 1u);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.writer_id, 2u);
  EXPECT_EQ(num_bundled_sequences(), 0u);
  // The read looks into staging and finds no more packets.
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(num_empty_staging_sequences(), 2u);
}

// ---------------------------------------------------------------------------
// Cloning
// ---------------------------------------------------------------------------

// The clone has the stored bundles, staging and the position inside a partly
// read bundle. The source and the clone then read independently.
TEST_F(TraceBufferV3Test, CloneContinuesWhereTheSourceStopped) {
  ResetBuffer(4096, BundleOnlyOnRead());
  WriteChunk(1, 1, 0, {Packet("a1"), Packet("a2"), Packet("a3")});
  BundleNow();
  WriteChunk(1, 1, 1, {Packet("b")});
  BundleNow();
  WriteChunk(1, 1, 2, {Packet("in-staging")});
  ASSERT_EQ(stats().bundles_written(), 2u);

  // The consumer of the source has "a1".
  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_EQ(packet.str, "a1");

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  ASSERT_TRUE(clone);
  EXPECT_EQ(clone->buf_type(), TraceBuffer::kV3);
  EXPECT_EQ(clone->size(), 4096u);
  EXPECT_EQ(clone->used_size(), trace_buffer_->used_size());
  EXPECT_EQ(clone->overwrite_policy(), TraceBuffer::kOverwrite);
  EXPECT_TRUE(clone->has_data());
  EXPECT_EQ(clone->stats().bundles_written(), 2u);
  EXPECT_EQ(clone->stats().bytes_read(), 0u);
  EXPECT_EQ(clone->stats().bytes_written(), stats().bytes_written());

  // The source moves on. It reads everything and takes new data.
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "a2");
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("a3", "b", "in-staging"));
  WriteChunk(1, 1, 3, {Packet("only-in-source")});

  std::vector<TestPacket> cloned = perfetto::ReadAll(clone.get());
  ASSERT_THAT(Strs(cloned), ElementsAre("a2", "a3", "b", "in-staging"));
  for (const TestPacket& cloned_packet : cloned) {
    EXPECT_EQ(cloned_packet.loss(), 0u);
    EXPECT_EQ(cloned_packet.uid, 1001u);
  }
  EXPECT_THAT(perfetto::ReadAll(clone.get()), IsEmpty());

  // A clone outlives its source.
  clone = trace_buffer_->CloneReadOnly();
  trace_buffer_.reset();
  EXPECT_THAT(Strs(perfetto::ReadAll(clone.get())),
              ElementsAre("only-in-source"));
}

// The stored bundles of the source continue at the start of its memory. The
// clone gets them in order.
TEST_F(TraceBufferV3Test, CloneOfWrappedBuffer) {
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  const std::string b = Packet(std::string(40, 'b'));
  const std::string c = Packet(std::string(40, 'c'));
  ResetBuffer(BundleSize(params, {a}) + BundleSize(params, {b}) + 20, params);
  WriteBatch(a);
  WriteBatch(b);
  WriteBatch(c);
  ASSERT_EQ(stats().bundles_overwritten(), 1u);

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  trace_buffer_.reset();
  std::vector<TestPacket> packets = perfetto::ReadAll(clone.get());
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, 'b'), std::string(40, 'c')));
  // The loss that waited in the source is reported by the clone.
  EXPECT_EQ(packets[0].loss_from_buffer, kLossPresent | kLossOverwrite);
  EXPECT_EQ(clone->stats().bundles_overwritten(), 1u);
}

// A clone of a buffer whose packets are all in staging stores no bundle at
// first. It bundles them when it reads. A packet that fits the source must
// fit the clone too.
TEST_F(TraceBufferV3Test, CloneWithStagingOnly) {
  ResetBuffer(4096, BundleOnlyOnRead());
  WriteChunk(1, 1, 0, {Packet("small")});
  // Random bytes do not compress, so its bundle needs more than 1000 bytes of
  // the buffer.
  WriteChunk(1, 1, 1, {Packet(RandomBytes(1000, 4))});
  trace_buffer_->EndWriteBatch();
  ASSERT_EQ(trace_buffer_->used_size(), 0u);

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  EXPECT_EQ(clone->used_size(), 0u);
  EXPECT_EQ(clone->size(), 4096u);
  std::vector<TestPacket> packets = perfetto::ReadAll(clone.get());
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "small");
  EXPECT_EQ(packets[1].str, RandomBytes(1000, 4));
  EXPECT_EQ(clone->stats().bundles_too_large(), 0u);

  // The source still has its packets.
  EXPECT_EQ(ReadAll().size(), 2u);
}

// The clone of a kDiscard buffer keeps the pending bundle in its place.
TEST_F(TraceBufferV3Test, CloneWithPendingBundle) {
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  ResetBuffer(BundleSize(params, {a}) + 4, params, TraceBuffer::kDiscard);
  WriteBatch(a);
  WriteBatch(Packet(std::string(40, 'b')));
  WriteBatch(Packet(std::string(40, 'c')));
  ASSERT_TRUE(has_pending_bundle());

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  EXPECT_EQ(clone->overwrite_policy(), TraceBuffer::kDiscard);
  EXPECT_GT(clone->GetMemoryUsageBytes(), 0u);
  trace_buffer_.reset();
  std::vector<TestPacket> packets = perfetto::ReadAll(clone.get());
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, 'a'), std::string(40, 'b'),
                          std::string(40, 'c')));
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss(), 0u);
}

// A buffer that moves to a cloned session with transfer_on_clone becomes
// read-only as it is, and still returns everything.
TEST_F(TraceBufferV3Test, SetReadOnly) {
  ResetBuffer(4096, OneBundlePerPacket());
  WriteBatch(Packet("bundled"));
  WriteChunk(1, 1, next_chunk_id_++, {Packet("staged")});
  trace_buffer_->set_read_only();
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("bundled", "staged"));
  trace_buffer_->MaybeCompact();
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// ---------------------------------------------------------------------------
// ProtoVM
// ---------------------------------------------------------------------------

// A writer emits P1, then P2. P1 is in a bundle and P2 is still in staging
// when staging overwrites P2. The ProtoVM must not get P2 then, because it
// would get the older P1 afterwards, when the bundle of P1 is overwritten.
TEST_F(TraceBufferV3Test, ProtoVmGetsBundleOverwritesOnly) {
  const Params params = OneBundlePerPacket();
  const std::string p1 = PatchPacket("P1");
  const std::string p2 = PatchPacket("P2");
  const std::string other = Packet(std::string(40, 'x'));
  // Room for one bundle, either of P1 or of a packet of the other producer
  // plus the few bytes of a loss field.
  ResetBuffer(
      std::max(BundleSize(params, {p1}), BundleSize(params, {other})) + 16,
      params);
  trace_buffer_->protovms()->MaybeSetUp(
      "data_source", CreateProtoVmProgram().SerializeAsString(), 1024,
      /*producer_id=*/1);

  WriteBatch(p1);
  ASSERT_EQ(stats().bundles_written(), 1u);
  WriteChunk(1, 1, next_chunk_id_++,
             {p2});  // No EndWriteBatch(), so it stays in staging.

  // A batch of another producer fills staging and overwrites the chunk of P2.
  for (ChunkID chunk_id = 0; chunk_id < 64; chunk_id++)
    WriteChunk(2, 1, chunk_id, {other});
  ASSERT_GE(stats().chunks_overwritten(), 1u);
  EXPECT_FALSE(ProtoVmState(trace_buffer_.get()).has_string_merged());

  // The pass bundles the other producer, which overwrites the bundle of P1.
  trace_buffer_->EndWriteBatch();
  ASSERT_GT(stats().bundles_overwritten(), 0u);
  ASSERT_EQ(stats().bundles_too_large(), 0u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "P1");

  // The next packet of the writer reports the loss of P1 and of P2.
  WriteBatch(PatchPacket("P3"));
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].str, "P3");
  EXPECT_EQ(packets[0].loss_in_packet, kLossPresent | kLossOverwrite);
}

// The consumer has read P1 of a bundle when the bundle is overwritten. The
// ProtoVM gets only the unread packet, P2. The clone of the buffer has a
// clone of the ProtoVM.
TEST_F(TraceBufferV3Test, ProtoVmGetsUnreadPacketsOfPartlyReadBundle) {
  const Params params = BundleOnlyOnRead();
  const std::vector<std::string> bundle_1 = {
      PatchPacket("P1", 7), PatchPacket("P2-" + std::string(40, '2'))};
  ResetBuffer(BundleSize(params, bundle_1), params);
  trace_buffer_->protovms()->MaybeSetUp(
      "data_source", CreateProtoVmProgram().SerializeAsString(), 1024,
      /*producer_id=*/1);
  WriteChunk(1, 1, 0, bundle_1);
  BundleNow();
  ASSERT_EQ(stats().bundles_written(), 1u);

  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_EQ(packet.str, "P1");

  WriteChunk(1, 1, 1, {PatchPacket("P3")});
  BundleNow();
  ASSERT_EQ(stats().bundles_overwritten(), 1u);
  ASSERT_EQ(stats().bundles_too_large(), 0u);
  auto state = ProtoVmState(trace_buffer_.get());
  EXPECT_EQ(state.string_merged(), "P2-" + std::string(40, '2'));
  // P1 had int_to_merge. The ProtoVM never saw it.
  EXPECT_FALSE(state.has_int_merged());

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  trace_buffer_.reset();
  EXPECT_EQ(ProtoVmState(clone.get()).string_merged(),
            "P2-" + std::string(40, '2'));
  std::vector<TestPacket> packets = perfetto::ReadAll(clone.get());
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].str, "P3");
  EXPECT_EQ(packets[0].loss_from_buffer, kLossPresent | kLossOverwrite);
}

// A bundle that nobody read is overwritten. The ProtoVM gets all its packets
// in order, including those of an SMB v2 writer, which staging has already
// rewritten to ordinary protobuf. A producer without a ProtoVM is ignored.
TEST_F(TraceBufferV3Test, ProtoVmGetsWholeBundleAndSmbV2Packets) {
  const Params params = BundleOnlyOnRead();
  // The first packet makes this the largest bundle of the test, so each
  // later bundle overwrites the one before it.
  const std::vector<std::string> bundle_1 = {PatchPacket(std::string(60, '0')),
                                             PatchPacket("P1", 7),
                                             PatchPacket("P2")};
  ResetBuffer(BundleSize(params, bundle_1), params);
  trace_buffer_->protovms()->MaybeSetUp(
      "data_source", CreateProtoVmProgram().SerializeAsString(), 1024,
      /*producer_id=*/1);
  WriteChunk(1, 1, 0, bundle_1);
  BundleNow();

  // Producer 2 has no ProtoVM. Its bundle overwrites bundle 1.
  WriteChunk(2, 1, 0, {Packet("x")});
  BundleNow();
  ASSERT_EQ(stats().bundles_overwritten(), 1u);
  auto state = ProtoVmState(trace_buffer_.get());
  EXPECT_EQ(state.string_merged(), "P2");
  EXPECT_EQ(state.int_merged(), 7);

  // for_testing { protovm_patch { string_to_merge: "v2" } } in proto groups.
  // A standalone TraceBufferV2 rejects SMB v2 chunks of a producer with a
  // ProtoVM. Staging has no ProtoVM, so it accepts them.
  const std::string v2_patch = std::string("\xa3\x38\x33\x0a\x02", 5) + "v2" +
                               std::string("\x04\x04", 2);
  ASSERT_TRUE(CopyChunkV2(1, 9, {v2_patch}));
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  BundleNow();
  ASSERT_EQ(stats().bundles_overwritten(), 2u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "P2");

  WriteChunk(2, 1, 1, {Packet("y")});
  BundleNow();
  ASSERT_EQ(stats().bundles_overwritten(), 3u);
  ASSERT_EQ(stats().bundles_too_large(), 0u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "v2");
}

// ---------------------------------------------------------------------------
// Counters and memory
// ---------------------------------------------------------------------------

TEST_F(TraceBufferV3Test, Stats) {
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  const size_t size_a = BundleSize(params, {a});
  ResetBuffer(2 * size_a, params);

  // Three chunks of 64 bytes, each one a bundle with a record of 47 bytes.
  // The third bundle overwrites the first.
  for (int i = 0; i < 3; i++)
    WriteBatch(a);
  {
    const TraceStats::BufferStats& s = stats();
    EXPECT_EQ(s.buffer_size(), 2 * size_a);
    EXPECT_EQ(s.chunks_written(), 3u);
    EXPECT_EQ(s.bytes_written(), 3 * 64u);
    EXPECT_EQ(s.staging_chunks_read(), 3u);
    EXPECT_EQ(s.staging_bytes_read(), 3 * 64u);
    EXPECT_EQ(s.chunks_overwritten(), 0u);
    EXPECT_EQ(s.bundles_written(), 3u);
    EXPECT_EQ(s.bundle_bytes_uncompressed(), 3 * 47u);
    EXPECT_EQ(s.bundle_bytes_compressed(), 3 * size_a);
    EXPECT_EQ(s.bundles_overwritten(), 1u);
    EXPECT_EQ(s.bundles_too_large(), 0u);
    EXPECT_EQ(s.bundles_corrupted(), 0u);
    EXPECT_EQ(s.compactions(), 3u);
    // Nothing was returned to the consumer yet.
    EXPECT_EQ(s.bytes_read(), 0u);
    EXPECT_EQ(s.chunks_read(), 0u);
    EXPECT_EQ(s.readaheads_succeeded(), 0u);
    EXPECT_EQ(s.readaheads_failed(), 0u);
  }
  EXPECT_EQ(ReadAll().size(), 2u);
  EXPECT_EQ(stats().bytes_read(), 2 * 47u);
  EXPECT_EQ(stats().chunks_read(), 0u);
  EXPECT_EQ(stats().staging_chunks_read(), 3u);

  // The histogram of chunk payload sizes comes from staging.
  size_t num_writers = 0;
  for (auto it = trace_buffer_->writer_stats().GetIterator(); it; ++it)
    num_writers++;
  EXPECT_EQ(num_writers, 1u);
}

// The memory bound covers the memory that is allocated later, on the first
// bundle and the first read, so it does not change when that happens.
TEST_F(TraceBufferV3Test, MemoryUsage) {
  const Params params = DefaultParams(1024 * 1024);
  EXPECT_EQ(params.bundle_target_bytes, 128 * 1024u);
  EXPECT_EQ(params.staging_bytes, 1024 * 1024u);
  ResetBuffer(1024 * 1024, params);
  const size_t initial = trace_buffer_->GetMemoryUsageBytes();
  EXPECT_GT(initial, 2 * 1024 * 1024u + 2 * params.decode_block_bytes);
  EXPECT_LT(initial, 5 * 1024 * 1024u);

  for (ChunkID chunk_id = 0; chunk_id < 200; chunk_id++) {
    WriteChunk(1, 1, chunk_id, {Packet(RandomBytes(2000, chunk_id))});
    trace_buffer_->EndWriteBatch();
  }
  EXPECT_GT(stats().bundles_written(), 1u);
  EXPECT_EQ(ReadAll().size(), 200u);
  trace_buffer_->BeginRead();
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), initial);
  trace_buffer_->MaybeCompact();
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), initial);

  // The smallest buffers still accept the largest SMB chunk.
  EXPECT_GT(DefaultParams(1).staging_bytes, 65536u + 16);
}

// ---------------------------------------------------------------------------
// Same output as TraceBufferV2
// ---------------------------------------------------------------------------

// Feeds the same chunks to a TraceBufferV2 and to a TraceBufferV3 that are
// large enough to lose nothing, with reads at the same points. Each sequence
// must give the same packets in the same order, with the same identity and
// the same loss.
TEST_F(TraceBufferV3Test, SameOutputAsTraceBufferV2) {
  Params params = OneBundlePerPacket();
  params.staging_bytes = 1024 * 1024;
  params.bundle_target_bytes = 3000;
  params.decode_block_bytes = 8000;
  ResetBuffer(4 * 1024 * 1024, params);
  std::unique_ptr<TraceBufferV2> v2 = TraceBufferV2::Create(4 * 1024 * 1024);
  TraceBuffer* buffers[] = {v2.get(), trace_buffer_.get()};

  struct Writer {
    ProducerID producer_id;
    WriterID writer_id;
    ChunkID next_chunk_id = 0;
    uint32_t next_packet = 0;
    // The rest of a packet that started in the last chunk of this writer.
    std::string pending;
  };
  std::vector<Writer> writers;
  for (ProducerID producer = 1; producer <= 3; producer++) {
    for (WriterID writer = 1; writer <= 4; writer++)
      writers.push_back({producer, writer, 0, 0, std::string()});
  }

  using SequencePackets =
      std::map<ProducerAndWriterID, std::vector<TestPacket>>;
  SequencePackets read[2];
  auto read_some = [&](size_t max_packets) {
    for (size_t i = 0; i < 2; i++) {
      buffers[i]->BeginRead();
      TestPacket packet;
      for (size_t n = 0; n < max_packets && ReadOne(buffers[i], &packet); n++) {
        read[i][MkProducerAndWriterID(packet.producer_id, packet.writer_id)]
            .push_back(packet);
      }
    }
  };

  std::minstd_rand rnd(11);
  for (int batch = 0; batch < 300; batch++) {
    const uint32_t num_chunks = 1 + rnd() % 6;
    for (uint32_t c = 0; c < num_chunks; c++) {
      Writer& writer = writers[rnd() % writers.size()];
      // Sometimes a writer skips a ChunkID, which is a gap for both buffers.
      if (writer.pending.empty() && rnd() % 40 == 0)
        writer.next_chunk_id++;
      const ChunkID chunk_id = writer.next_chunk_id++;
      FakeChunk chunks[2] = {
          FakeChunk(buffers[0], writer.producer_id, writer.writer_id, chunk_id),
          FakeChunk(buffers[1], writer.producer_id, writer.writer_id,
                    chunk_id)};
      auto add = [&](const std::string& fragment, uint8_t flags) {
        for (FakeChunk& chunk : chunks) {
          chunk.SetUID(1000 + writer.producer_id);
          chunk.AddFragment(fragment, flags);
        }
      };
      if (!writer.pending.empty()) {
        add(writer.pending, kContFromPrevChunk);
        writer.pending.clear();
      }
      const uint32_t num_packets = rnd() % 5;
      for (uint32_t p = 0; p < num_packets; p++) {
        const size_t num_random_bytes = rnd() % 300;
        const uint32_t seed = static_cast<uint32_t>(rnd());
        const std::string packet =
            Packet(std::to_string(writer.next_packet++) + ":" +
                   RandomBytes(num_random_bytes, seed));
        // The last packet of a chunk sometimes continues in the next chunk
        // of the writer, which arrives in a later batch.
        if (p + 1 == num_packets && rnd() % 4 == 0) {
          const size_t cut = 1 + rnd() % (packet.size() - 1);
          add(packet.substr(0, cut), kContOnNextChunk);
          writer.pending = packet.substr(cut);
        } else {
          add(packet, 0);
        }
      }
      for (FakeChunk& chunk : chunks)
        chunk.CopyIntoTraceBuffer();
    }
    for (TraceBuffer* buffer : buffers)
      buffer->EndWriteBatch();
    if (rnd() % 10 == 0)
      read_some(1 + rnd() % 50);
  }
  read_some(1000 * 1000);

  EXPECT_GT(stats().bundles_written(), 50u);
  EXPECT_EQ(stats().bundles_overwritten(), 0u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  ASSERT_EQ(read[0].size(), read[1].size());
  size_t num_packets = 0;
  size_t num_losses = 0;
  for (const auto& [sequence_id, v2_packets] : read[0]) {
    const std::vector<TestPacket>& v3_packets = read[1][sequence_id];
    ASSERT_EQ(v2_packets.size(), v3_packets.size());
    for (size_t i = 0; i < v2_packets.size(); i++) {
      ASSERT_EQ(v2_packets[i].str, v3_packets[i].str);
      EXPECT_EQ(v2_packets[i].uid, v3_packets[i].uid);
      EXPECT_EQ(v2_packets[i].loss(), v3_packets[i].loss());
      num_packets++;
      num_losses += v3_packets[i].loss() != 0;
    }
  }
  EXPECT_GT(num_packets, 1000u);
  EXPECT_GT(num_losses, 5u);
}

}  // namespace perfetto
