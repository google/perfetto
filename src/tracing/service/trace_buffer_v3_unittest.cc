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
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/slice.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protovm/vm.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/service/tracing_service_impl.h"
#include "src/tracing/service/zstd_frame_codec.h"
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
using tracing_service::TracingServiceImpl;

namespace {

using internal::BundleBuffer;
using internal::DecodeArena;
using BundleMetadata = internal::BundleBuffer::BundleMetadata;
using SequenceLoss = BundleMetadata::SequenceLoss;

constexpr uint32_t kLossPresent =
    protos::pbzero::TracePacket::DATA_LOSS_PRESENT;
constexpr uint32_t kLossReadGap =
    protos::pbzero::TracePacket::DATA_LOSS_READ_GAP;
constexpr uint32_t kLossOverwrite =
    protos::pbzero::TracePacket::DATA_LOSS_OVERWRITE;

constexpr uint8_t kContFromPrevChunk =
    SharedMemoryABI::ChunkHeader::kFirstPacketContinuesFromPrevChunk;
constexpr uint8_t kContOnNextChunk =
    SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk;
constexpr uint8_t kChunkNeedsPatching =
    SharedMemoryABI::ChunkHeader::kChunkNeedsPatching;

// A valid TracePacket with for_testing.str = |str|.
// |str| up to 125 bytes takes str.size() + 5 bytes in the packet.
std::string Packet(const std::string& str) {
  protozero::HeapBuffered<protos::pbzero::TracePacket> packet;
  packet->set_for_testing()->set_str(str);
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

// A codec sized like the one that TraceBufferFactory shares.
std::shared_ptr<ZstdFrameCodec> CreateCodec() {
  return std::make_shared<ZstdFrameCodec>(TraceBufferV3::kMaxBundleTargetSize);
}

// One bundle record with [sequence_index][loss_reasons][packet_size][packet].
std::string Record(uint32_t sequence_index, const std::string& packet) {
  uint8_t header[20];
  uint8_t* end = protozero::proto_utils::WriteVarInt(sequence_index, header);
  end = protozero::proto_utils::WriteVarInt(0, end);
  end = protozero::proto_utils::WriteVarInt(packet.size(), end);
  return std::string(reinterpret_cast<const char*>(header),
                     static_cast<size_t>(end - header)) +
         packet;
}

// SequenceLoss has no operator==, so tests compare loss lists as pairs.
std::vector<std::pair<ProducerAndWriterID, uint32_t>> Losses(
    const std::vector<SequenceLoss>& sequence_losses) {
  std::vector<std::pair<ProducerAndWriterID, uint32_t>> losses;
  for (const SequenceLoss& loss : sequence_losses)
    losses.emplace_back(loss.sequence_key, loss.loss_reasons);
  return losses;
}

std::string SliceToString(const Slice& slice) {
  return std::string(static_cast<const char*>(slice.start), slice.size);
}

// Packet contents and trusted metadata returned by the buffer.
struct TestPacket {
  std::string str;
  ProducerID producer_id = 0;
  WriterID writer_id = 0;
  uid_t uid = 0;

  // The loss that ReadNextTracePacket() returned for the service to append.
  uint32_t loss = 0;
};

std::vector<std::string> Strs(const std::vector<TestPacket>& packets) {
  std::vector<std::string> strs;
  for (const TestPacket& packet : packets)
    strs.push_back(packet.str);
  return strs;
}

// Reads one packet and gives |raw| its slices into the buffer, if set.
bool ReadOne(TraceBuffer* buf, TestPacket* out, TracePacket* raw = nullptr) {
  TracePacket packet;
  TraceBuffer::PacketSequenceProperties properties{};
  uint32_t loss = 0;
  if (!buf->ReadNextTracePacket(&packet, &properties, &loss))
    return false;
  for (const Slice& slice : packet.slices())
    EXPECT_LE(slice.size, TracingServiceImpl::kMaxTracePacketSliceSize);

  protos::gen::TracePacket proto;
  EXPECT_TRUE(proto.ParseFromString(packet.GetRawBytesForTesting()));
  *out = TestPacket();
  out->str = proto.for_testing().has_protovm_patch()
                 ? proto.for_testing().protovm_patch().string_to_merge()
                 : proto.for_testing().str();
  out->producer_id = properties.producer_id_trusted;
  out->writer_id = properties.writer_id;
  out->uid = properties.producer_uid_trusted();
  out->loss = loss;
  if (raw)
    *raw = std::move(packet);
  return true;
}

std::vector<TestPacket> ReadAll(TraceBuffer* buf) {
  std::vector<TestPacket> packets;
  buf->BeginRead();
  for (TestPacket packet; ReadOne(buf, &packet);)
    packets.push_back(std::move(packet));
  return packets;
}

// Returns the string and integer last merged by the ProtoVM of |buf|.
protos::gen::TestEvent::ProtoVmIncrementalState ProtoVmState(TraceBuffer* buf) {
  const auto& instances = buf->protovms()->instances();
  EXPECT_EQ(instances.size(), 1u);
  if (instances.empty())
    return {};
  protozero::HeapBuffered<protos::pbzero::TracePacket> state;
  instances[0].instance->SerializeIncrementalState(state.get());
  protos::gen::TracePacket proto;
  EXPECT_TRUE(proto.ParseFromString(state.SerializeAsString()));
  return proto.for_testing().protovm_incremental_state();
}

}  // namespace

class TraceBufferV3Test : public testing::Test {
 public:
  // A target at which a checkpoint after any new chunk starts a bundling pass.
  static constexpr size_t kOneBundlePerPacket = 1;

  // A target above the amount the tests write, so packets stay in staging
  // until a read or an explicit BundleNow().
  static constexpr size_t kBundleOnlyOnRead = 1024 * 1024;

  // Staging and the bundle buffer both get |size|.
  void ResetBuffer(
      size_t size,
      size_t target_records_size,
      TraceBuffer::OverwritePolicy policy = TraceBuffer::kOverwrite) {
    trace_buffer_ = TraceBufferV3::CreateInternal(
        size, policy, target_records_size, CreateCodec());
    ASSERT_TRUE(trace_buffer_);
  }

  // Creates a buffer like ResetBuffer() that shares |trace_buffer_|'s codec.
  std::unique_ptr<TraceBufferV3> CreateBufferSharingCodec(
      size_t size,
      size_t target_records_size) {
    return TraceBufferV3::CreateInternal(size, TraceBuffer::kOverwrite,
                                         target_records_size,
                                         trace_buffer_->codec_);
  }

  FakeChunk CreateChunk(ProducerID p, WriterID w, ChunkID c) {
    FakeChunk chunk(trace_buffer_.get(), p, w, c);
    chunk.SetUID(1000 + p);
    return chunk;
  }

  // Copies |packets| as one complete chunk and returns its size in staging.
  size_t WriteChunk(ProducerID p,
                    WriterID w,
                    ChunkID c,
                    const std::vector<std::string>& packets) {
    FakeChunk chunk = CreateChunk(p, w, c);
    for (const std::string& packet : packets)
      chunk.AddFragment(packet);
    return base::AlignUp(chunk.CopyIntoTraceBuffer(), 16);
  }

  // Writes |packet| as one batch for sequence {1, 1} with consecutive ChunkIDs,
  // so the sequence has no gap.
  void WriteBatch(const std::string& packet) {
    WriteChunk(1, 1, next_chunk_id_++, {packet});
    trace_buffer_->OnWriteCheckpoint();
  }

  // Runs a checkpoint pass regardless of the target, so a test using
  // kBundleOnlyOnRead can choose which packets share a bundle.
  void BundleNow() {
    trace_buffer_->BundleStagingForWrite(internal::ChunkSeqReader::kReadMode);
  }

  // Leaves a 1 KiB chunk that bundling cannot consume yet.
  // WriteEndOfSplitPacket() supplies the missing fragment.
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

  // |flags| takes kContFromPrevChunk and kContOnNextChunk.
  bool CopyChunkV2(ProducerID p,
                   WriterID w,
                   const std::vector<std::string>& fragments,
                   uint32_t flags = 0) {
    std::vector<protozero::ConstBytes> bytes;
    for (const std::string& fragment : fragments) {
      bytes.push_back(
          {reinterpret_cast<const uint8_t*>(fragment.data()), fragment.size()});
    }
    return trace_buffer_->CopyChunkV2Untrusted(
        {p, ClientIdentity(1000 + p, 2000 + p), w}, bytes.data(), bytes.size(),
        (flags & kContFromPrevChunk) != 0, (flags & kContOnNextChunk) != 0);
  }

  std::vector<TestPacket> ReadAll() {
    return perfetto::ReadAll(trace_buffer_.get());
  }

  // Tests need exact bundle offsets, but compressed sizes depend on zstd.
  // Measure the first bundle built from one chunk of |packets| with
  // |target_records_size|.
  static size_t BundleSize(size_t target_records_size,
                           const std::vector<std::string>& packets) {
    auto probe =
        TraceBufferV3::CreateInternal(1024 * 1024, TraceBuffer::kOverwrite,
                                      target_records_size, CreateCodec());
    FakeChunk chunk(probe.get(), 1, 1, 0);
    chunk.SetUID(1001);
    for (const std::string& packet : packets)
      chunk.AddFragment(packet);
    chunk.CopyIntoTraceBuffer();
    probe->BeginRead();
    probe->BundleStagingForRead();
    return probe->used_size();
  }

  size_t target_records_size() const {
    return trace_buffer_->target_records_size_;
  }
  BundleBuffer& bundle_buf() { return trace_buffer_->bundle_buf_; }
  TraceBufferV2* staging() { return trace_buffer_->staging_buf_.get(); }
  bool staging_discard_writes() { return staging()->discard_writes(); }
  void SuppressClientDchecksForTesting() {
    staging()->suppress_client_dchecks_for_testing_ = true;
  }
  bool has_overflow_bundle() { return !!trace_buffer_->overflow_bundle_; }
  size_t num_bundled_sequences() {
    return trace_buffer_->bundled_sequences_.size();
  }
  size_t num_staging_sequences() { return staging()->sequences_.size(); }
  size_t num_empty_staging_sequences() { return staging()->empty_sequences_; }

  BundleMetadata FrontBundleMetadata() {
    BundleMetadata metadata;
    bundle_buf().PeekFrontMetadata(&metadata);
    return metadata;
  }

  size_t decode_memory_bytes() {
    return trace_buffer_->decode_arena_.GetMemoryUsageBytes();
  }

  const TraceStats::BufferStats& stats() { return trace_buffer_->stats(); }

  const TraceStats::BufferStats::ExperimentalV3Stats& v3_stats() {
    return stats().experimental_v3_stats();
  }

 protected:
  std::unique_ptr<TraceBufferV3> trace_buffer_;
  ChunkID next_chunk_id_ = 0;
};

// ---------------------------------------------------------------------------
// BundleBuffer and DecodeArena
// ---------------------------------------------------------------------------

// Pushes the last bundle so that it starts exactly at offset 0, or wraps in its
// header, a sequence key, its loss entry or its compressed records.
// Reads must return the same metadata and bytes in every case.
TEST(BundleBufferTest, PushAndReadAcrossTheEnd) {
  const std::vector<ProducerAndWriterID> first_keys = {1, 2};
  const std::vector<SequenceLoss> first_losses = {
      {1, kLossPresent | kLossReadGap}};
  const std::vector<ProducerAndWriterID> last_keys = {3, 4};
  const std::vector<SequenceLoss> last_losses = {
      {4, kLossPresent | kLossOverwrite}};
  const protozero::ConstBytes first_compressed_records{
      reinterpret_cast<const uint8_t*>("abcdefgh"), 8};
  const protozero::ConstBytes last_compressed_records{
      reinterpret_cast<const uint8_t*>("01234567"), 8};
  const size_t bundle_header_size =
      static_cast<size_t>(BundleBuffer::GetBundleSize(0, 0, 0));
  const size_t metadata_size = static_cast<size_t>(
      BundleBuffer::GetBundleSize(first_keys.size(), first_losses.size(), 0));
  const size_t bundle_size = static_cast<size_t>(BundleBuffer::GetBundleSize(
      first_keys.size(), first_losses.size(), first_compressed_records.size));

  for (size_t bytes_to_end :
       {size_t{0}, bundle_header_size / 2,
        bundle_header_size + sizeof(ProducerAndWriterID) / 2,
        metadata_size - sizeof(SequenceLoss) / 2,
        metadata_size + last_compressed_records.size / 2}) {
    SCOPED_TRACE(bytes_to_end);
    BundleBuffer buf;
    ASSERT_TRUE(buf.Initialize(2 * bundle_size + bytes_to_end));
    buf.Push({first_keys, first_losses, 80}, first_compressed_records);
    buf.Push({first_keys, first_losses, 81}, first_compressed_records);
    buf.Pop();
    // Keep the second bundle so the cursors do not reset before this push.
    buf.Push({last_keys, last_losses, 82}, last_compressed_records);
    EXPECT_EQ(buf.used_size(), 2 * bundle_size);
    EXPECT_EQ(buf.free_size(), bytes_to_end);

    BundleMetadata metadata;
    buf.PeekFrontMetadata(&metadata);
    EXPECT_EQ(metadata.sequence_keys, first_keys);
    EXPECT_EQ(Losses(metadata.sequence_losses), Losses(first_losses));
    EXPECT_EQ(metadata.records_size, 81u);
    EXPECT_EQ(SliceToString(buf.PeekFrontCompressedRecords()), "abcdefgh");
    buf.Pop();

    buf.PeekFrontMetadata(&metadata);
    EXPECT_EQ(metadata.sequence_keys, last_keys);
    EXPECT_EQ(Losses(metadata.sequence_losses), Losses(last_losses));
    EXPECT_EQ(metadata.records_size, 82u);
    EXPECT_EQ(SliceToString(buf.PeekFrontCompressedRecords()), "01234567");
    buf.Pop();
    EXPECT_TRUE(buf.empty());
  }
}

// The clone has its own copy of the bundles, with the wrapped one in one piece.
TEST(BundleBufferTest, CloneCopiesStoredBundles) {
  const std::vector<ProducerAndWriterID> keys = {1};
  const std::vector<SequenceLoss> losses = {{1, kLossPresent}};
  const protozero::ConstBytes compressed_records{
      reinterpret_cast<const uint8_t*>("abcdefgh"), 8};
  const size_t metadata_size = static_cast<size_t>(
      BundleBuffer::GetBundleSize(keys.size(), losses.size(), 0));
  const size_t bundle_size = static_cast<size_t>(BundleBuffer::GetBundleSize(
      keys.size(), losses.size(), compressed_records.size));
  BundleBuffer src;
  ASSERT_TRUE(src.Initialize(2 * bundle_size + metadata_size +
                             compressed_records.size / 2));
  src.Push({keys, losses, 80}, compressed_records);
  src.Push({keys, losses, 81}, compressed_records);
  src.Pop();
  // This one wraps in the middle of its compressed records.
  src.Push({keys, losses, 82}, compressed_records);

  BundleBuffer clone;
  ASSERT_TRUE(clone.CloneFrom(src));
  EXPECT_EQ(clone.capacity(), src.capacity());
  EXPECT_EQ(clone.used_size(), 2 * bundle_size);

  // Overwriting all of the source must not change the clone.
  src.Pop();
  src.Pop();
  std::vector<uint8_t> replacement(src.capacity() - metadata_size, 'z');
  src.Push({{9}, {{9, kLossPresent}}, 99},
           {replacement.data(), replacement.size()});

  for (uint32_t records_size : {81u, 82u}) {
    BundleMetadata metadata;
    clone.PeekFrontMetadata(&metadata);
    EXPECT_EQ(metadata.sequence_keys, keys);
    EXPECT_EQ(Losses(metadata.sequence_losses), Losses(losses));
    EXPECT_EQ(metadata.records_size, records_size);
    EXPECT_EQ(SliceToString(clone.PeekFrontCompressedRecords()), "abcdefgh");
    clone.Pop();
  }
  EXPECT_TRUE(clone.empty());
}

TEST(BundleBufferTest, SizeDoesNotWrap) {
  EXPECT_GT(
      BundleBuffer::GetBundleSize(std::numeric_limits<uint32_t>::max(), 0, 0),
      std::numeric_limits<uint32_t>::max());
  EXPECT_GT(
      BundleBuffer::GetBundleSize(0, std::numeric_limits<uint32_t>::max(), 0),
      std::numeric_limits<uint32_t>::max());
  EXPECT_GT(
      BundleBuffer::GetBundleSize(0, 0, std::numeric_limits<uint32_t>::max()),
      std::numeric_limits<uint32_t>::max());
  EXPECT_GT(BundleBuffer::GetBundleSize(std::numeric_limits<size_t>::max(),
                                        std::numeric_limits<size_t>::max(),
                                        std::numeric_limits<size_t>::max()),
            std::numeric_limits<uint32_t>::max());
}

TEST(DecodeArenaTest, BlocksAreReusedAndTemporariesFreed) {
  DecodeArena arena(100);
  const size_t two_blocks = 200;
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);

  uint8_t* a = arena.Allocate(60);  // Block 0.
  uint8_t* b = arena.Allocate(60);  // Block 1 because it does not fit after a.
  uint8_t* c = arena.Allocate(40);  // Block 0, after a.
  EXPECT_EQ(c, a + 60);
  EXPECT_NE(b, a);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);

  uint8_t* e = arena.Allocate(50);  // Temporary because neither block has room.
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks + 50);
  memset(a, 'a', 60);
  memset(b, 'b', 60);
  memset(c, 'c', 40);
  memset(e, 'e', 50);

  // Keep c's whole block even though a, which precedes it, is already read.
  arena.ResetExcept(c);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);
  EXPECT_EQ(c[39], 'c');
  EXPECT_EQ(arena.Allocate(100), b);

  // Keep the temporary allocation and reset both blocks.
  uint8_t* f = arena.Allocate(300);
  arena.ResetExcept(f);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks + 300);
  EXPECT_EQ(arena.Allocate(100), a);

  arena.CompactExcept(nullptr);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);
  EXPECT_NE(arena.Allocate(10), nullptr);
}

TEST(DecodeArenaTest, CompactionPreservesLiveBlockUntilReset) {
  DecodeArena arena(100);
  uint8_t* a = arena.Allocate(60);
  uint8_t* live = arena.Allocate(40);
  ASSERT_EQ(live, a + 60);
  memset(live, 'a', 40);
  arena.Allocate(100);  // Block 1.

  // Compaction frees block 1 but keeps the live records in block 0.
  arena.CompactExcept(live);
  arena.CompactExcept(live);
  uint8_t* next_live = arena.Allocate(100);
  memset(next_live, 'b', 100);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(live), 40),
            std::string(40, 'a'));

  // Once the reader moves to block 1, block 0 can be reused.
  arena.ResetExcept(next_live);
  EXPECT_EQ(arena.Allocate(100), a);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(next_live), 100),
            std::string(100, 'b'));
}

// ---------------------------------------------------------------------------
// Input formats and the write cycle
// ---------------------------------------------------------------------------

TEST_F(TraceBufferV3Test, EmptyBuffer) {
  ResetBuffer(4096, kOneBundlePerPacket);
  EXPECT_THAT(ReadAll(), IsEmpty());
  trace_buffer_->OnWriteCheckpoint();
  trace_buffer_->MaybeCompact();
  EXPECT_THAT(ReadAll(), IsEmpty());
  EXPECT_FALSE(trace_buffer_->has_data());
  EXPECT_EQ(trace_buffer_->size(), 4096u);
  EXPECT_EQ(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(trace_buffer_->buf_type(), TraceBuffer::kV3);
  EXPECT_EQ(stats().buffer_size(), 4096u);
  EXPECT_EQ(v3_stats().bundles_written(), 0u);
}

// Packets below the target stay in staging until a read bundles them and
// returns them with their writer's identity.
TEST_F(TraceBufferV3Test, ReadBundlesWhatIsLeftInStaging) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  WriteChunk(1, 1, 0, {Packet("a"), Packet("b")});
  WriteChunk(2, 3, 0, {Packet("c")});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_TRUE(trace_buffer_->has_data());
  EXPECT_EQ(v3_stats().bundles_written(), 0u);
  EXPECT_EQ(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(num_empty_staging_sequences(), 0u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "b", "c"));
  EXPECT_EQ(packets[0].producer_id, 1u);
  EXPECT_EQ(packets[0].writer_id, 1u);
  EXPECT_EQ(packets[0].uid, 1001u);
  EXPECT_EQ(packets[2].producer_id, 2u);
  EXPECT_EQ(packets[2].writer_id, 3u);
  EXPECT_EQ(packets[2].uid, 1002u);
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss, 0u);

  EXPECT_EQ(v3_stats().bundles_written(), 1u);
  EXPECT_EQ(trace_buffer_->used_size(), 0u);
  // Once read, no bundle holds the sequences, so staging may prune them.
  EXPECT_EQ(num_bundled_sequences(), 0u);
  EXPECT_EQ(num_empty_staging_sequences(), 2u);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

TEST_F(TraceBufferV3Test, OnWriteCheckpointBundlesAfterEnoughStagingBytes) {
  ResetBuffer(4096, 128);

  // Each chunk takes 64 bytes in staging, 16 + 1 + 45 rounded up.
  ASSERT_EQ(WriteChunk(1, 1, 0, {Packet(std::string(40, 'a'))}), 64u);
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(v3_stats().bundles_written(), 0u);
  EXPECT_EQ(staging()->used_size(), 64u);

  ASSERT_EQ(WriteChunk(1, 1, 1, {Packet(std::string(40, 'b'))}), 64u);
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(v3_stats().bundles_written(), 1u);
  EXPECT_GT(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(staging()->used_size(), 0u);
  EXPECT_EQ(num_bundled_sequences(), 1u);

  // Both records, 48 bytes each, are in one bundle below the 128 byte target.
  EXPECT_EQ(FrontBundleMetadata().records_size, 96u);
  EXPECT_EQ(FrontBundleMetadata().sequence_keys.size(), 1u);
  EXPECT_EQ(v3_stats().bundle_bytes_stored(), trace_buffer_->used_size());

  // The counter restarts after a pass.
  WriteChunk(1, 1, 2, {Packet(std::string(40, 'c'))});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(v3_stats().bundles_written(), 1u);

  EXPECT_THAT(Strs(ReadAll()),
              ElementsAre(std::string(40, 'a'), std::string(40, 'b'),
                          std::string(40, 'c')));
}

// Returning one bundle leaves the rest of staging eligible for a checkpoint,
// even when no more chunks arrive.
TEST_F(TraceBufferV3Test, OnWriteCheckpointBundlesAfterPartialRead) {
  const std::string a(40, 'a');
  const std::string b(40, 'b');
  const std::string c(40, 'c');
  ResetBuffer(4096, 64);
  WriteChunk(1, 1, 0, {Packet(a), Packet(b), Packet(c)});

  // The first two records cross the target and leave the third in staging.
  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, a);
  EXPECT_EQ(v3_stats().bundles_written(), 1u);

  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(v3_stats().bundles_written(), 2u);
  EXPECT_EQ(staging()->used_size(), 0u);
  EXPECT_THAT(Strs(ReadAll()), ElementsAre(b, c));
}

// A scrape copies chunk 2 (packet '7') before chunk 1 (packet '6').
// Chunk 2 crosses the target, but bundling waits for OnWriteCheckpoint(),
// so both chunks are read in ChunkID order without loss.
TEST_F(TraceBufferV3Test, NoReadBeforeOnWriteCheckpoint) {
  ResetBuffer(4096, 64);

  WriteBatch(Packet(std::string(40, '5')));  // Chunk 0.
  ASSERT_EQ(v3_stats().bundles_written(), 1u);
  ASSERT_EQ(staging()->stats().chunks_read(), 1u);

  WriteChunk(1, 1, 2, {Packet(std::string(40, '7'))});
  EXPECT_EQ(v3_stats().bundles_written(), 1u);
  EXPECT_EQ(staging()->stats().chunks_read(), 1u);
  WriteChunk(1, 1, 1, {Packet(std::string(40, '6'))});
  EXPECT_EQ(staging()->stats().chunks_read(), 1u);

  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(staging()->stats().chunks_read(), 3u);
  EXPECT_EQ(stats().chunks_committed_out_of_order(), 1u);
  EXPECT_EQ(stats().chunks_discarded(), 0u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, '5'), std::string(40, '6'),
                          std::string(40, '7')));
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss, 0u);
}

// A split packet waits in staging until its first chunk is patched.
// Earlier packets and other writers' packets can still be bundled.
TEST_F(TraceBufferV3Test, FragmentsWaitForPatches) {
  ResetBuffer(4096, kOneBundlePerPacket);
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
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("a", "other"));
  EXPECT_EQ(staging()->stats().readaheads_failed(), 2u);
  EXPECT_EQ(stats().readaheads_failed(), 0u);

  // Patch "XXXX" after [a]'s 7 bytes, the fragment's size byte and the
  // packet's 5-byte protobuf prefix.
  // A patch adds no staging bytes toward a checkpoint, so the read bundles it.
  const TraceBuffer::Patch patch{7 + 1 + 5, {'r', 'e', 'a', 'd'}};
  ASSERT_TRUE(trace_buffer_->TryPatchChunkContents(
      1, 1, 0, &patch, 1, /*other_patches_pending=*/false));
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("read-and-the-rest", "b"));
  EXPECT_EQ(staging()->stats().readaheads_succeeded(), 1u);
  EXPECT_EQ(stats().patches_succeeded(), 1u);
}

TEST_F(TraceBufferV3Test, IncompleteScrapedChunk) {
  ResetBuffer(4096, kOneBundlePerPacket);
  // The writer is still on the third packet, so the scrape takes two.
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("a"))
      .AddFragment(Packet("b"))
      .AddFragment(Packet("unfinished"))
      .PadTo(256)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("a", "b"));

  CreateChunk(1, 1, 0)
      .AddFragment(Packet("a"))
      .AddFragment(Packet("b"))
      .AddFragment(Packet("c"))
      .CopyIntoTraceBuffer();
  trace_buffer_->OnWriteCheckpoint();
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("c"));
  EXPECT_EQ(packets[0].loss, 0u);
  EXPECT_EQ(stats().chunks_discarded(), 0u);
}

// A scrape bundles completed chunks before their commit arrives.
// The late commit must not duplicate those packets or report loss.
TEST_F(TraceBufferV3Test, LateCommitAfterScrapeAndBundling) {
  ResetBuffer(4096, kOneBundlePerPacket);
  WriteChunk(1, 1, 0, {Packet("P")});
  WriteChunk(1, 1, 1, {Packet("Q")});
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_EQ(v3_stats().bundles_written(), 2u);
  const uint64_t bytes_written = stats().bytes_written();

  WriteChunk(1, 1, 0, {Packet("P")});
  WriteChunk(1, 1, 1, {Packet("Q")});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().chunks_discarded(), 2u);
  EXPECT_EQ(stats().chunks_written(), 2u);
  EXPECT_EQ(stats().bytes_written(), bytes_written);
  EXPECT_EQ(v3_stats().bundles_written(), 2u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("P", "Q"));
  EXPECT_EQ(packets[0].loss, 0u);
  EXPECT_EQ(packets[1].loss, 0u);

  // The sequence continues without a gap.
  WriteChunk(1, 1, 2, {Packet("R")});
  trace_buffer_->OnWriteCheckpoint();
  packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("R"));
  EXPECT_EQ(packets[0].loss, 0u);
}

// Staging rewrites SMB v2 packets, so reads return ordinary protobuf.
TEST_F(TraceBufferV3Test, SmbV2Chunks) {
  ResetBuffer(4096, kOneBundlePerPacket);
  ASSERT_TRUE(trace_buffer_->supports_tracing_v2());
  ASSERT_TRUE(CopyChunkV2(1, 1, {PacketV2("whole")}));
  // A packet in two fragments, one per batch of the writer.
  const std::string split = PacketV2("in-two-fragments");
  ASSERT_TRUE(CopyChunkV2(1, 1, {split.substr(0, 8)}, kContOnNextChunk));
  ASSERT_TRUE(CopyChunkV2(1, 1, {split.substr(8)}, kContFromPrevChunk));
  // An SMB v1 writer of the same producer.
  WriteChunk(1, 2, 0, {Packet("v1")});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(v3_stats().bundles_written(), 3u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("whole", "in-two-fragments", "v1"));
  EXPECT_EQ(packets[0].producer_id, 1u);
  EXPECT_EQ(packets[0].writer_id, 1u);
  EXPECT_EQ(packets[0].uid, 1001u);

  // A gap in the SMB v2 sequence is reported on the packet after it.
  trace_buffer_->RecordChunkV2DataLoss(1, 1);
  ASSERT_TRUE(CopyChunkV2(1, 1, {PacketV2("after-gap")}));
  trace_buffer_->OnWriteCheckpoint();
  packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("after-gap"));
  EXPECT_EQ(packets[0].loss, kLossPresent | kLossReadGap);

  trace_buffer_->RecordAbiViolation();
  EXPECT_EQ(stats().abi_violations(), 1u);
}

// ---------------------------------------------------------------------------
// Bundle size
// ---------------------------------------------------------------------------

TEST_F(TraceBufferV3Test, PacketThatCrossesTheTargetStaysWhole) {
  ResetBuffer(4096, 100);

  // Each record takes 3 header bytes and 45 packet bytes, so the third crosses
  // the 100 byte target.
  // The second bundle gets the other two and ends because staging is empty.
  std::vector<std::string> packets;
  for (char c : {'a', 'b', 'c', 'd', 'e'})
    packets.push_back(Packet(std::string(40, c)));
  WriteChunk(1, 1, 0, packets);
  trace_buffer_->OnWriteCheckpoint();

  EXPECT_EQ(v3_stats().bundles_written(), 2u);
  EXPECT_EQ(v3_stats().bundle_bytes_uncompressed(), 5 * 48u);
  EXPECT_EQ(v3_stats().bundle_bytes_stored(), trace_buffer_->used_size());
  EXPECT_EQ(FrontBundleMetadata().records_size, 3 * 48u);

  trace_buffer_->BeginRead();
  TestPacket packet;
  for (int i = 0; i < 3; i++)
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, std::string(40, 'c'));
  EXPECT_EQ(FrontBundleMetadata().records_size, 2 * 48u);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, std::string(40, 'e'));
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(stats().bytes_read(), 5 * 48u);
}

TEST_F(TraceBufferV3Test, PacketsOfManySizesInOneBundle) {
  ResetBuffer(1024 * 1024, kBundleOnlyOnRead);

  // Payloads of 122/123 and 16374/16375/16376 bytes cross the packet_size
  // varint boundaries at 128 and 16384 packet bytes.
  // Packets over 16 KiB also bypass the codec's input buffer.
  const std::vector<size_t> sizes = {0,    1,     2,     100,   122,   123,
                                     4000, 16374, 16375, 16376, 40000, 3};

  std::vector<std::string> expected;
  for (size_t i = 0; i < sizes.size(); i++) {
    expected.push_back(RandomBytes(sizes[i], static_cast<uint32_t>(i + 1)));
    WriteChunk(1, 1, static_cast<ChunkID>(i), {Packet(expected.back())});
  }
  ASSERT_EQ(Packet(expected[5]).size(), 128u);
  ASSERT_EQ(Packet(expected[8]).size(), 16384u);
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(v3_stats().bundles_written(), 0u);

  EXPECT_EQ(Strs(ReadAll()), expected);
  EXPECT_EQ(v3_stats().bundles_written(), 1u);
}

// A pass blocked on a missing fragment still resets the checkpoint counter,
// so OnWriteCheckpoint() waits for enough new chunks before trying again.
TEST_F(TraceBufferV3Test, BlockedStagingDoesNotCausePassPerOnWriteCheckpoint) {
  ResetBuffer(4096, 64);

  // The first fragment of a packet whose second fragment never arrives.
  ASSERT_EQ(CreateChunk(1, 1, 0)
                .AddFragment(std::string(40, 'x'), kContOnNextChunk)
                .CopyIntoTraceBuffer(),
            57u);  // Takes 64 bytes in staging.
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(staging()->stats().readaheads_failed(), 1u);
  EXPECT_EQ(v3_stats().bundles_written(), 0u);

  // No new chunks, so no pass.
  trace_buffer_->OnWriteCheckpoint();
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(staging()->stats().readaheads_failed(), 1u);

  // The next 64 bytes start a pass that bundles the other writer and leaves
  // the blocked chunk in staging.
  WriteChunk(1, 2, 0, {Packet(std::string(40, 'b'))});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(staging()->stats().readaheads_failed(), 2u);
  EXPECT_EQ(v3_stats().bundles_written(), 1u);
  EXPECT_EQ(staging()->used_size(), 64u);
  EXPECT_THAT(Strs(ReadAll()), ElementsAre(std::string(40, 'b')));
}

// ---------------------------------------------------------------------------
// Bundle buffer wrapping, overwrites and capacity
// ---------------------------------------------------------------------------

// Bundles of different sizes wrap at different points, sometimes in the
// header and sometimes in the compressed records.
// Intermittent reads must return surviving packets in order.
TEST_F(TraceBufferV3Test, ManyWrapsKeepPacketsIntact) {
  ResetBuffer(331, kOneBundlePerPacket);
  std::minstd_rand rnd(7);
  uint32_t next_to_write = 0;
  uint32_t next_to_read = 0;
  uint32_t num_lost = 0;

  // Packet |index| is "<index>-<random bytes>" with |index| as the random seed,
  // so the reader can check that it got the whole packet.
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
      EXPECT_EQ(packet.loss != 0, index != next_to_read) << index;
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
  EXPECT_EQ(v3_stats().bundles_overwritten(), num_lost);
  EXPECT_EQ(v3_stats().bundles_too_large(), 0u);
}

TEST_F(TraceBufferV3Test, OverwritesSeveralBundles) {
  const std::string small = Packet(std::string(40, 's'));
  const std::string big = Packet(RandomBytes(120, 1));
  const size_t small_size = BundleSize(kOneBundlePerPacket, {small});
  const size_t big_size = BundleSize(kOneBundlePerPacket, {big});
  const size_t num_overwritten = (big_size + small_size - 1) / small_size;
  ASSERT_GE(num_overwritten, 2u);

  ResetBuffer((num_overwritten + 1) * small_size, kOneBundlePerPacket);
  for (size_t i = 0; i < num_overwritten + 1; i++)
    WriteBatch(small);
  EXPECT_EQ(bundle_buf().free_size(), 0u);

  WriteBatch(big);
  EXPECT_EQ(v3_stats().bundles_overwritten(), num_overwritten);
  EXPECT_EQ(trace_buffer_->used_size(), small_size + big_size);
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, 's'), RandomBytes(120, 1)));
  EXPECT_EQ(packets[0].loss, kLossPresent | kLossOverwrite);
  EXPECT_EQ(packets[1].loss, 0u);
}

// Dropping older bundles cannot make a bundle that is too large fit, so they
// stay.
// P1 precedes the drop, so report loss on P4, the next packet from staging.
// That loss includes the READ_GAP that P2's record carried.
TEST_F(TraceBufferV3Test, BundleLargerThanTheBuffer) {
  const std::string p1 = Packet(std::string(40, '1'));
  const std::string p4 = Packet(std::string(40, '4'));
  const size_t small_size = BundleSize(kOneBundlePerPacket, {p1});
  const size_t capacity = 3 * small_size;
  const std::string p2 = Packet(RandomBytes(capacity, 2));
  ASSERT_GT(BundleSize(kOneBundlePerPacket, {p2}), capacity);
  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    SCOPED_TRACE(policy);
    ResetBuffer(capacity, kOneBundlePerPacket, policy);
    WriteChunk(1, 1, 0, {p1});
    trace_buffer_->OnWriteCheckpoint();
    // Chunk 1 is missing, so P2's record carries a READ_GAP.
    WriteChunk(1, 1, 2, {p2});
    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(v3_stats().bundles_too_large(), 1u);
    EXPECT_EQ(v3_stats().bundles_overwritten(), 0u);
    EXPECT_EQ(v3_stats().bundles_written(), 1u);
    EXPECT_EQ(trace_buffer_->used_size(), small_size);
    EXPECT_FALSE(has_overflow_bundle());
    EXPECT_EQ(num_bundled_sequences(), 1u);

    WriteChunk(1, 1, 3, {p4});
    trace_buffer_->OnWriteCheckpoint();
    std::vector<TestPacket> packets = ReadAll();
    ASSERT_THAT(Strs(packets),
                ElementsAre(std::string(40, '1'), std::string(40, '4')));
    EXPECT_EQ(packets[0].loss, 0u);
    EXPECT_EQ(packets[1].loss, kLossPresent | kLossReadGap);
    EXPECT_EQ(num_bundled_sequences(), 0u);
  }
}

// A blocked sequence is tried once per read batch, even when the batch
// reads several bundles from staging.
TEST_F(TraceBufferV3Test, ReadKeepsStagingCursorAcrossBundles) {
  ResetBuffer(4096, 64);
  const std::string split = Packet("fragmented");
  const std::string a(80, 'a');
  const std::string b(80, 'b');
  CreateChunk(1, 1, 0)
      .AddFragment(split.substr(0, 9), kContOnNextChunk)
      .CopyIntoTraceBuffer();
  const size_t blocked_chunk_size = staging()->used_size();
  WriteChunk(1, 2, 0, {Packet(a), Packet(b), Packet("tail")});

  trace_buffer_->BeginRead();
  TestPacket packet;
  for (const auto& expected : {a, b, std::string("tail")}) {
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
    EXPECT_EQ(packet.str, expected);
    EXPECT_EQ(packet.loss, 0u);
    EXPECT_EQ(staging()->stats().readaheads_failed(), 1u);
  }

  // The final partial bundle completes the staging read and compacts it.
  // Further reads must not resume that cursor or retry the blocked sequence.
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(v3_stats().bundles_written(), 3u);
  EXPECT_EQ(stats().compactions(), 1u);
  EXPECT_EQ(staging()->stats().readaheads_failed(), 1u);
  EXPECT_EQ(staging()->used_size(), blocked_chunk_size);

  // A new batch can read the sequence once its missing fragment arrives.
  CreateChunk(1, 1, 1)
      .AddFragment(split.substr(9), kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("fragmented"));
  EXPECT_EQ(packets[0].loss, 0u);
  EXPECT_EQ(staging()->stats().readaheads_succeeded(), 1u);
  EXPECT_EQ(staging()->used_size(), 0u);
}

// A read keeps bundling staging until it stores a bundle, so it skips bundles
// too large to store, or returns nothing if staging has only those.
TEST_F(TraceBufferV3Test, ReadSkipsOversizedBundles) {
  const size_t target = 64;
  const std::string small = Packet("kept");
  const std::string large = Packet(RandomBytes(512, 1));
  const size_t capacity = 3 * BundleSize(target, {small});
  ASSERT_GT(BundleSize(target, {large}), capacity);
  ResetBuffer(capacity, target);

  WriteChunk(1, 1, 0, {large, large, small});
  ASSERT_EQ(v3_stats().bundles_written(), 0u);
  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "kept");
  EXPECT_EQ(packet.loss, kLossPresent);
  EXPECT_EQ(v3_stats().bundles_too_large(), 2u);
  EXPECT_EQ(v3_stats().bundles_written(), 1u);
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));

  WriteChunk(1, 1, 1, {large});
  EXPECT_THAT(ReadAll(), IsEmpty());
  EXPECT_EQ(v3_stats().bundles_too_large(), 3u);

  WriteChunk(1, 1, 2, {small});
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("kept"));
  EXPECT_EQ(packets[0].loss, kLossPresent);
}

// kDiscard keeps a bundle that exceeds free space but fits the capacity.
// Readout returns this overflow bundle after stored bundles and before staging.
// Further chunks are rejected even after reads free space.
TEST_F(TraceBufferV3Test, DiscardKeepsOverflowBundle) {
  const std::string a = Packet(std::string(40, 'a'));
  const std::string b = Packet(std::string(40, 'b'));
  const std::string c = Packet(std::string(40, 'c'));
  const size_t size_ab = BundleSize(kOneBundlePerPacket, {a}) +
                         BundleSize(kOneBundlePerPacket, {b});
  const size_t size_c = BundleSize(kOneBundlePerPacket, {c});
  ResetBuffer(size_ab + 4, kOneBundlePerPacket, TraceBuffer::kDiscard);

  WriteBatch(a);
  WriteBatch(b);
  const size_t memory_without_overflow = trace_buffer_->GetMemoryUsageBytes();
  // |c| fits the buffer but not the 4 free bytes, so the pass keeps it as the
  // overflow bundle and stops before it reads |d|.
  WriteChunk(1, 1, next_chunk_id_++, {c});
  WriteChunk(1, 1, next_chunk_id_++, {Packet("d")});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_TRUE(has_overflow_bundle());
  EXPECT_TRUE(staging_discard_writes());
  EXPECT_GT(trace_buffer_->GetMemoryUsageBytes(), memory_without_overflow);
  EXPECT_EQ(trace_buffer_->used_size(), size_ab);
  EXPECT_EQ(v3_stats().bundles_written(), 3u);
  EXPECT_EQ(v3_stats().bundle_bytes_stored(), size_ab + size_c);
  EXPECT_EQ(num_bundled_sequences(), 1u);

  WriteBatch(Packet("rejected"));
  EXPECT_EQ(stats().chunks_discarded(), 1u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, 'a'), std::string(40, 'b'),
                          std::string(40, 'c'), "d"));
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss, 0u);
  EXPECT_FALSE(has_overflow_bundle());
  trace_buffer_->BeginRead();
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), memory_without_overflow);

  // Neither reads nor compaction make us accept writes again.
  WriteBatch(a);
  EXPECT_THAT(ReadAll(), IsEmpty());
  trace_buffer_->MaybeCompact();
  WriteBatch(a);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// A kDiscard batch can exceed staging capacity before its checkpoint.
// A space pass makes room while the stored bundles still fit.
TEST_F(TraceBufferV3Test, DiscardSpacePassDrainsStaging) {
  ResetBuffer(4096, 2048, TraceBuffer::kDiscard);

  // Each chunk takes 1040 bytes, so three fit in the 4096 bytes of staging.
  const std::string big = Packet(std::string(1000, 'x'));
  for (ChunkID chunk_id = 0; chunk_id < 5; chunk_id++)
    WriteChunk(1, 1, chunk_id, {big});
  EXPECT_EQ(stats().chunks_written(), 5u);
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  EXPECT_EQ(v3_stats().staging_space_passes(), 1u);
  EXPECT_EQ(v3_stats().bundles_written(), 1u);
  EXPECT_FALSE(staging_discard_writes());

  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(v3_stats().bundles_written(), 2u);
  EXPECT_EQ(staging()->used_size(), 0u);

  const auto packets = ReadAll();
  ASSERT_EQ(packets.size(), 5u);
  for (const auto& packet : packets) {
    EXPECT_EQ(packet.str, std::string(1000, 'x'));
    EXPECT_EQ(packet.loss, 0u);
  }

  WriteChunk(1, 1, 5, {Packet("late")});
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("late"));
}

// kDiscard stops accepting writes once the stored bundles add up to the
// capacity, even if reads already freed their space.
// Chunks accepted before that stay readable, also after compaction.
TEST_F(TraceBufferV3Test, DiscardReadsDoNotRestoreWriteAllowance) {
  const std::string payload(40, 'a');
  const std::string packet = Packet(payload);
  const size_t bundle_size = BundleSize(kOneBundlePerPacket, {packet});
  ResetBuffer(2 * bundle_size, kOneBundlePerPacket, TraceBuffer::kDiscard);
  WriteBatch(packet);
  ASSERT_THAT(Strs(ReadAll()), ElementsAre(payload));
  trace_buffer_->MaybeCompact();
  EXPECT_EQ(trace_buffer_->used_size(), 0u);
  EXPECT_FALSE(staging_discard_writes());

  // Both chunks arrive before the pass that reaches the capacity.
  WriteChunk(1, 1, next_chunk_id_++, {packet});
  WriteChunk(1, 1, next_chunk_id_++, {packet});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(trace_buffer_->used_size(), bundle_size);
  EXPECT_EQ(v3_stats().bundle_bytes_stored(), 2 * bundle_size);
  EXPECT_TRUE(staging_discard_writes());

  trace_buffer_->MaybeCompact();
  WriteChunk(1, 1, next_chunk_id_++, {packet});
  EXPECT_EQ(stats().chunks_discarded(), 1u);
  EXPECT_THAT(Strs(ReadAll()), ElementsAre(payload, payload));
}

// Once zstd copies packets, each pass compacts staging under either policy.
// The remaining 8 KiB of fragments move to the front while waiting for their
// packets to complete.
TEST_F(TraceBufferV3Test, CompactsStagingAfterEachPass) {
  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    SCOPED_TRACE(policy == TraceBuffer::kDiscard ? "kDiscard" : "kOverwrite");
    ResetBuffer(64 * 1024, kOneBundlePerPacket, policy);

    for (WriterID writer = 10; writer < 18; writer++)
      WriteStartOfSplitPacket(writer);
    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(staging()->used_size(), 8u * 1024);
    const uint64_t compactions = stats().compactions();

    // Each batch is a pass that bundles one chunk of 64 bytes.
    for (ChunkID chunk_id = 0; chunk_id < 10; chunk_id++) {
      WriteChunk(1, 1, chunk_id, {Packet(std::string(40, 'a'))});
      trace_buffer_->OnWriteCheckpoint();
      EXPECT_EQ(staging()->used_size(), 8u * 1024);
    }
    EXPECT_EQ(stats().compactions(), compactions + 10);

    // Moved chunks keep their contents, so their packets can complete and
    // follow the 10 packets already bundled.
    for (WriterID writer = 10; writer < 18; writer++)
      WriteEndOfSplitPacket(writer);
    trace_buffer_->OnWriteCheckpoint();
    const std::vector<TestPacket> packets = ReadAll();
    ASSERT_EQ(packets.size(), 10u + 8);
    for (size_t i = 0; i < packets.size(); i++) {
      EXPECT_EQ(packets[i].str, std::string(i < 10 ? 40 : 1100, 'a'));
      EXPECT_EQ(packets[i].loss, 0u);
    }
    EXPECT_EQ(stats().chunks_overwritten(), 0u);
    EXPECT_EQ(stats().chunks_discarded(), 0u);
  }
}

// ---------------------------------------------------------------------------
// Reads and slice lifetimes
// ---------------------------------------------------------------------------

// File read batches fit from the first read, including when the next batch
// resumes an unread bundle or follows compaction.
TEST_F(TraceBufferV3Test, DecodeArenaFitsFileReadBatchesFromFirstRead) {
  constexpr size_t kBatchBytes = TracingServiceImpl::kWriteIntoFileChunkSize;
  trace_buffer_ =
      TraceBufferV3::Create(256 * 1024, TraceBuffer::kOverwrite, CreateCodec());
  ASSERT_TRUE(trace_buffer_);
  const size_t initial_memory = trace_buffer_->GetMemoryUsageBytes();
  const size_t decode_memory = decode_memory_bytes();
  // A bundle holds 11 of these packets and a batch reads 349, so every batch
  // ends partway through a bundle.
  const std::string input = Packet(std::string(3000, 'a'));
  const size_t packets_per_batch =
      (kBatchBytes + input.size() - 1) / input.size();
  // Three batches reuse both decode blocks, and then one batch follows
  // compaction.
  for (size_t num_batches : {3u, 1u}) {
    SCOPED_TRACE(num_batches);
    const size_t total_packets = num_batches * packets_per_batch + 32;
    for (size_t i = 0; i < total_packets; ++i)
      WriteBatch(input);
    ASSERT_EQ(stats().chunks_overwritten(), 0u);
    ASSERT_EQ(v3_stats().bundles_overwritten(), 0u);
    ASSERT_EQ(FrontBundleMetadata().records_size, 11 * Record(0, input).size());

    size_t packets_read = 0;
    for (size_t batch = 0; batch < num_batches; ++batch) {
      SCOPED_TRACE(batch);
      trace_buffer_->BeginRead();
      std::vector<TracePacket> packets;
      for (size_t bytes = 0; bytes < kBatchBytes;) {
        TracePacket packet;
        TraceBuffer::PacketSequenceProperties properties{};
        uint32_t loss = 0;
        ASSERT_TRUE(
            trace_buffer_->ReadNextTracePacket(&packet, &properties, &loss));
        EXPECT_EQ(loss, 0u);
        bytes += packet.size();
        packets.push_back(std::move(packet));
        ++packets_read;
        // Block capacities are counted before allocation, so any increase
        // within the batch would be a new temporary.
        EXPECT_EQ(decode_memory_bytes(), decode_memory);
      }
      // Earlier slices must survive every later decode in the same batch.
      for (const TracePacket& packet : packets)
        EXPECT_EQ(packet.GetRawBytesForTesting(), input);
    }
    EXPECT_EQ(packets_read + ReadAll().size(), total_packets);
    trace_buffer_->MaybeCompact();
    EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), initial_memory);
  }
}

// A bundle can exceed the target by its last record, so each decode block fits
// twice the target.
// Each middle batch finishes one bundle and reads half the next, so BeginRead()
// must keep that bundle's block and reuse the other without a temporary.
TEST_F(TraceBufferV3Test, TargetCrossingBundlesReuseDecodeBlocksAcrossBatches) {
  constexpr size_t kTargetBytes = 1024 * 1024;
  constexpr size_t kPacketsPerBundle = 18;
  constexpr size_t kNumBundles = 4;
  trace_buffer_ = TraceBufferV3::Create(8 * kTargetBytes,
                                        TraceBuffer::kOverwrite, CreateCodec());
  ASSERT_TRUE(trace_buffer_);
  ASSERT_EQ(target_records_size(), kTargetBytes);
  const size_t decode_memory = decode_memory_bytes();

  std::vector<std::string> expected;
  for (size_t bundle = 0; bundle < kNumBundles; ++bundle) {
    expected.push_back(
        Packet(std::string(60000, static_cast<char>('A' + bundle))));
    // Checkpoint only after all 18 records, so it cannot cut a bundle short.
    for (size_t i = 0; i < kPacketsPerBundle; ++i)
      WriteChunk(1, 1, next_chunk_id_++, {expected.back()});
    trace_buffer_->OnWriteCheckpoint();
    ASSERT_EQ(v3_stats().bundles_written(), bundle + 1);
  }
  ASSERT_GT(FrontBundleMetadata().records_size, kTargetBytes);
  ASSERT_EQ(v3_stats().bundles_overwritten(), 0u);

  const size_t batch_sizes[] = {9, 18, 18, 18, 9};
  size_t packets_read = 0;
  for (size_t batch_size : batch_sizes) {
    SCOPED_TRACE(packets_read);
    trace_buffer_->BeginRead();
    std::vector<TracePacket> packets(batch_size);
    for (TracePacket& packet : packets) {
      TestPacket result;
      ASSERT_TRUE(ReadOne(trace_buffer_.get(), &result, &packet));
      // Both blocks count already, so an increase means a temporary allocation.
      EXPECT_EQ(decode_memory_bytes(), decode_memory);
    }
    for (size_t i = 0; i < packets.size(); ++i) {
      EXPECT_EQ(packets[i].GetRawBytesForTesting(),
                expected[(packets_read + i) / kPacketsPerBundle]);
    }
    packets_read += batch_size;
  }
  TestPacket packet;
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
}

// When a batch stops inside a bundle, the next batch continues in the same
// decoded bytes even with writes in between.
TEST_F(TraceBufferV3Test, ReadResumesInsideBundle) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  WriteChunk(1, 1, 0, {Packet("p1"), Packet("p2"), Packet("p3"), Packet("p4")});
  trace_buffer_->OnWriteCheckpoint();

  trace_buffer_->BeginRead();
  TestPacket packet;
  TracePacket p2;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet, &p2));
  EXPECT_EQ(packet.str, "p2");
  ASSERT_EQ(p2.slices().size(), 1u);
  const uint8_t* p2_end =
      static_cast<const uint8_t*>(p2.slices()[0].start) + p2.slices()[0].size;
  EXPECT_EQ(v3_stats().bundles_written(), 1u);
  EXPECT_EQ(num_bundled_sequences(), 1u);

  // This write cycle between the two read tasks stays in staging.
  WriteChunk(1, 1, 1, {Packet("p5")});
  trace_buffer_->OnWriteCheckpoint();

  trace_buffer_->BeginRead();
  TracePacket p3;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet, &p3));
  EXPECT_EQ(packet.str, "p3");
  // Resuming keeps p3 at p2's end plus its three-byte record header.
  EXPECT_EQ(static_cast<const uint8_t*>(p3.slices()[0].start), p2_end + 3);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "p4");
  EXPECT_EQ(v3_stats().bundles_written(), 1u);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "p5");
  EXPECT_EQ(v3_stats().bundles_written(), 2u);
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
}

// A packet beyond block size needs a temporary allocation and multiple slices.
// Once its bundle is fully read, BeginRead() frees the temporary.
TEST_F(TraceBufferV3Test, LargePacketUsesTemporaryStorage) {
  ResetBuffer(3 * 1024 * 1024, kBundleOnlyOnRead);
  const size_t initial_memory = trace_buffer_->GetMemoryUsageBytes();
  const size_t initial_decode_memory = decode_memory_bytes();

  // Larger than a 2 MiB decode block, in fragments of at most one chunk.
  const std::string large = Packet(RandomBytes(2500000, 3));
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
  trace_buffer_->OnWriteCheckpoint();

  trace_buffer_->BeginRead();
  TracePacket large_packet;
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet, &large_packet));
  EXPECT_EQ(large_packet.GetRawBytesForTesting(), large);
  // The packet exceeds the IPC slice limit, so it must span several slices.
  EXPECT_GT(large_packet.slices().size(), 1u);
  EXPECT_GT(decode_memory_bytes(), initial_decode_memory + large.size());
  EXPECT_GT(trace_buffer_->GetMemoryUsageBytes(), initial_memory);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "small");
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(large_packet.GetRawBytesForTesting(), large);

  trace_buffer_->BeginRead();
  EXPECT_EQ(decode_memory_bytes(), initial_decode_memory);
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), initial_memory);
}

// B1 stays decoded while writes overwrite stored B2, so B1's unread packet
// precedes the gap in both the original and its clone.
TEST_F(TraceBufferV3Test, DecodedBundleSurvivesOverwrite) {
  // In bundle 1, writer 1 has s1 and s2, and writer 2 has t1.
  auto write_bundle_1 = [this] {
    WriteChunk(1, 1, 0, {Packet("s1"), Packet("s2")});
    WriteChunk(1, 2, 0, {Packet("t1")});
    BundleNow();
  };
  ResetBuffer(4096, kBundleOnlyOnRead);
  write_bundle_1();
  ASSERT_EQ(v3_stats().bundles_written(), 1u);
  const size_t bundle_1_size = trace_buffer_->used_size();

  // A buffer that bundle 1 fills, except for a few bytes.
  ResetBuffer(bundle_1_size + 8, kBundleOnlyOnRead);
  write_bundle_1();
  ASSERT_EQ(trace_buffer_->used_size(), bundle_1_size);

  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "s2");
  // The consumer task ends after reading all of writer 1, leaving t1 unread.

  // B2 uses the space released when B1 was decoded.
  WriteChunk(1, 1, 1, {Packet("s3"), Packet("s4")});
  WriteChunk(1, 2, 1, {Packet("t2")});
  BundleNow();
  EXPECT_EQ(v3_stats().bundles_written(), 2u);
  EXPECT_EQ(v3_stats().bundles_overwritten(), 0u);
  WriteChunk(1, 2, 2, {Packet("t3")});
  BundleNow();
  EXPECT_EQ(v3_stats().bundles_overwritten(), 1u);
  EXPECT_EQ(v3_stats().bundles_too_large(), 0u);

  auto clone = trace_buffer_->CloneReadOnly();
  for (TraceBuffer* buffer :
       {trace_buffer_.get(), static_cast<TraceBufferV3*>(clone.get())}) {
    std::vector<TestPacket> packets = perfetto::ReadAll(buffer);
    ASSERT_THAT(Strs(packets), ElementsAre("t1", "t3"));
    EXPECT_EQ(packets[0].loss, 0u);
    EXPECT_EQ(packets[1].loss, kLossPresent | kLossOverwrite);
  }
  EXPECT_EQ(num_bundled_sequences(), 0u);
}

// Buffers that share a codec can bundle and read in turn.
// |other| decodes its bundles while |trace_buffer_| has one partly read.
TEST_F(TraceBufferV3Test, BuffersShareCodec) {
  ResetBuffer(64 * 1024, kBundleOnlyOnRead);
  std::unique_ptr<TraceBufferV3> other =
      CreateBufferSharingCodec(64 * 1024, kOneBundlePerPacket);
  ASSERT_TRUE(other);
  auto write_other = [&other](ChunkID c, const std::string& packet) {
    FakeChunk(other.get(), 2, 1, c).AddFragment(packet).CopyIntoTraceBuffer();
    other->OnWriteCheckpoint();
  };

  // The two buffers' bundles alternate on the codec.
  WriteChunk(1, 1, 0, {Packet("s1"), Packet("s2")});
  BundleNow();
  write_other(0, Packet("t1"));
  write_other(1, Packet("t2"));
  WriteChunk(1, 1, 1, {Packet("s3")});
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_written(), 2u);
  ASSERT_EQ(other->stats().experimental_v3_stats().bundles_written(), 2u);

  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "s1");

  EXPECT_THAT(Strs(perfetto::ReadAll(other.get())), ElementsAre("t1", "t2"));

  std::vector<std::string> rest;
  while (ReadOne(trace_buffer_.get(), &packet))
    rest.push_back(packet.str);
  EXPECT_THAT(rest, ElementsAre("s2", "s3"));
}

// ---------------------------------------------------------------------------
// Data loss
// ---------------------------------------------------------------------------

// Reassembling empty SMB v1 fragments can return an empty packet with loss.
// Skipping that packet must keep its loss for the next nonempty packet.
TEST_F(TraceBufferV3Test, EmptyPacketKeepsLossForNextPacket) {
  ResetBuffer(4096, kOneBundlePerPacket);
  WriteChunk(1, 1, 0, {Packet("a")});
  // Chunk 1 is missing, and chunks 2 and 3 hold two empty fragments.
  CreateChunk(1, 1, 2).AddFragment("", kContOnNextChunk).CopyIntoTraceBuffer();
  CreateChunk(1, 1, 3)
      .AddFragment("", kContFromPrevChunk)
      .AddFragment(Packet("b"))
      .AddFragment(Packet("c"))
      .CopyIntoTraceBuffer();
  trace_buffer_->OnWriteCheckpoint();

  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "b", "c"));
  EXPECT_EQ(packets[0].loss, 0u);
  EXPECT_EQ(packets[1].loss, kLossPresent | kLossReadGap);
  EXPECT_EQ(packets[2].loss, 0u);
}

// An overwrite removes the sequence's last bundle, so its loss waits in staging
// for the next packet.
TEST_F(TraceBufferV3Test, BundleLossOfSequenceWithNoBundleLeft) {
  const std::string a = Packet(std::string(40, 'a'));
  const size_t size_a = BundleSize(kOneBundlePerPacket, {a});
  // Room for one bundle, so the next one overwrites it.
  ResetBuffer(size_a + 16, kOneBundlePerPacket);
  WriteBatch(a);
  ASSERT_EQ(num_bundled_sequences(), 1u);

  // Another writer overwrites the only bundle of {1, 1}.
  WriteChunk(2, 1, 0, {Packet(std::string(40, 'x'))});
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_EQ(v3_stats().bundles_overwritten(), 1u);
  EXPECT_EQ(num_bundled_sequences(), 1u);  // Only {2, 1}.

  WriteBatch(Packet(std::string(40, 'b')));  // Overwrites the bundle of {2, 1}.
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre(std::string(40, 'b')));
  EXPECT_EQ(packets[0].loss, kLossPresent | kLossOverwrite);
}

// Overwriting a bundle also reports the loss that its records carried, so the
// next packet still shows the gap before them.
TEST_F(TraceBufferV3Test, OverwriteReportsLossOfOverwrittenRecords) {
  const std::string a = Packet(std::string(40, 'a'));
  const std::string b = Packet(std::string(40, 'b'));
  const std::string c = Packet(std::string(40, 'c'));
  // Room for one bundle, so each new bundle overwrites the one before it.
  const size_t max_bundle_size =
      std::max({BundleSize(kOneBundlePerPacket, {a}),
                BundleSize(kOneBundlePerPacket, {b}),
                BundleSize(kOneBundlePerPacket, {c})});
  ResetBuffer(max_bundle_size + 16, kOneBundlePerPacket);
  WriteChunk(1, 1, 0, {a});
  trace_buffer_->OnWriteCheckpoint();
  // Chunk 1 is missing, so the record of |b| carries a READ_GAP.
  WriteChunk(1, 1, 2, {b});
  trace_buffer_->OnWriteCheckpoint();
  WriteChunk(1, 1, 3, {c});
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_EQ(v3_stats().bundles_overwritten(), 2u);

  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre(std::string(40, 'c')));
  EXPECT_EQ(packets[0].loss, kLossPresent | kLossOverwrite | kLossReadGap);
}

// A sequence's records with loss share one loss entry, even when another
// sequence's entry comes between them.
TEST_F(TraceBufferV3Test, LossListHasOneEntryPerSequence) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  // Chunks 1 and 3 are missing, so the records of b, y and c carry a READ_GAP.
  WriteChunk(1, 1, 0, {Packet("a")});
  WriteChunk(1, 2, 0, {Packet("x")});
  WriteChunk(1, 1, 2, {Packet("b")});
  WriteChunk(1, 2, 2, {Packet("y")});
  WriteChunk(1, 1, 4, {Packet("c")});
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_written(), 1u);

  const BundleMetadata metadata = FrontBundleMetadata();
  ASSERT_EQ(metadata.sequence_keys.size(), 2u);
  EXPECT_THAT(Losses(metadata.sequence_losses),
              ElementsAre(std::make_pair(MkProducerAndWriterID(1, 1),
                                         kLossPresent | kLossReadGap),
                          std::make_pair(MkProducerAndWriterID(1, 2),
                                         kLossPresent | kLossReadGap)));

  // Each record still reports its own loss.
  const std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "x", "b", "y", "c"));
  EXPECT_EQ(packets[0].loss, 0u);
  EXPECT_EQ(packets[1].loss, 0u);
  EXPECT_EQ(packets[2].loss, kLossPresent | kLossReadGap);
  EXPECT_EQ(packets[3].loss, kLossPresent | kLossReadGap);
  EXPECT_EQ(packets[4].loss, kLossPresent | kLossReadGap);
}

// A batch larger than staging drains readable packets before overwriting them.
TEST_F(TraceBufferV3Test, SpacePassPreservesPackets) {
  ResetBuffer(4096, kOneBundlePerPacket);
  WriteBatch(Packet("first"));

  // One batch of 5 chunks of 1040 bytes into 4096 bytes of staging.
  // Their bundles compress to far less, so the bundle buffer keeps them all.
  const std::string big = Packet(std::string(1000, 'x'));
  for (int i = 0; i < 5; i++)
    WriteChunk(1, 1, next_chunk_id_++, {big});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_EQ(v3_stats().bundles_overwritten(), 0u);
  uint64_t passes = 0;
  for (uint64_t count : v3_stats().bundling_time_us_histogram_counts())
    passes += count;
  // The first checkpoint, one space pass and the final checkpoint.
  // The 6 bundles built inside those passes are not separate samples.
  EXPECT_EQ(passes, 3u);
  EXPECT_EQ(v3_stats().staging_space_passes(), 1u);
  EXPECT_EQ(stats().compactions_skipped(), 0u);

  std::vector<TestPacket> packets = ReadAll();
  std::vector<std::string> expected(6, std::string(1000, 'x'));
  expected.front() = "first";
  ASSERT_THAT(Strs(packets), ElementsAreArray(expected));
  for (const auto& packet : packets)
    EXPECT_EQ(packet.loss, 0u);
}

// Chunks that never become readable fill staging, and later ones land on both
// sides of the write cursor, so compaction is skipped.
// After a pass that can't make room, the next 1/8 of staging, 8 of these
// chunks, overwrites without a pass.
TEST_F(TraceBufferV3Test, StuckChunksOnBothSidesLimitSpacePasses) {
  ResetBuffer(64 * 1024, kBundleOnlyOnRead);

  // 64 stuck chunks of 1 KiB fill staging exactly.
  for (WriterID writer = 1; writer <= 64; ++writer)
    WriteStartOfSplitPacket(writer);
  ASSERT_EQ(v3_stats().staging_space_passes(), 0u);

  // These writes replace unreadable chunks with more unreadable chunks.
  // |overwrite_bytes_until_space_pass_| avoids scans that free no space.
  for (WriterID writer = 65; writer <= 128; ++writer)
    WriteStartOfSplitPacket(writer);
  EXPECT_GT(stats().compactions_skipped(), 0u);
  EXPECT_GT(v3_stats().staging_space_passes(), 0u);
  EXPECT_LE(v3_stats().staging_space_passes(), 16u);
}

// Stuck chunks fill all but 1 KiB of staging, all before the write cursor.
// Compaction succeeds, but each pass frees only the chunks it bundles.
// Deferred passes keep the count low, and the overwrites in between reach only
// stuck chunks, so no readable packet is lost.
TEST_F(TraceBufferV3Test, StuckChunksFillingStagingLimitSpacePasses) {
  ResetBuffer(64 * 1024, kBundleOnlyOnRead);
  for (WriterID writer = 1; writer <= 63; ++writer)
    WriteStartOfSplitPacket(writer);

  // Each complete packet frees little space among the incomplete packets.
  // |overwrite_bytes_until_space_pass_| must prevent a scan on every write.
  std::vector<std::string> expected;
  for (ChunkID chunk_id = 0; chunk_id < 64; ++chunk_id) {
    expected.push_back(std::to_string(chunk_id) + std::string(900, 'r'));
    WriteChunk(1, 100, chunk_id, {Packet(expected.back())});
  }
  EXPECT_GT(v3_stats().staging_space_passes(), 0u);
  EXPECT_LE(v3_stats().staging_space_passes(), 16u);

  const std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAreArray(expected));
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss, 0u);
}

// A space pass runs inside a write cycle, so it waits for chunks that a
// scrape copies out of order instead of reading past them.
TEST_F(TraceBufferV3Test, SpacePassWaitsForOutOfOrderChunks) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  WriteChunk(1, 1, 0, {Packet("first")});
  BundleNow();

  // The scrape finds chunk 2 before chunk 1, and another writer fills staging.
  WriteChunk(1, 1, 2, {Packet("third")});
  for (ChunkID id = 0; id < 63; ++id)
    WriteChunk(2, 1, id, {Packet(std::string(40, 'x'))});
  // Only 32 bytes remain, so this chunk forces a space pass.
  WriteChunk(1, 1, 1, {Packet(std::string(80, 's'))});
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  ASSERT_EQ(v3_stats().staging_space_passes(), 1u);

  BundleNow();
  std::vector<std::string> writer_packets;
  for (const auto& packet : ReadAll()) {
    EXPECT_EQ(packet.loss, 0u);
    if (packet.producer_id == 1)
      writer_packets.push_back(packet.str);
  }
  EXPECT_THAT(writer_packets,
              ElementsAre("first", std::string(80, 's'), "third"));
}

// SMB v2 chunks carry gap flags, so a space pass does not wait for
// missing chunk IDs.
TEST_F(TraceBufferV3Test, SpacePassWithSmbV2) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  for (int i = 0; i < 80; ++i)
    ASSERT_TRUE(CopyChunkV2(1, 1, {PacketV2(std::string(40, 'x'))}));
  EXPECT_EQ(v3_stats().staging_space_passes(), 1u);
  EXPECT_GT(v3_stats().bundles_written(), 0u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  const auto packets = ReadAll();
  ASSERT_EQ(packets.size(), 80u);
  for (const auto& packet : packets) {
    EXPECT_EQ(packet.str, std::string(40, 'x'));
    EXPECT_EQ(packet.loss, 0u);
  }
}

// A consumed scrape needs relocation when its commit adds payload.
// The first attempt must keep it intact through a space pass and compaction.
TEST_F(TraceBufferV3Test, SpacePassRetriesScrapedChunkRelocation) {
  ResetBuffer(4096, kOneBundlePerPacket);
  WriteChunk(2, 1, 0, {Packet(std::string(40, 'x'))});
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("first"))
      .AddFragment(Packet("unfinished"))
      .PadTo(256)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  // Read these two bundles and stop before staging can compact.
  // The scrape stays at offset 64.
  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_EQ(packet.producer_id, 2u);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_EQ(packet.str, "first");
  ASSERT_EQ(stats().compactions(), 0u);
  for (ChunkID id = 1; id < 60; ++id)
    WriteChunk(2, 1, id, {Packet(std::string(40, 'x'))});
  ASSERT_EQ(staging()->used_size(), staging()->size());

  CreateChunk(1, 1, 0)
      .AddFragment(Packet("first"))
      .AddFragment(Packet("second"))
      .CopyIntoTraceBuffer();
  EXPECT_EQ(stats().chunks_relocated(), 1u);
  EXPECT_EQ(stats().chunks_written(), 62u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  const auto packets = ReadAll();
  ASSERT_EQ(packets.size(), 60u);
  std::vector<std::string> writer_packets;
  for (const auto& result : packets) {
    EXPECT_EQ(result.loss, 0u);
    if (result.producer_id == 1)
      writer_packets.push_back(result.str);
  }
  EXPECT_THAT(writer_packets, ElementsAre("second"));
}

// Completing a scrape in place needs no allocation even when staging is full,
// so it must not start a space pass or reject accepted data.
TEST_F(TraceBufferV3Test, FullStagingAllowsInPlaceRecommit) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("first"))
      .AddFragment(Packet("unfinished"))
      .PadTo(256)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  for (ChunkID id = 0; id < 60; ++id)
    WriteChunk(2, 1, id, {Packet(std::string(40, 'x'))});
  ASSERT_EQ(staging()->used_size(), staging()->size());
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("first"))
      .AddFragment(Packet("second"))
      .CopyIntoTraceBuffer();
  EXPECT_EQ(v3_stats().bundles_written(), 0u);
  EXPECT_EQ(stats().chunks_rewritten(), 1u);
  EXPECT_EQ(stats().chunks_relocated(), 0u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  const auto packets = ReadAll();
  ASSERT_EQ(packets.size(), 62u);
  EXPECT_EQ(packets[0].str, "first");
  EXPECT_EQ(packets[1].str, "second");
}

// Reject framing errors before a space pass, so rejected input does not
// cause staging packets to be bundled.
TEST_F(TraceBufferV3Test, RejectedInputDoesNotDrainStaging) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  for (ChunkID id = 0; id < 64; ++id)
    WriteChunk(1, 1, id, {Packet(std::string(40, 'x'))});
  ASSERT_EQ(staging()->used_size(), staging()->size());
  SuppressClientDchecksForTesting();
  WriteChunk(2, 1, 0, {Packet(std::string(5000, 'y'))});
  EXPECT_FALSE(CopyChunkV2(2, 1, {std::string(5000, 'z')}));
  // Writer {1, 1} already uses SMB v1, so it cannot switch formats.
  EXPECT_FALSE(CopyChunkV2(1, 1, {PacketV2("wrong-format")}));
  EXPECT_EQ(stats().abi_violations(), 3u);
  EXPECT_EQ(num_staging_sequences(), 1u);
  EXPECT_EQ(v3_stats().bundles_written(), 0u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_EQ(ReadAll().size(), 64u);
}

TEST_F(TraceBufferV3Test, NeedsSpaceRetryAccountsInputOnce) {
  for (bool writer_abort : {false, true}) {
    SCOPED_TRACE(writer_abort);
    ResetBuffer(4096, kBundleOnlyOnRead);
    for (ChunkID id = 0; id < 64; ++id)
      WriteChunk(1, 1, id, {Packet(std::string(40, 'x'))});
    ASSERT_EQ(staging()->used_size(), staging()->size());
    SuppressClientDchecksForTesting();
    auto chunk = CreateChunk(1, 1, 64);
    chunk.AddFragment(Packet("valid-prefix"));
    if (writer_abort)
      chunk.AddPacket({0xff, 0xff, 0xff, 0x7f});  // kPacketSizeDropPacket.
    else
      chunk.IncrementNumPackets();  // Malformed fragment after the prefix.
    chunk.CopyIntoTraceBuffer();
    EXPECT_GT(v3_stats().bundles_written(), 0u);
    EXPECT_EQ(stats().abi_violations(), writer_abort ? 0u : 1u);
    EXPECT_EQ(stats().trace_writer_packet_loss(), writer_abort ? 1u : 0u);
    EXPECT_EQ(stats().chunks_written(), 65u);
    EXPECT_EQ(stats().chunks_overwritten(), 0u);
    uint64_t inputs = 0;
    for (auto it = trace_buffer_->writer_stats().GetIterator(); it; ++it) {
      const auto& hist = it.value().chunk_payload_histogram;
      for (size_t i = 0; i < hist.num_buckets(); ++i)
        inputs += hist.GetBucketCount(i);
    }
    EXPECT_EQ(inputs, 65u);
    const auto packets = ReadAll();
    ASSERT_EQ(packets.size(), 65u);
    EXPECT_EQ(packets.back().str, "valid-prefix");
    // Accounting an abort before the drain would put its loss on an older
    // packet, even though the incoming write has not been accepted yet.
    for (size_t i = 0; i < 64; ++i)
      EXPECT_EQ(packets[i].loss, 0u);
    EXPECT_EQ(
        packets.back().loss,
        writer_abort
            ? kLossPresent | protos::pbzero::TracePacket::DATA_LOSS_WRITER_ABORT
            : 0u);
  }
}

// A pending patch blocks this sequence, so the space pass bundles nothing.
// The retry overwrites blocked chunks, and the next packet reports the loss.
TEST_F(TraceBufferV3Test, BlockedSpacePassReportsLoss) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("missing-tail"),
                   kContOnNextChunk | kChunkNeedsPatching)
      .CopyIntoTraceBuffer();
  // Packets behind the unfinished packet cannot leave staging yet.
  for (ChunkID id = 1; id <= 70; ++id)
    WriteChunk(1, 1, id, {Packet(std::string(40, 'x'))});
  EXPECT_GT(stats().chunks_overwritten(), 0u);
  const auto packets = ReadAll();
  ASSERT_FALSE(packets.empty());
  EXPECT_EQ(packets[0].loss, kLossPresent | kLossOverwrite);
}

// Exactly full kDiscard staging still accepts writes after a checkpoint bundles
// its packets and reclaims the space.
TEST_F(TraceBufferV3Test, DiscardExactlyFullStagingDoesNotStopIntake) {
  const std::string packet = Packet(std::string(40, 'a'));
  ResetBuffer(4096, kOneBundlePerPacket, TraceBuffer::kDiscard);
  for (ChunkID id = 0; id < 64; ++id)
    WriteChunk(1, 1, id, {packet});
  ASSERT_EQ(staging()->used_size(), staging()->size());
  EXPECT_FALSE(staging_discard_writes());
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(staging()->used_size(), 0u);
  WriteChunk(1, 1, 64, {packet});
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  EXPECT_EQ(ReadAll().size(), 65u);
}

// A space pass can stop accepting writes before the write retry.
// The retry must reject the prepared chunk.
TEST_F(TraceBufferV3Test, DiscardSpacePassStopsAtCompressedCapacity) {
  const std::string payload(40, 'a');
  const std::string packet = Packet(payload);
  for (bool v2 : {false, true}) {
    SCOPED_TRACE(v2);
    const size_t capacity = BundleSize(kOneBundlePerPacket, {packet});
    ResetBuffer(capacity, kOneBundlePerPacket, TraceBuffer::kDiscard);
    for (ChunkID id = 0; id < 64; ++id) {
      if (v2)
        ASSERT_TRUE(CopyChunkV2(1, 1, {PacketV2(payload)}));
      else
        WriteChunk(1, 1, id, {packet});
    }
    ASSERT_EQ(staging()->used_size(), staging()->size());
    EXPECT_EQ(v3_stats().bundles_written(), 0u);
    EXPECT_FALSE(staging_discard_writes());

    if (v2)
      EXPECT_FALSE(CopyChunkV2(2, 1, {PacketV2("rejected")}));
    else
      WriteChunk(2, 1, 0, {Packet("rejected")});
    EXPECT_EQ(v3_stats().staging_space_passes(), 1u);
    EXPECT_EQ(v3_stats().bundles_written(), 1u);
    EXPECT_EQ(v3_stats().bundle_bytes_stored(), capacity);
    EXPECT_EQ(stats().chunks_written(), 64u);
    EXPECT_EQ(stats().chunks_discarded(), 1u);
    EXPECT_TRUE(staging_discard_writes());

    // Checkpoints stop, but accepted packets remain readable.
    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(v3_stats().bundles_written(), 1u);
    const auto packets = ReadAll();
    ASSERT_EQ(packets.size(), 64u);
    for (const auto& result : packets) {
      EXPECT_EQ(result.str, payload);
      EXPECT_EQ(result.loss, 0u);
    }
  }
}

// A complete split packet waits for its final patch notification.
// If a space pass cannot bundle it, the next chunk stops kDiscard writes.
TEST_F(TraceBufferV3Test, DiscardStopsWhenStagingWaitsForPatches) {
  ResetBuffer(4096, kOneBundlePerPacket, TraceBuffer::kDiscard);
  const std::string payload(4000, 'x');
  const std::string packet = Packet(payload);
  CreateChunk(1, 1, 0)
      .AddFragment(packet.substr(0, 2000),
                   kContOnNextChunk | kChunkNeedsPatching)
      .CopyIntoTraceBuffer();
  CreateChunk(1, 1, 1)
      .AddFragment(packet.substr(2000), kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  ASSERT_EQ(staging()->used_size(), 4064u);
  EXPECT_FALSE(staging_discard_writes());

  WriteChunk(2, 1, 0, {Packet(std::string(40, 'a'))});
  EXPECT_EQ(v3_stats().staging_space_passes(), 1u);
  EXPECT_EQ(v3_stats().bundles_written(), 0u);
  EXPECT_EQ(stats().chunks_written(), 2u);
  EXPECT_EQ(stats().chunks_discarded(), 1u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_TRUE(staging_discard_writes());

  // Patches still unblock accepted data after we stop accepting writes.
  ASSERT_TRUE(trace_buffer_->TryPatchChunkContents(1, 1, 0, nullptr, 0, false));
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(v3_stats().bundles_written(), 0u);
  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre(payload));
  EXPECT_EQ(packets[0].loss, 0u);
}

// ---------------------------------------------------------------------------
// Invalid packets
// ---------------------------------------------------------------------------

// The service receives the exact malformed bytes, with loss outside them.
// In particular, a loss field must not complete the truncated packet.
TEST_F(TraceBufferV3Test, ReturnsInvalidPacketsWithoutStampingLoss) {
  ResetBuffer(4096, kOneBundlePerPacket);
  const std::string good = Packet("a");
  const std::string sets_trusted_uid = "\x18\x01";
  const std::string truncated = "\xa2\x38\x06\x0a\x01\x41";
  WriteChunk(1, 1, 0, {good});
  WriteChunk(1, 1, 2, {truncated, sets_trusted_uid, Packet("b")});
  trace_buffer_->OnWriteCheckpoint();

  trace_buffer_->BeginRead();
  TracePacket packet;
  TraceBuffer::PacketSequenceProperties properties{};
  uint32_t loss = 0;
  for (const auto& bytes : {good, truncated, sets_trusted_uid, Packet("b")}) {
    packet.Clear();
    ASSERT_TRUE(
        trace_buffer_->ReadNextTracePacket(&packet, &properties, &loss));
    EXPECT_EQ(packet.GetRawBytesForTesting(), bytes);
    EXPECT_EQ(loss, bytes == truncated ? kLossPresent | kLossReadGap : 0u);
  }
  packet.Clear();
  EXPECT_FALSE(trace_buffer_->ReadNextTracePacket(&packet, &properties, &loss));
}

// ---------------------------------------------------------------------------
// Sequences
// ---------------------------------------------------------------------------

TEST_F(TraceBufferV3Test, InterleavedSequencesSurviveMapGrowth) {
  ResetBuffer(128 * 1024, kBundleOnlyOnRead);

  // Grow both sequence maps within one bundle.
  // Consecutive packets also exercise the cached entry after each insertion.
  constexpr WriterID kNumWriters = 1024;
  std::vector<std::pair<WriterID, std::string>> expected;
  std::vector<ProducerAndWriterID> expected_keys;
  for (WriterID writer = 1; writer <= kNumWriters; ++writer) {
    const std::string str = std::to_string(writer);
    WriteChunk(1, writer, 0, {Packet(str), Packet(str)});
    expected.emplace_back(writer, str);
    expected.emplace_back(writer, str);
    expected_keys.push_back(MkProducerAndWriterID(1, writer));
  }
  // This writer must reuse its index after the map grows.
  WriteChunk(1, 1, 1, {Packet("returning-writer")});
  expected.emplace_back(1, "returning-writer");
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_written(), 1u);
  EXPECT_EQ(FrontBundleMetadata().sequence_keys, expected_keys);
  EXPECT_EQ(num_bundled_sequences(), kNumWriters);
  const size_t first_bundle_packets = expected.size();

  // Reuse the construction map with different indexes for these writers.
  // Their shared entries now have references from both bundles.
  WriteChunk(1, kNumWriters, 1, {Packet("next-last")});
  WriteChunk(1, 1, 2, {Packet("next-first")});
  expected.emplace_back(kNumWriters, "next-last");
  expected.emplace_back(1, "next-first");
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_written(), 2u);

  trace_buffer_->BeginRead();
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    TestPacket packet;
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
    EXPECT_EQ(packet.writer_id, expected[i].first);
    EXPECT_EQ(packet.str, expected[i].second);
    EXPECT_EQ(packet.producer_id, 1u);
    EXPECT_EQ(packet.uid, 1001u);
    EXPECT_EQ(packet.loss, 0u);
    // Only the writers in the second bundle keep their entries.
    if (i + 1 == first_bundle_packets)
      EXPECT_EQ(num_bundled_sequences(), 2u);
  }
  EXPECT_EQ(num_bundled_sequences(), 0u);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// ---------------------------------------------------------------------------
// Cloning
// ---------------------------------------------------------------------------

// The clone keeps stored bundles, staging and the position in a partly read
// bundle, so it and the source can continue reading independently.
TEST_F(TraceBufferV3Test, CloneContinuesWhereTheSourceStopped) {
  ResetBuffer(4096, kBundleOnlyOnRead);
  WriteChunk(1, 1, 0, {Packet("a1"), Packet("a2"), Packet("a3")});
  BundleNow();
  WriteChunk(1, 1, 1, {Packet("b")});
  BundleNow();
  WriteChunk(1, 1, 2, {Packet("in-staging")});
  ASSERT_EQ(v3_stats().bundles_written(), 2u);

  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_EQ(packet.str, "a1");

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  ASSERT_TRUE(clone);
  // The clone keeps the write counters but counts its own reads.
  EXPECT_EQ(clone->stats().experimental_v3_stats().bundles_written(), 2u);
  EXPECT_EQ(clone->stats().bytes_read(), 0u);

  // The source reads the rest, takes new data and then goes away.
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, "a2");
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("a3", "b", "in-staging"));
  WriteChunk(1, 1, 3, {Packet("only-in-source")});
  trace_buffer_.reset();

  std::vector<TestPacket> cloned = perfetto::ReadAll(clone.get());
  ASSERT_THAT(Strs(cloned), ElementsAre("a2", "a3", "b", "in-staging"));
  for (const TestPacket& cloned_packet : cloned) {
    EXPECT_EQ(cloned_packet.loss, 0u);
    EXPECT_EQ(cloned_packet.uid, 1001u);
  }
  EXPECT_THAT(perfetto::ReadAll(clone.get()), IsEmpty());
}

TEST_F(TraceBufferV3Test, CloneWithOverflowBundle) {
  const std::string first = RandomBytes(500, 1);
  const std::string a = Packet(first);
  ResetBuffer(BundleSize(kBundleOnlyOnRead, {a}) + 4, kBundleOnlyOnRead,
              TraceBuffer::kDiscard);
  WriteChunk(1, 1, 0, {a});
  BundleNow();
  // Both sequences share an overflow bundle that fits the capacity.
  WriteChunk(1, 2, 0, {Packet(std::string(40, 'b'))});
  WriteChunk(1, 3, 0, {Packet(std::string(40, 'c'))});
  BundleNow();
  ASSERT_TRUE(has_overflow_bundle());

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  trace_buffer_.reset();
  std::vector<TestPacket> packets = perfetto::ReadAll(clone.get());
  ASSERT_THAT(Strs(packets),
              ElementsAre(first, std::string(40, 'b'), std::string(40, 'c')));
  EXPECT_EQ(packets[1].writer_id, 2u);
  EXPECT_EQ(packets[2].writer_id, 3u);
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss, 0u);
}

// transfer_on_clone marks the moved buffer read-only without copying it.
// Reads must still return both bundled and staged packets.
TEST_F(TraceBufferV3Test, SetReadOnly) {
  ResetBuffer(4096, kOneBundlePerPacket);
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

// A space pass bundles P2 after P1, so eviction feeds the ProtoVM in that
// order.
TEST_F(TraceBufferV3Test, ProtoVmGetsSpacePassOverwritesInOrder) {
  const std::string p1 = PatchPacket("P1");
  const std::string p2 = PatchPacket("P2");
  const std::string other = Packet(std::string(40, 'x'));
  // Room for one bundle of either producer, but not two.
  const size_t max_bundle_size =
      std::max(BundleSize(kOneBundlePerPacket, {p1}),
               BundleSize(kOneBundlePerPacket, {other}));
  ResetBuffer(max_bundle_size + 16, kOneBundlePerPacket);
  trace_buffer_->protovms()->MaybeSetUp(
      "data_source", CreateProtoVmProgram().SerializeAsString(), 1024,
      /*producer_id=*/1);

  WriteBatch(p1);
  ASSERT_EQ(v3_stats().bundles_written(), 1u);
  // Leave P2 in staging until the space pass.
  WriteChunk(1, 1, next_chunk_id_++, {p2});

  // Another producer fills staging, so a space pass bundles P2 before
  // reusing its space.
  for (ChunkID chunk_id = 0; chunk_id < 64; chunk_id++)
    WriteChunk(2, 1, chunk_id, {other});
  ASSERT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "P2");

  // The checkpoint finishes the other producer's remaining staging data.
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_GT(v3_stats().bundles_overwritten(), 0u);
  ASSERT_EQ(v3_stats().bundles_too_large(), 0u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "P2");

  // The next packet of the writer reports the loss of P1 and of P2.
  WriteBatch(PatchPacket("P3"));
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("P3"));
  EXPECT_EQ(packets[0].loss, kLossPresent | kLossOverwrite);
}

// P2 stays decoded while P4 overwrites stored P3, so the ProtoVM sees only P3.
// A clone keeps both P2 and the ProtoVM state.
TEST_F(TraceBufferV3Test, ProtoVmDoesNotGetRetainedDecodedPackets) {
  const std::vector<std::string> bundle_1 = {
      PatchPacket("P1", 7), PatchPacket("P2-" + std::string(40, '2'))};
  ResetBuffer(BundleSize(kBundleOnlyOnRead, bundle_1), kBundleOnlyOnRead);
  trace_buffer_->protovms()->MaybeSetUp(
      "data_source", CreateProtoVmProgram().SerializeAsString(), 1024,
      /*producer_id=*/1);
  WriteChunk(1, 1, 0, bundle_1);
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_written(), 1u);

  trace_buffer_->BeginRead();
  TestPacket packet;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_EQ(packet.str, "P1");

  WriteChunk(1, 1, 1, {PatchPacket("P3")});
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_overwritten(), 0u);
  WriteChunk(1, 1, 2, {PatchPacket(std::string(60, '4'))});
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_overwritten(), 1u);
  ASSERT_EQ(v3_stats().bundles_too_large(), 0u);
  auto state = ProtoVmState(trace_buffer_.get());
  EXPECT_EQ(state.string_merged(), "P3");
  // The ProtoVM never saw P1, so it has no int_to_merge.
  EXPECT_FALSE(state.has_int_merged());

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  trace_buffer_.reset();
  EXPECT_EQ(ProtoVmState(clone.get()).string_merged(), "P3");
  std::vector<TestPacket> packets = perfetto::ReadAll(clone.get());
  ASSERT_THAT(Strs(packets),
              ElementsAre("P2-" + std::string(40, '2'), std::string(60, '4')));
  EXPECT_EQ(packets[0].loss, 0u);
  EXPECT_EQ(packets[1].loss, kLossPresent | kLossOverwrite);
}

// Overwrites send unread packets to each producer's ProtoVM in order.
// Staging already rewrote SMB v2 packets as ordinary protobuf.
// Producers without a ProtoVM are skipped.
TEST_F(TraceBufferV3Test, ProtoVmGetsWholeBundleAndSmbV2Packets) {
  // Size the buffer from the largest bundle so all later bundles fit.
  const std::vector<std::string> bundle_1 = {PatchPacket(std::string(60, '0')),
                                             PatchPacket("P1", 7),
                                             PatchPacket("P2")};
  ResetBuffer(BundleSize(kBundleOnlyOnRead, bundle_1), kBundleOnlyOnRead);
  trace_buffer_->protovms()->MaybeSetUp(
      "data_source", CreateProtoVmProgram().SerializeAsString(), 1024,
      /*producer_id=*/1);
  WriteChunk(1, 1, 0, bundle_1);
  BundleNow();

  // Producer 2 has no ProtoVM, and its bundle overwrites bundle 1.
  WriteChunk(2, 1, 0, {Packet("x")});
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_overwritten(), 1u);
  auto state = ProtoVmState(trace_buffer_.get());
  EXPECT_EQ(state.string_merged(), "P2");
  EXPECT_EQ(state.int_merged(), 7);

  // for_testing { protovm_patch { string_to_merge: "v2" } } in proto groups.
  // A standalone TraceBufferV2 rejects SMB v2 producers with a ProtoVM, but
  // staging has no ProtoVM and accepts these chunks.
  const std::string v2_patch = std::string("\xa3\x38\x33\x0a\x02", 5) + "v2" +
                               std::string("\x04\x04", 2);
  ASSERT_TRUE(CopyChunkV2(1, 9, {v2_patch}));
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_overwritten(), 2u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "P2");

  WriteChunk(2, 1, 1, {Packet("y")});
  BundleNow();
  ASSERT_EQ(v3_stats().bundles_overwritten(), 3u);
  ASSERT_EQ(v3_stats().bundles_too_large(), 0u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "v2");
}

// ProtoVM overwrites decompress into one scratch buffer, which grows for a
// larger bundle.
// GetMemoryUsageBytes() counts it until MaybeCompact() frees it, and the next
// overwrite allocates it again.
TEST_F(TraceBufferV3Test, ProtoVmOverwriteScratchGrowsAndIsFreedByCompaction) {
  // Each bundle takes more than half the buffer, so it overwrites the one
  // before it.
  const std::string small = RandomBytes(400, 1);
  const std::string large = RandomBytes(600, 2);
  ResetBuffer(BundleSize(kOneBundlePerPacket, {PatchPacket(large)}),
              kOneBundlePerPacket);
  trace_buffer_->protovms()->MaybeSetUp(
      "data_source", CreateProtoVmProgram().SerializeAsString(), 1024,
      /*producer_id=*/1);

  WriteBatch(PatchPacket(small));
  WriteBatch(PatchPacket(large));
  ASSERT_EQ(v3_stats().bundles_overwritten(), 1u);
  const uint32_t large_records_size = FrontBundleMetadata().records_size;

  // The scratch grows from the records of |small| to those of |large|.
  WriteBatch(PatchPacket(small));
  ASSERT_EQ(v3_stats().bundles_overwritten(), 2u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), large);

  const size_t memory_with_scratch = trace_buffer_->GetMemoryUsageBytes();
  trace_buffer_->MaybeCompact();
  EXPECT_EQ(memory_with_scratch - trace_buffer_->GetMemoryUsageBytes(),
            large_records_size);

  WriteBatch(PatchPacket(large));
  ASSERT_EQ(v3_stats().bundles_overwritten(), 3u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), small);
}

// ---------------------------------------------------------------------------
// Counters and memory
// ---------------------------------------------------------------------------

TEST_F(TraceBufferV3Test, Stats) {
  const std::string a = Packet(std::string(40, 'a'));
  const size_t size_a = BundleSize(kOneBundlePerPacket, {a});
  ResetBuffer(2 * size_a, kOneBundlePerPacket);

  // Three chunks of 64 bytes, each one a bundle with a record of 48 bytes.
  // The third bundle overwrites the first.
  for (int i = 0; i < 3; i++)
    WriteBatch(a);
  {
    const TraceStats::BufferStats& s = stats();
    const auto& v3 = s.experimental_v3_stats();
    EXPECT_EQ(s.buffer_size(), 2 * size_a);
    EXPECT_EQ(s.chunks_written(), 3u);
    EXPECT_EQ(s.bytes_written(), 3 * 64u);
    EXPECT_EQ(staging()->stats().chunks_read(), 3u);
    EXPECT_EQ(staging()->stats().bytes_read(), 3 * 64u);
    EXPECT_EQ(s.chunks_overwritten(), 0u);
    EXPECT_EQ(v3.bundles_written(), 3u);
    EXPECT_EQ(v3.bundle_bytes_uncompressed(), 3 * 48u);
    EXPECT_EQ(v3.bundle_bytes_stored(), 3 * size_a);
    EXPECT_EQ(v3.bundles_overwritten(), 1u);
    EXPECT_EQ(v3.bundles_too_large(), 0u);
    EXPECT_EQ(s.compactions(), 3u);
    // Nothing was returned to the consumer yet.
    EXPECT_EQ(s.bytes_read(), 0u);
    EXPECT_EQ(s.chunks_read(), 0u);
    EXPECT_EQ(s.readaheads_succeeded(), 0u);
    EXPECT_EQ(s.readaheads_failed(), 0u);
  }
  EXPECT_EQ(ReadAll().size(), 2u);
  EXPECT_EQ(stats().bytes_read(), 2 * 48u);
  EXPECT_EQ(stats().chunks_read(), 0u);
  EXPECT_EQ(staging()->stats().chunks_read(), 3u);

  TraceStats trace_stats;
  *trace_stats.add_buffer_stats() = stats();
  TraceStats decoded;
  ASSERT_TRUE(decoded.ParseFromString(trace_stats.SerializeAsString()));
  const auto& serialized = decoded.buffer_stats()[0].experimental_v3_stats();
  EXPECT_THAT(serialized.bundling_time_us_histogram_def(),
              ElementsAre(10, 100, 1000, 10000, 100000));
  ASSERT_EQ(serialized.bundling_time_us_histogram_counts().size(), 6u);
  ASSERT_EQ(serialized.bundling_time_us_histogram_sum().size(), 6u);
  uint64_t count = 0;
  for (uint64_t bucket_count : serialized.bundling_time_us_histogram_counts())
    count += bucket_count;
  // Three checkpoints and one read of empty staging give four measurements,
  // regardless of how many bundles each pass evicts.
  EXPECT_EQ(count, 4u);
}

// The memory budget includes lazy allocations, so the first write and read
// must not increase it.
TEST_F(TraceBufferV3Test, MemoryUsage) {
  trace_buffer_ = TraceBufferV3::Create(1024 * 1024, TraceBuffer::kOverwrite,
                                        CreateCodec());
  ASSERT_TRUE(trace_buffer_);
  const size_t initial = trace_buffer_->GetMemoryUsageBytes();
  // The bundle buffer, staging and decode blocks.
  // TraceBufferFactory counts the shared codec instead.
  EXPECT_EQ(initial, 2 * 1024 * 1024u + decode_memory_bytes());

  for (ChunkID chunk_id = 0; chunk_id < 200; chunk_id++) {
    WriteChunk(1, 1, chunk_id, {Packet(RandomBytes(2000, chunk_id))});
    trace_buffer_->OnWriteCheckpoint();
  }
  EXPECT_GT(v3_stats().bundles_written(), 1u);
  EXPECT_EQ(ReadAll().size(), 200u);
  trace_buffer_->BeginRead();
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), initial);
  trace_buffer_->MaybeCompact();
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), initial);
}

TEST_F(TraceBufferV3Test, BothBuffersUseConfiguredCapacity) {
  constexpr size_t kMiB = 1024 * 1024;
  const struct {
    size_t size;
    size_t target_records_size;
    size_t decode_memory_bytes;
  } cases[] = {
      {1, 512, 2 * kMiB},
      {4096, 512, 2 * kMiB},
      {4 * kMiB, 512 * 1024, 2 * kMiB},
      {6 * kMiB, 768 * 1024, 3 * kMiB},
      {8 * kMiB, kMiB, 4 * kMiB},
      {16 * kMiB, kMiB, 4 * kMiB},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.size);
    trace_buffer_ = TraceBufferV3::Create(
        test_case.size, TraceBuffer::kOverwrite, CreateCodec());
    ASSERT_TRUE(trace_buffer_);
    const size_t capacity = base::AlignUp(test_case.size, 4096);
    EXPECT_EQ(trace_buffer_->size(), capacity);
    EXPECT_EQ(staging()->size(), capacity);
    EXPECT_EQ(target_records_size(), test_case.target_records_size);
    EXPECT_EQ(decode_memory_bytes(), test_case.decode_memory_bytes);
    EXPECT_GE(trace_buffer_->GetMemoryUsageBytes(),
              2 * capacity + test_case.decode_memory_bytes);
  }
}

// ---------------------------------------------------------------------------
// Same output as TraceBufferV2
// ---------------------------------------------------------------------------

// Bundling can change interleaving between sequences, so compare packets,
// identity and loss in sequence order.
TEST_F(TraceBufferV3Test, SameOutputAsTraceBufferV2) {
  ResetBuffer(4 * 1024 * 1024, 3000);
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
  for (int batch = 0; batch < 120; batch++) {
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
        // Split the last packet so a later chunk must complete it.
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
      buffer->OnWriteCheckpoint();
    if (rnd() % 10 == 0)
      read_some(1 + rnd() % 50);
  }
  read_some(1000 * 1000);

  EXPECT_GT(v3_stats().bundles_written(), 50u);
  EXPECT_EQ(v3_stats().bundles_overwritten(), 0u);
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
      EXPECT_EQ(v2_packets[i].loss, v3_packets[i].loss);
      num_packets++;
      num_losses += v3_packets[i].loss != 0;
    }
  }
  EXPECT_GT(num_packets, 500u);
  EXPECT_GT(num_losses, 5u);
}

}  // namespace perfetto
