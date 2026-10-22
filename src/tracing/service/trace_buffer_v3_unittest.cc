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
#include "perfetto/ext/tracing/core/slice.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protovm/vm.h"
#include "src/tracing/service/trace_buffer_v2.h"
#include "src/tracing/service/tracing_service_impl.h"
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

// One record of a bundle: [sequence_index][loss_reasons][packet_size][packet].
std::string Record(uint32_t sequence_index, const std::string& packet) {
  uint8_t header[20];
  uint8_t* end = protozero::proto_utils::WriteVarInt(sequence_index, header);
  end = protozero::proto_utils::WriteVarInt(0, end);
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

  // The service appends nonzero buffer loss after validation, and the last
  // occurrence of a field wins. Otherwise the producer's value stays.
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
  static_assert(
      TraceBufferV3::kReadSliceSize <=
          TracingServiceImpl::kMaxTracePacketSliceSize,
      "TBv3 output slices must fit the service's consumer slice limit");

  using Params = TraceBufferV3::Params;
  using BundleMetadata = BundledBuffer::BundleMetadata;

  // Every packet becomes its own bundle, and every OnWriteCheckpoint() after a
  // chunk starts a pass. The tests of the bundled buffer use this to place
  // bundles one at a time.
  static Params OneBundlePerPacket() {
    Params params{};
    params.staging_capacity_bytes = 4096;
    params.bundle_target_bytes = 1;
    return params;
  }

  // Sets the target above the amount the tests write, so packets stay in
  // staging until a read or an explicit BundleNow().
  static Params BundleOnlyOnRead() {
    Params params{};
    params.staging_capacity_bytes = 8192;
    params.bundle_target_bytes = 1024 * 1024;
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
    trace_buffer_->OnWriteCheckpoint();
  }

  // Runs the OnWriteCheckpoint() bundling pass regardless of the target. With
  // BundleOnlyOnRead() a test then decides which packets share a bundle.
  void BundleNow() {
    trace_buffer_->BundleStagingForWrite(
        TraceBufferV3::WriteBundlingReason::kCheckpoint);
  }

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
    probe->BeginRead();
    probe->BundleStagingForRead();
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
    ASSERT_LE(BundledBuffer::GetStoredSize(ids.size(), frame->size()),
              bundled().free_size());
    bundled().Push(
        uncompressed_size.value_or(static_cast<uint32_t>(records.size())), ids,
        {reinterpret_cast<const uint8_t*>(frame->data()), frame->size()});
    for (ProducerAndWriterID id : ids) {
      auto* sequence = trace_buffer_->bundled_sequences_.Find(id);
      ASSERT_TRUE(sequence);
      ++sequence->bundle_ref_count;
    }
    // Lets a header that is wrong by a few bytes reach the decoder.
    trace_buffer_->bundle_counters_.max_bundle_uncompressed_size =
        std::max(trace_buffer_->bundle_counters_.max_bundle_uncompressed_size,
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

  const Params& params() const { return trace_buffer_->params_; }
  BundledBuffer& bundled() { return trace_buffer_->bundled_buf_; }
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

  void ExpectNoOpenBundle() {
    EXPECT_TRUE(trace_buffer_->open_bundle_.sequence_list.empty());
    EXPECT_EQ(trace_buffer_->open_bundle_.sequences.size(), 0u);
    EXPECT_EQ(trace_buffer_->open_bundle_.uncompressed_size, 0u);
  }

  void ExpectNoDecodedBundle() {
    EXPECT_EQ(trace_buffer_->decoded_bundle_.records, nullptr);
    EXPECT_EQ(trace_buffer_->decoded_bundle_.read_offset, 0u);
    EXPECT_EQ(trace_buffer_->decoded_bundle_.uncompressed_size, 0u);
    EXPECT_TRUE(trace_buffer_->decoded_bundle_.sequences.empty());
  }

  BundleMetadata FrontBundleMetadata() {
    BundleMetadata metadata;
    bundled().ReadFrontMetadata(&metadata);
    return metadata;
  }

  size_t decode_memory_bytes() {
    return trace_buffer_->decode_arena_.GetMemoryUsageBytes();
  }

  const TraceStats::BufferStats& stats() { return trace_buffer_->stats(); }

 protected:
  std::unique_ptr<TraceBufferV3> trace_buffer_;
  ChunkID next_chunk_id_ = 0;
};

// ---------------------------------------------------------------------------
// BundledBuffer and DecodeArena
// ---------------------------------------------------------------------------

// The header, sequence list and frame can each wrap around the buffer.
TEST(BundledBufferTest, PushAndReadAcrossTheEnd) {
  const std::vector<ProducerAndWriterID> first_ids = {1, 2};
  const std::vector<ProducerAndWriterID> last_ids = {3, 4};
  const protozero::ConstBytes first_frame{
      reinterpret_cast<const uint8_t*>("abcdefgh"), 8};
  const protozero::ConstBytes last_frame{
      reinterpret_cast<const uint8_t*>("01234567"), 8};
  const size_t header_size =
      static_cast<size_t>(BundledBuffer::GetStoredSize(0, 0));
  const size_t metadata_size =
      static_cast<size_t>(BundledBuffer::GetStoredSize(first_ids.size(), 0));
  const size_t stored_size = static_cast<size_t>(
      BundledBuffer::GetStoredSize(first_ids.size(), first_frame.size));

  for (size_t bytes_to_end : {size_t{0}, header_size / 2,
                              header_size + sizeof(ProducerAndWriterID) / 2,
                              metadata_size + last_frame.size / 2}) {
    SCOPED_TRACE(bytes_to_end);
    BundledBuffer buf;
    ASSERT_TRUE(buf.Initialize(2 * stored_size + bytes_to_end));
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.free_size(), buf.capacity());

    buf.Push(80, first_ids, first_frame);
    buf.Push(81, first_ids, first_frame);
    buf.Pop();
    // Keep the second bundle so the cursors do not reset before this append.
    buf.Push(82, last_ids, last_frame);
    EXPECT_EQ(buf.used_size(), 2 * stored_size);
    EXPECT_EQ(buf.free_size(), bytes_to_end);

    BundledBuffer::BundleMetadata metadata;
    buf.ReadFrontMetadata(&metadata);
    EXPECT_EQ(metadata.uncompressed_size, 81u);
    EXPECT_EQ(metadata.sequence_list, first_ids);
    const auto front_frame = buf.ReadFrontFrame();
    EXPECT_EQ(std::string(static_cast<const char*>(front_frame.start),
                          front_frame.size),
              "abcdefgh");
    EXPECT_EQ(front_frame.start, buf.ReadFrontFrame().start);
    EXPECT_EQ(buf.used_size(), 2 * stored_size);
    buf.Pop();

    buf.ReadFrontMetadata(&metadata);
    EXPECT_EQ(metadata.uncompressed_size, 82u);
    EXPECT_EQ(metadata.sequence_list, last_ids);
    const auto last_frame_view = buf.ReadFrontFrame();
    EXPECT_EQ(std::string(static_cast<const char*>(last_frame_view.start),
                          last_frame_view.size),
              "01234567");
    EXPECT_EQ(last_frame_view.start != buf.ReadFrontFrame().start,
              bytes_to_end > metadata_size);
    EXPECT_EQ(buf.used_size(), stored_size);
    buf.Pop();
    EXPECT_TRUE(buf.empty());

    // Draining resets the cursors so a bundle can use the whole capacity.
    std::vector<uint8_t> full_frame(buf.capacity() - metadata_size, 'y');
    buf.Push(100, first_ids, {full_frame.data(), full_frame.size()});
    EXPECT_EQ(buf.free_size(), 0u);
    const auto full_frame_view = buf.ReadFrontFrame();
    EXPECT_EQ(std::string(static_cast<const char*>(full_frame_view.start),
                          full_frame_view.size),
              std::string(full_frame.size(), 'y'));
    EXPECT_EQ(full_frame_view.start, buf.ReadFrontFrame().start);
    buf.Pop();
    buf.MaybeCompact();
    buf.Push(80, first_ids, first_frame);
    const auto reused_frame = buf.ReadFrontFrame();
    EXPECT_EQ(std::string(static_cast<const char*>(reused_frame.start),
                          reused_frame.size),
              "abcdefgh");
  }
}

// A clone has the stored bundles, in order, and the capacity of its source.
TEST(BundledBufferTest, CloneCopiesStoredBundles) {
  const std::vector<ProducerAndWriterID> ids = {1};
  const protozero::ConstBytes frame{
      reinterpret_cast<const uint8_t*>("abcdefgh"), 8};
  const size_t metadata_size =
      static_cast<size_t>(BundledBuffer::GetStoredSize(ids.size(), 0));
  const size_t stored_size =
      static_cast<size_t>(BundledBuffer::GetStoredSize(ids.size(), frame.size));
  BundledBuffer src;
  ASSERT_TRUE(src.Initialize(2 * stored_size + metadata_size + frame.size / 2));
  src.Push(80, ids, frame);
  src.Push(81, ids, frame);
  src.Pop();
  src.Push(82, ids, frame);  // Its frame wraps around the buffer.

  BundledBuffer clone;
  ASSERT_TRUE(clone.CloneFrom(src));
  EXPECT_EQ(clone.capacity(), src.capacity());
  EXPECT_EQ(clone.used_size(), 2 * stored_size);

  // The source can change. The clone has its own memory.
  src.Pop();
  const auto wrapped_frame = src.ReadFrontFrame();
  EXPECT_EQ(std::string(static_cast<const char*>(wrapped_frame.start),
                        wrapped_frame.size),
            "abcdefgh");
  EXPECT_NE(wrapped_frame.start, src.ReadFrontFrame().start);
  src.Pop();
  std::vector<uint8_t> replacement(src.capacity() - metadata_size, 'z');
  src.Push(99, {9}, {replacement.data(), replacement.size()});
  // The returned slice owns the wrapped copy after the source is overwritten.
  EXPECT_EQ(std::string(static_cast<const char*>(wrapped_frame.start),
                        wrapped_frame.size),
            "abcdefgh");

  for (uint32_t uncompressed_size : {81u, 82u}) {
    BundledBuffer::BundleMetadata metadata;
    clone.ReadFrontMetadata(&metadata);
    EXPECT_EQ(metadata.uncompressed_size, uncompressed_size);
    EXPECT_EQ(metadata.sequence_list, ids);
    const auto cloned_frame = clone.ReadFrontFrame();
    EXPECT_EQ(std::string(static_cast<const char*>(cloned_frame.start),
                          cloned_frame.size),
              "abcdefgh");
    EXPECT_EQ(cloned_frame.start, clone.ReadFrontFrame().start);
    clone.Pop();
  }
  EXPECT_TRUE(clone.empty());

  // The clone can store as much as its source.
  std::vector<uint8_t> big(clone.capacity() - metadata_size, 'y');
  clone.Push(100, ids, {big.data(), big.size()});
  EXPECT_EQ(clone.used_size(), src.capacity());
  EXPECT_EQ(clone.free_size(), 0u);

  // A clone of an empty buffer is empty and can store bundles.
  BundledBuffer empty_src;
  ASSERT_TRUE(empty_src.Initialize(4096));
  BundledBuffer empty_clone;
  ASSERT_TRUE(empty_clone.CloneFrom(empty_src));
  EXPECT_TRUE(empty_clone.empty());
  EXPECT_EQ(empty_clone.capacity(), 4096u);
  empty_clone.Push(80, ids, frame);
  EXPECT_EQ(empty_clone.used_size(), stored_size);
  const auto cloned_frame = empty_clone.ReadFrontFrame();
  EXPECT_EQ(std::string(static_cast<const char*>(cloned_frame.start),
                        cloned_frame.size),
            "abcdefgh");
}

TEST(BundledBufferTest, SizeDoesNotWrap) {
  EXPECT_GT(BundledBuffer::GetStoredSize(UINT32_MAX, 0), UINT32_MAX);
  EXPECT_GT(BundledBuffer::GetStoredSize(0, UINT32_MAX), UINT32_MAX);
  EXPECT_GT(BundledBuffer::GetStoredSize(SIZE_MAX, SIZE_MAX), UINT32_MAX);
}

// Resets keep the blocks allocated and free temporaries once no packets
// remain unread and the caller has finished using their slices.
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

  uint8_t* e = arena.Allocate(500);  // Temporary. Larger than a block.
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks + 500);
  memset(a, 'a', 60);
  memset(b, 'b', 60);
  memset(c, 'c', 40);
  memset(e, 'e', 500);

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

  arena.Compact(nullptr);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), two_blocks);
  EXPECT_NE(arena.Allocate(10), nullptr);
}

// A batch can need several temporary allocations while earlier slices remain
// in use.
TEST(DecodeArenaTest, TemporariesStayValidUntilReset) {
  DecodeArena arena(100);
  uint8_t* a = arena.Allocate(100);
  uint8_t* b = arena.Allocate(100);
  uint8_t* c = arena.Allocate(60);
  uint8_t* d = arena.Allocate(60);
  memset(a, 'a', 100);
  memset(b, 'b', 100);
  memset(c, 'c', 60);
  memset(d, 'd', 60);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 320u);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(a), 100),
            std::string(100, 'a'));
  EXPECT_EQ(std::string(reinterpret_cast<char*>(b), 100),
            std::string(100, 'b'));
  EXPECT_EQ(std::string(reinterpret_cast<char*>(c), 60), std::string(60, 'c'));
  EXPECT_EQ(std::string(reinterpret_cast<char*>(d), 60), std::string(60, 'd'));

  arena.ResetExcept(c);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 260u);
  EXPECT_EQ(arena.Allocate(100), a);
  EXPECT_EQ(arena.Allocate(100), b);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(c), 60), std::string(60, 'c'));
  arena.ResetExcept(nullptr);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
}

TEST(DecodeArenaTest, CompactionPreservesLiveTemporary) {
  DecodeArena arena(100);
  arena.Allocate(200);
  uint8_t* live = arena.Allocate(300);
  memset(live, 'x', 300);
  arena.Allocate(500);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 1200u);

  arena.Compact(live);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 500u);
  arena.Allocate(100);
  arena.Allocate(100);
  arena.Compact(live);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 500u);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(live), 300),
            std::string(300, 'x'));
  arena.ResetExcept(nullptr);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
}

TEST(DecodeArenaTest, CompactionPreservesLiveBlockUntilReset) {
  DecodeArena arena(100);
  uint8_t* a = arena.Allocate(60);
  uint8_t* live = arena.Allocate(40);
  ASSERT_EQ(live, a + 60);
  memset(live, 'a', 40);
  arena.Allocate(100);  // Block 1.

  arena.Compact(live);
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
  arena.Compact(live);
  uint8_t* next_live = arena.Allocate(100);
  memset(next_live, 'b', 100);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(live), 40),
            std::string(40, 'a'));
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);

  // Once the reader moves to block 1, block 0 can be reused at the same size.
  arena.ResetExcept(next_live);
  EXPECT_EQ(arena.Allocate(100), a);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(next_live), 100),
            std::string(100, 'b'));
  EXPECT_EQ(arena.GetMemoryUsageBytes(), 200u);
}

// ---------------------------------------------------------------------------
// Input formats and the write batch
// ---------------------------------------------------------------------------

TEST_F(TraceBufferV3Test, EmptyBuffer) {
  ResetBuffer(4096, OneBundlePerPacket());
  EXPECT_THAT(ReadAll(), IsEmpty());
  trace_buffer_->OnWriteCheckpoint();
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
  trace_buffer_->OnWriteCheckpoint();
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

// OnWriteCheckpoint() bundles once enough chunk bytes arrived. The packets then
// are in the bundled buffer and staging is empty.
TEST_F(TraceBufferV3Test, OnWriteCheckpointBundlesAfterEnoughStagingBytes) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 128;
  ResetBuffer(4096, params);

  // Each chunk takes 64 bytes in staging, 16 + 1 + 45 rounded up.
  ASSERT_EQ(WriteChunk(1, 1, 0, {Packet(std::string(40, 'a'))}), 64u);
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().bundles_written(), 0u);
  EXPECT_EQ(staging()->used_size(), 64u);

  ASSERT_EQ(WriteChunk(1, 1, 1, {Packet(std::string(40, 'b'))}), 64u);
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_GT(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(staging()->used_size(), 0u);
  EXPECT_EQ(num_bundled_sequences(), 1u);

  // Both records, 48 bytes each, are in one bundle below the 128 byte target.
  EXPECT_EQ(FrontBundleMetadata().uncompressed_size, 96u);
  EXPECT_EQ(FrontBundleMetadata().sequence_list.size(), 1u);
  EXPECT_EQ(stats().bundle_bytes_stored(), trace_buffer_->used_size());

  // The counter restarts after a pass.
  WriteChunk(1, 1, 2, {Packet(std::string(40, 'c'))});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().bundles_written(), 1u);

  EXPECT_THAT(Strs(ReadAll()),
              ElementsAre(std::string(40, 'a'), std::string(40, 'b'),
                          std::string(40, 'c')));
}

// Returning one bundle leaves the rest of staging eligible for a checkpoint,
// even when no more chunks arrive.
TEST_F(TraceBufferV3Test, OnWriteCheckpointBundlesAfterPartialRead) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 64;
  const std::string a(40, 'a');
  const std::string b(40, 'b');
  const std::string c(40, 'c');
  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    SCOPED_TRACE(policy);
    ResetBuffer(4096, params, policy);
    WriteChunk(1, 1, 0, {Packet(a), Packet(b), Packet(c)});

    // The first two records cross the target and leave the third in staging.
    trace_buffer_->BeginRead();
    TestPacket packet;
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
    EXPECT_EQ(packet.str, a);
    EXPECT_EQ(stats().bundles_written(), 1u);

    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(stats().bundles_written(), 2u);
    EXPECT_EQ(staging()->used_size(), 0u);
    EXPECT_THAT(Strs(ReadAll()), ElementsAre(b, c));
  }
}

// A scrape copies chunk 2 (packet '7') before chunk 1 (packet '6'). Chunk 2
// crosses the target, but nothing is read until OnWriteCheckpoint(), so both
// chunks are bundled in ChunkID order without loss.
TEST_F(TraceBufferV3Test, NoReadBeforeOnWriteCheckpoint) {
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

  trace_buffer_->OnWriteCheckpoint();
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

// A completed batch uses the TBv2 read gap handling. Later batches cannot
// restore chunks that precede the last consumed chunk.
TEST_F(TraceBufferV3Test, CheckpointsAdvancePastMissingPredecessors) {
  for (bool missing_first : {false, true}) {
    SCOPED_TRACE(missing_first);
    ResetBuffer(4096, OneBundlePerPacket());
    if (!missing_first)
      WriteChunk(1, 1, 0, {Packet("first")});
    WriteChunk(1, 1, 2, {Packet("third")});
    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(stats().staging_chunks_read(), missing_first ? 1u : 2u);
    if (missing_first)
      WriteChunk(1, 1, 0, {Packet("first")});
    WriteChunk(1, 1, 1, {Packet("second")});
    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(stats().chunks_discarded(), missing_first ? 2u : 1u);
    const auto packets = ReadAll();
    ASSERT_EQ(packets.size(), missing_first ? 1u : 2u);
    EXPECT_EQ(packets.back().str, "third");
    // As in TBv2, a read gap requires a previously consumed chunk.
    EXPECT_EQ(packets.back().loss_from_buffer,
              missing_first ? 0u : kLossPresent | kLossReadGap);
  }
}

TEST_F(TraceBufferV3Test, CheckpointDropsPacketAcrossGap) {
  ResetBuffer(4096, OneBundlePerPacket());
  const std::string packet = Packet("split-across-three-chunks");
  CreateChunk(1, 1, 0)
      .AddFragment(packet.substr(0, 10), kContOnNextChunk)
      .CopyIntoTraceBuffer();
  CreateChunk(1, 1, 2)
      .AddFragment(packet.substr(20), kContFromPrevChunk)
      .AddFragment(Packet("after"))
      .CopyIntoTraceBuffer();
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().bundles_written(), 1u);

  CreateChunk(1, 1, 1)
      .AddFragment(packet.substr(10, 10), kContFromPrevChunk | kContOnNextChunk)
      .CopyIntoTraceBuffer();
  trace_buffer_->OnWriteCheckpoint();
  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("after"));
  EXPECT_NE(packets[0].loss_from_buffer & kLossReadGap, 0u);
}

TEST_F(TraceBufferV3Test, CheckpointBundlesAllProducers) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 128;
  ResetBuffer(4096, params);

  // Neither producer reaches the target alone.
  // The second checkpoint bundles both producers' packets together.
  for (ChunkID chunk = 0; chunk < 2; ++chunk) {
    const char value = chunk == 0 ? 'a' : 'b';
    ASSERT_EQ(WriteChunk(1, 1, chunk, {Packet(std::string(40, value))}), 64u);
    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(stats().bundles_written(), chunk);
    ASSERT_EQ(WriteChunk(2, 1, chunk, {Packet(std::string(40, value))}), 64u);
    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(stats().bundles_written(), chunk + 1);
    EXPECT_EQ(staging()->used_size(), 0u);
  }
  EXPECT_EQ(FrontBundleMetadata().sequence_list.size(), 2u);
  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, 'a'), std::string(40, 'a'),
                          std::string(40, 'b'), std::string(40, 'b')));
  for (size_t i = 0; i < packets.size(); ++i) {
    EXPECT_EQ(packets[i].producer_id, i % 2 + 1);
    EXPECT_EQ(packets[i].loss(), 0u);
  }
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
  trace_buffer_->OnWriteCheckpoint();
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
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("read-and-the-rest", "b"));
  EXPECT_EQ(stats().staging_readaheads_succeeded(), 1u);
  EXPECT_EQ(stats().patches_succeeded(), 1u);
}

// Chunks and their patches arrive in one CommitData. OnWriteCheckpoint() comes
// after both, so the patched packet is in the bundle of that batch.
TEST_F(TraceBufferV3Test, OnWriteCheckpointAfterChunksAndPatches) {
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
  trace_buffer_->OnWriteCheckpoint();
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
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(stats().chunks_discarded(), 0u);
}

// The scrape copies complete chunks 0 and 1 before their CommitData arrives.
// OnWriteCheckpoint() bundles them, which erases the copies in staging. Staging
// then drops the late commits and counts them in chunks_discarded, as a
// standalone TraceBufferV2 does after a read. The packets are returned once and
// without loss.
TEST_F(TraceBufferV3Test, LateCommitAfterScrapeAndBundling) {
  ResetBuffer(4096, OneBundlePerPacket());
  WriteChunk(1, 1, 0, {Packet("P")});
  WriteChunk(1, 1, 1, {Packet("Q")});
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_EQ(stats().bundles_written(), 2u);
  const uint64_t bytes_written = stats().bytes_written();

  WriteChunk(1, 1, 0, {Packet("P")});
  WriteChunk(1, 1, 1, {Packet("Q")});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().chunks_discarded(), 2u);
  EXPECT_EQ(stats().chunks_written(), 2u);
  EXPECT_EQ(stats().bytes_written(), bytes_written);
  EXPECT_EQ(stats().bundles_written(), 2u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("P", "Q"));
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[1].loss(), 0u);

  // The sequence continues without a gap.
  WriteChunk(1, 1, 2, {Packet("R")});
  trace_buffer_->OnWriteCheckpoint();
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
  ASSERT_TRUE(CopyChunkV2(1, 1, {split.substr(0, 8)}, kContOnNextChunk));
  ASSERT_TRUE(CopyChunkV2(1, 1, {split.substr(8)}, kContFromPrevChunk));
  // An SMB v1 writer of the same producer.
  WriteChunk(1, 2, 0, {Packet("v1")});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().bundles_written(), 3u);

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
  EXPECT_EQ(packets[0].loss_in_packet, 0u);
  EXPECT_EQ(packets[0].loss_from_buffer, kLossPresent | kLossReadGap);

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

  // Each record takes 48 bytes, 3 for its header and 45 for the packet. The
  // third one crosses 100, so the first bundle has three records. The second
  // bundle gets the other two and ends because staging has no more packets.
  std::vector<std::string> packets;
  for (char c : {'a', 'b', 'c', 'd', 'e'})
    packets.push_back(Packet(std::string(40, c)));
  WriteChunk(1, 1, 0, packets);
  trace_buffer_->OnWriteCheckpoint();

  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_EQ(stats().bundle_bytes_uncompressed(), 5 * 48u);
  EXPECT_EQ(stats().bundle_bytes_stored(), trace_buffer_->used_size());
  EXPECT_EQ(FrontBundleMetadata().uncompressed_size, 3 * 48u);

  trace_buffer_->BeginRead();
  TestPacket packet;
  for (int i = 0; i < 3; i++)
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, std::string(40, 'c'));
  EXPECT_EQ(FrontBundleMetadata().uncompressed_size, 2 * 48u);
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(packet.str, std::string(40, 'e'));
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  EXPECT_EQ(stats().bytes_read(), 5 * 48u);
}

// Records that add up to exactly the target end the bundle.
TEST_F(TraceBufferV3Test, ExactTarget) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 96;
  ResetBuffer(4096, params);
  std::vector<std::string> packets;
  for (char c : {'a', 'b', 'c', 'd'})
    packets.push_back(Packet(std::string(40, c)));
  WriteChunk(1, 1, 0, packets);
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_EQ(FrontBundleMetadata().uncompressed_size, 96u);
  EXPECT_EQ(Strs(ReadAll()).size(), 4u);
}

// One bundle with packets from 5 bytes to about 40 KB.
// Packet() adds 5 bytes to a string of up to 125 bytes. From 128 bytes up to
// 16 KiB it adds 9, because protozero keeps a 4-byte length for the nested
// message.
// - Strings of 122 and 123 bytes make packets of 127 and 128 bytes. The
//   packet_size varint of the record grows from 1 to 2 bytes there.
// - Strings of 16374 to 16376 bytes make packets of 16383 to 16385 bytes.
//   ZstdFrameCodec gathers inputs of up to 16384 bytes and passes larger ones
//   directly to zstd. The packet_size varint grows to 3 bytes at 16384.
TEST_F(TraceBufferV3Test, PacketsOfManySizesInOneBundle) {
  Params params = BundleOnlyOnRead();
  params.staging_capacity_bytes = 1024 * 1024;
  ResetBuffer(1024 * 1024, params);

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
  EXPECT_EQ(stats().bundles_written(), 0u);

  EXPECT_EQ(Strs(ReadAll()), expected);
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(stats().bundles_corrupted(), 0u);
}

// A sequence that waits for a fragment makes no progress. A pass that finds
// nothing to read still resets the counter, so OnWriteCheckpoint() does not
// start a pass again until enough new chunks arrive.
TEST_F(TraceBufferV3Test, BlockedStagingDoesNotCausePassPerOnWriteCheckpoint) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 64;
  ResetBuffer(4096, params);

  // The first fragment of a packet whose second fragment never arrives.
  ASSERT_EQ(CreateChunk(1, 1, 0)
                .AddFragment(std::string(40, 'x'), kContOnNextChunk)
                .CopyIntoTraceBuffer(),
            57u);  // Takes 64 bytes in staging.
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().staging_readaheads_failed(), 1u);
  EXPECT_EQ(stats().bundles_written(), 0u);

  // No new chunks, so no pass.
  trace_buffer_->OnWriteCheckpoint();
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().staging_readaheads_failed(), 1u);

  // The next 64 bytes start a pass. The other writer is bundled and the
  // blocked chunk stays.
  WriteChunk(1, 2, 0, {Packet(std::string(40, 'b'))});
  trace_buffer_->OnWriteCheckpoint();
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
  EXPECT_EQ(stats().bundle_bytes_stored(), size_a + size_b + size_c);

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
  const Params params = OneBundlePerPacket();
  const std::string p1 = Packet(std::string(40, '1'));
  const std::string p4 = Packet(std::string(40, '4'));
  const size_t small_size = BundleSize(params, {p1});
  const size_t capacity = 3 * small_size;
  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    SCOPED_TRACE(policy);
    for (uint32_t producer_loss : {0u, kLossPresent | kLossSmbFull}) {
      SCOPED_TRACE(producer_loss);
      // Dropping P2 reports loss even without a producer flag, and preserves
      // any reason that the producer supplied.
      const std::string p2 = Packet(RandomBytes(capacity, 2), producer_loss);
      ASSERT_GT(BundleSize(params, {p2}), capacity);

      next_chunk_id_ = 0;
      ResetBuffer(capacity, params, policy);
      WriteBatch(p1);
      WriteBatch(p2);
      EXPECT_EQ(stats().bundles_too_large(), 1u);
      EXPECT_EQ(stats().bundles_overwritten(), 0u);
      EXPECT_EQ(stats().bundles_written(), 1u);
      EXPECT_EQ(trace_buffer_->used_size(), small_size);
      EXPECT_FALSE(has_overflow_bundle());
      EXPECT_EQ(num_bundled_sequences(), 1u);
      ExpectNoOpenBundle();

      WriteBatch(p4);
      std::vector<TestPacket> packets = ReadAll();
      ASSERT_THAT(Strs(packets),
                  ElementsAre(std::string(40, '1'), std::string(40, '4')));
      EXPECT_EQ(packets[0].loss(), 0u);
      EXPECT_EQ(packets[1].loss_in_packet, 0u);
      EXPECT_EQ(packets[1].loss_from_buffer, kLossPresent | producer_loss);
      EXPECT_EQ(num_bundled_sequences(), 0u);
    }
  }
}

// A blocked sequence is tried once per consumer batch, even when the batch
// reads several bundles from staging.
TEST_F(TraceBufferV3Test, ReadKeepsStagingCursorAcrossBundles) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 64;
  const std::string split = Packet("fragmented");
  const std::string a(80, 'a');
  const std::string b(80, 'b');

  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    SCOPED_TRACE(policy);
    ResetBuffer(4096, params, policy);
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
      EXPECT_EQ(packet.loss(), 0u);
      EXPECT_EQ(stats().staging_readaheads_failed(), 1u);
    }

    // The final partial bundle completes the staging read and compacts it.
    // Further reads must not resume that cursor or retry the blocked sequence.
    EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
    EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
    EXPECT_EQ(stats().bundles_written(), 3u);
    EXPECT_EQ(stats().compactions(), 1u);
    EXPECT_EQ(stats().staging_readaheads_failed(), 1u);
    EXPECT_EQ(staging()->used_size(), blocked_chunk_size);

    // A new batch can read the sequence once its missing fragment arrives.
    CreateChunk(1, 1, 1)
        .AddFragment(split.substr(9), kContFromPrevChunk)
        .CopyIntoTraceBuffer();
    const auto packets = ReadAll();
    ASSERT_THAT(Strs(packets), ElementsAre("fragmented"));
    EXPECT_EQ(packets[0].loss(), 0u);
    EXPECT_EQ(stats().staging_readaheads_succeeded(), 1u);
    EXPECT_EQ(staging()->used_size(), 0u);
  }
}

// Readout skips bundles that are too large and keeps their loss for later
// packets. The small packet completes the staging read before its bundle
// reaches the target, leaving an earlier sequence blocked on a missing
// fragment.
TEST_F(TraceBufferV3Test, ReadSkipsOversizedBundles) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 64;
  const std::string small = Packet("kept");
  const std::string large =
      Packet(RandomBytes(512, 1), kLossPresent | kLossSmbFull);
  const std::string split = Packet("fragmented");
  const size_t capacity = 3 * BundleSize(params, {small});
  ASSERT_GT(BundleSize(params, {large}), capacity);

  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    ResetBuffer(capacity, params, policy);
    CreateChunk(1, 2, 0)
        .AddFragment(split.substr(0, 9), kContOnNextChunk)
        .CopyIntoTraceBuffer();
    const size_t blocked_chunk_size = staging()->used_size();
    // Skip consecutive bundles that are too large before returning the final
    // packet.
    WriteChunk(1, 1, 0, {large, large, small});
    EXPECT_EQ(stats().bundles_written(), 0u);

    trace_buffer_->BeginRead();
    TestPacket packet;
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
    EXPECT_EQ(packet.str, "kept");
    EXPECT_EQ(packet.loss_from_buffer, kLossPresent | kLossSmbFull);
    EXPECT_EQ(stats().bundles_too_large(), 2u);
    EXPECT_EQ(stats().bundles_written(), 1u);
    // Dropping bundles must not retry the unchanged blocked sequence.
    EXPECT_EQ(stats().staging_readaheads_failed(), 1u);
    const uint64_t stored_bytes = stats().bundle_bytes_stored();

    // Finish the staging read before returning its final partial bundle.
    EXPECT_EQ(stats().compactions(), 1u);
    EXPECT_EQ(staging()->used_size(), blocked_chunk_size);
    uint64_t passes = 0;
    for (uint64_t count : stats().bundling_time_us_histogram_counts())
      passes += count;
    // Drops of bundles that are too large and the final partial bundle share
    // one sample.
    EXPECT_EQ(passes, 1u);
    EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));

    // A bundle that is too large can leave no stored bundle for the decoder.
    WriteChunk(1, 1, 1, {large});
    EXPECT_THAT(ReadAll(), IsEmpty());
    EXPECT_EQ(stats().bundles_too_large(), 3u);
    EXPECT_EQ(stats().bundle_bytes_stored(), stored_bytes);

    WriteChunk(1, 1, 2, {small});
    std::vector<TestPacket> packets = ReadAll();
    ASSERT_THAT(Strs(packets), ElementsAre("kept"));
    EXPECT_EQ(packets[0].loss_from_buffer, kLossPresent | kLossSmbFull);
    EXPECT_EQ(stats().bundles_written(), 2u);

    // The blocked sequence remains intact and completes in a later pass.
    CreateChunk(1, 2, 1)
        .AddFragment(split.substr(9), kContFromPrevChunk)
        .CopyIntoTraceBuffer();
    packets = ReadAll();
    ASSERT_THAT(Strs(packets), ElementsAre("fragmented"));
    EXPECT_EQ(packets[0].loss(), 0u);
    EXPECT_EQ(staging()->used_size(), 0u);
  }
}

// In kDiscard, a bundle that fits the buffer but not its free space is kept.
// It waits as the overflow bundle and is read after the stored bundles and
// before staging. Further chunks are rejected, including after reads free
// space.
TEST_F(TraceBufferV3Test, DiscardKeepsOverflowBundle) {
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
  const size_t memory_without_overflow = trace_buffer_->GetMemoryUsageBytes();
  WriteBatch(c);  // Fits the buffer, not the 4 bytes that are free.
  EXPECT_TRUE(has_overflow_bundle());
  ExpectNoOpenBundle();
  EXPECT_GT(trace_buffer_->GetMemoryUsageBytes(), memory_without_overflow);
  EXPECT_EQ(trace_buffer_->used_size(), size_ab);
  EXPECT_EQ(stats().bundles_written(), 3u);
  EXPECT_EQ(stats().bundle_bytes_stored(), size_ab + size_c);
  EXPECT_EQ(stats().bundles_overwritten(), 0u);
  EXPECT_EQ(stats().bundles_too_large(), 0u);
  EXPECT_EQ(num_bundled_sequences(), 1u);
  // The pass stopped right after it read |c|. Staging erases a chunk when it
  // reads past it, so the chunk of |c| is still there, with nothing to read.
  EXPECT_EQ(staging()->used_size(), 64u);

  // The cumulative bundle bytes crossed capacity, so the next chunk is
  // rejected.
  WriteBatch(d);
  EXPECT_EQ(stats().bundles_written(), 3u);
  EXPECT_EQ(stats().staging_chunks_read(), 3u);
  EXPECT_EQ(stats().chunks_discarded(), 1u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets),
              ElementsAre(std::string(40, 'a'), std::string(40, 'b'),
                          std::string(40, 'c')));
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss(), 0u);
  EXPECT_FALSE(has_overflow_bundle());
  EXPECT_EQ(trace_buffer_->used_size(), 0u);
  EXPECT_EQ(stats().bundles_written(), 3u);
  EXPECT_EQ(stats().bundle_bytes_stored(), size_ab + size_c);
  trace_buffer_->BeginRead();
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), memory_without_overflow);

  // Neither reads nor compaction reopen intake.
  WriteBatch(a);
  EXPECT_THAT(ReadAll(), IsEmpty());
  trace_buffer_->MaybeCompact();
  WriteBatch(a);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// A DISCARD batch can exceed staging capacity before its checkpoint.
// Pressure bundling makes room while compressed storage has space.
TEST_F(TraceBufferV3Test, DiscardDrainsStagingUnderPressure) {
  Params params = OneBundlePerPacket();
  params.bundle_target_bytes = 2048;
  ResetBuffer(64 * 1024, params, TraceBuffer::kDiscard);

  // Each chunk takes 1040 bytes, so three fit in the 4096 bytes of staging.
  const std::string big = Packet(std::string(1000, 'x'));
  for (ChunkID chunk_id = 0; chunk_id < 5; chunk_id++)
    WriteChunk(1, 1, chunk_id, {big});
  EXPECT_EQ(stats().chunks_written(), 5u);
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  EXPECT_EQ(stats().staging_pressure_passes(), 1u);
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_FALSE(staging_discard_writes());

  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_EQ(staging()->used_size(), 0u);

  const auto packets = ReadAll();
  ASSERT_EQ(packets.size(), 5u);
  for (const auto& packet : packets) {
    EXPECT_EQ(packet.str, std::string(1000, 'x'));
    EXPECT_EQ(packet.loss(), 0u);
  }

  WriteChunk(1, 1, 5, {Packet("late")});
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  EXPECT_THAT(Strs(ReadAll()), ElementsAre("late"));
}

// While kDiscard still accepts writes, each pass returns the space used by
// bundled chunks to staging. Their compressed total fits the write allowance,
// although their chunks need much more than staging's capacity.
TEST_F(TraceBufferV3Test, DiscardReusesStagingWhileBundling) {
  ResetBuffer(64 * 1024, OneBundlePerPacket(), TraceBuffer::kDiscard);
  size_t num_read = 0;
  // 128 chunks need twice staging's capacity before compression.
  for (int i = 0; i < 128; i++) {
    WriteBatch(Packet(std::string(40, 'a')));
    if (i % 32 == 31)
      num_read += ReadAll().size();
  }
  EXPECT_EQ(stats().chunks_discarded(), 0u);
  EXPECT_FALSE(has_overflow_bundle());
  EXPECT_EQ(num_read, 128u);
  EXPECT_EQ(stats().bytes_written(), 128u * 64);
  EXPECT_GT(stats().bytes_written(), staging()->size());
  EXPECT_LT(stats().bundle_bytes_stored(), trace_buffer_->size());
}

// Reads and compaction reclaim storage without restoring the compressed
// allowance.
// Both checkpoint and consumer bundling stop intake at the cumulative limit.
TEST_F(TraceBufferV3Test, DiscardReadsDoNotRestoreWriteAllowance) {
  const Params params = OneBundlePerPacket();
  const std::string payload(40, 'a');
  const std::string packet = Packet(payload);
  const size_t bundle_size = BundleSize(params, {packet});
  for (bool checkpoint : {false, true}) {
    SCOPED_TRACE(checkpoint);
    for (size_t capacity : {2 * bundle_size, 2 * bundle_size - 1}) {
      SCOPED_TRACE(capacity);
      ResetBuffer(capacity, params, TraceBuffer::kDiscard);
      WriteChunk(1, 1, 0, {packet});
      if (checkpoint)
        trace_buffer_->OnWriteCheckpoint();
      ASSERT_THAT(Strs(ReadAll()), ElementsAre(payload));
      trace_buffer_->MaybeCompact();
      EXPECT_EQ(trace_buffer_->used_size(), 0u);
      EXPECT_EQ(staging()->used_size(), 0u);
      EXPECT_EQ(stats().bundle_bytes_stored(), bundle_size);
      EXPECT_FALSE(staging_discard_writes());

      // Both chunks arrive before the next pass closes intake.
      // The crossing bundle fits the empty ring, so it needs no overflow
      // bundle.
      WriteChunk(1, 1, 1, {packet});
      WriteChunk(1, 1, 2, {packet});
      if (checkpoint)
        trace_buffer_->OnWriteCheckpoint();
      trace_buffer_->BeginRead();
      TestPacket read_packet;
      ASSERT_TRUE(ReadOne(trace_buffer_.get(), &read_packet));
      EXPECT_EQ(read_packet.str, payload);
      EXPECT_EQ(stats().bundle_bytes_stored(), 2 * bundle_size);
      EXPECT_TRUE(staging_discard_writes());
      EXPECT_FALSE(has_overflow_bundle());

      // The already accepted chunk survives compaction after intake stops.
      trace_buffer_->MaybeCompact();
      WriteChunk(1, 1, 3, {packet});
      EXPECT_EQ(stats().chunks_discarded(), 1u);
      EXPECT_THAT(Strs(ReadAll()), ElementsAre(payload));
      EXPECT_EQ(stats().bundle_bytes_stored(), 3 * bundle_size);
      trace_buffer_->MaybeCompact();
      EXPECT_EQ(staging()->used_size(), 0u);
      WriteChunk(1, 1, 4, {packet});
      EXPECT_EQ(stats().chunks_discarded(), 2u);
      EXPECT_THAT(ReadAll(), IsEmpty());
    }
  }
}

// Each pass compacts staging while the buffer accepts writes, in both
// policies. zstd has copied the packets it read, so their chunks are free.
// The remaining chunks move to the start of staging. Here they hold 8 KiB
// of fragments waiting for the rest of their packets.
TEST_F(TraceBufferV3Test, CompactsStagingAfterEachPass) {
  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    SCOPED_TRACE(policy == TraceBuffer::kDiscard ? "kDiscard" : "kOverwrite");
    Params params = OneBundlePerPacket();
    params.staging_capacity_bytes = 64 * 1024;
    ResetBuffer(64 * 1024, params, policy);

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

    // The chunks that moved are intact. Their packets complete and are read
    // after the 10 packets that were bundled before them.
    for (WriterID writer = 10; writer < 18; writer++)
      WriteEndOfSplitPacket(writer);
    trace_buffer_->OnWriteCheckpoint();
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

// Earlier slices stay valid as later bundles fill the second decode block
// and then use a temporary allocation.
TEST_F(TraceBufferV3Test, SlicesStayValidForTheBatch) {
  Params params = OneBundlePerPacket();
  params.staging_capacity_bytes = 64 * 1024;
  ResetBuffer(64 * 1024, params);
  const size_t initial_decode_memory = decode_memory_bytes();
  std::vector<std::string> expected;
  // Each 1 MiB block fits 17 records, so the 35th needs a temporary.
  for (size_t i = 0; i < 35; ++i) {
    expected.push_back(Packet(std::string(60000, static_cast<char>('A' + i))));
    WriteBatch(expected.back());
  }

  trace_buffer_->BeginRead();
  std::vector<TracePacket> packets(expected.size());
  for (TracePacket& packet : packets) {
    TestPacket ignored;
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &ignored, &packet));
  }
  // A write checkpoint must keep the bytes backing returned slices.
  WriteChunk(1, 1, next_chunk_id_++, {Packet("after-read")});
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_EQ(stats().bundles_written(), expected.size() + 1);
  ASSERT_EQ(decode_memory_bytes(),
            initial_decode_memory + Record(0, expected.back()).size());
  for (size_t i = 0; i < packets.size(); i++)
    EXPECT_EQ(packets[i].GetRawBytesForTesting(), expected[i]);

  // The temporary is freed and both blocks keep their capacity.
  packets.clear();
  trace_buffer_->BeginRead();
  EXPECT_EQ(decode_memory_bytes(), initial_decode_memory);
}

// File-sized batches fit from the first read, including when the next batch
// resumes an unread bundle or follows compaction.
TEST_F(TraceBufferV3Test, DecodeArenaFitsFileReadBatchesFromFirstRead) {
  constexpr size_t kNumBatches = 4;
  constexpr size_t kBatchBytes = TracingServiceImpl::kWriteIntoFileChunkSize;
  trace_buffer_ = TraceBufferV3::Create(256 * 1024);
  ASSERT_TRUE(trace_buffer_);
  const size_t initial_memory = trace_buffer_->GetMemoryUsageBytes();
  const size_t decode_memory = decode_memory_bytes();
  const std::string input = Packet(std::string(1024, 'a'));
  const size_t packets_per_batch =
      (kBatchBytes + input.size() - 1) / input.size();
  const size_t total_packets = kNumBatches * packets_per_batch + 32;
  for (size_t drain = 0; drain < 2; ++drain) {
    SCOPED_TRACE(drain);
    for (size_t i = 0; i < total_packets; ++i)
      WriteBatch(input);
    ASSERT_EQ(stats().chunks_overwritten(), 0u);
    ASSERT_EQ(stats().bundles_overwritten(), 0u);

    size_t packets_read = 0;
    for (size_t batch = 0; batch < kNumBatches; ++batch) {
      SCOPED_TRACE(batch);
      trace_buffer_->BeginRead();
      std::vector<TracePacket> packets;
      for (size_t bytes = 0; bytes < kBatchBytes;) {
        TracePacket packet;
        TestPacket result;
        ASSERT_TRUE(ReadOne(trace_buffer_.get(), &result, &packet));
        EXPECT_EQ(result.loss(), 0u);
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

  WriteBatch(input);
  EXPECT_EQ(ReadAll().size(), 1u);
  trace_buffer_->MaybeCompact();
  EXPECT_EQ(trace_buffer_->GetMemoryUsageBytes(), initial_memory);
}

TEST_F(TraceBufferV3Test, TargetCrossingBundlesReuseDecodeBlocksAcrossBatches) {
  constexpr size_t kTargetBytes = 1024 * 1024;
  constexpr size_t kPacketsPerBundle = 18;
  constexpr size_t kNumBundles = 4;
  trace_buffer_ = TraceBufferV3::Create(8 * kTargetBytes);
  ASSERT_TRUE(trace_buffer_);
  ASSERT_EQ(params().bundle_target_bytes, kTargetBytes);
  const size_t decode_memory = decode_memory_bytes();
  EXPECT_EQ(decode_memory, 4 * kTargetBytes);

  std::vector<std::string> expected;
  for (size_t bundle = 0; bundle < kNumBundles; ++bundle) {
    expected.push_back(
        Packet(std::string(60000, static_cast<char>('A' + bundle))));
    // Checkpoint only after all 18 records, so it cannot cut a bundle short.
    for (size_t i = 0; i < kPacketsPerBundle; ++i)
      WriteChunk(1, 1, next_chunk_id_++, {expected.back()});
    trace_buffer_->OnWriteCheckpoint();
    ASSERT_EQ(stats().bundles_written(), bundle + 1);
  }
  const size_t record_size = Record(0, expected.front()).size();
  const size_t bundle_size = kPacketsPerBundle * record_size;
  ASSERT_LT(bundle_size - record_size, kTargetBytes);
  ASSERT_GT(bundle_size, kTargetBytes);
  ASSERT_LE(bundle_size, 2 * kTargetBytes);
  ASSERT_EQ(stats().bundle_bytes_uncompressed(), kNumBundles * bundle_size);
  ASSERT_EQ(stats().chunks_overwritten(), 0u);
  ASSERT_EQ(stats().bundles_overwritten(), 0u);

  // Each middle batch finishes one bundle and reads half the next. BeginRead()
  // must keep the unread bundle and make the other block reusable.
  const size_t batch_sizes[] = {9, 18, 18, 18, 9};
  const void* block_starts[2] = {};
  const uint8_t* previous_packet_end = nullptr;
  const size_t header_size = record_size - expected.front().size();
  size_t packets_read = 0;
  for (size_t batch_size : batch_sizes) {
    SCOPED_TRACE(packets_read);
    trace_buffer_->BeginRead();
    EXPECT_EQ(decode_memory_bytes(), decode_memory);
    std::vector<TracePacket> packets(batch_size);
    const size_t batch_start = packets_read;
    for (TracePacket& packet : packets) {
      const size_t bundle = packets_read / kPacketsPerBundle;
      const bool first_in_bundle = packets_read % kPacketsPerBundle == 0;
      if (first_in_bundle) {
        ASSERT_FALSE(bundled().empty());
        ASSERT_EQ(FrontBundleMetadata().uncompressed_size, bundle_size);
      }
      TestPacket result;
      ASSERT_TRUE(ReadOne(trace_buffer_.get(), &result, &packet));
      EXPECT_EQ(result.loss(), 0u);
      ASSERT_EQ(packet.slices().size(), 1u);
      const auto* start = static_cast<const uint8_t*>(packet.slices()[0].start);
      if (first_in_bundle) {
        if (bundle < 2)
          block_starts[bundle] = start;
        else
          EXPECT_EQ(start, block_starts[bundle % 2]);
      } else {
        // This also checks that a read resumes in the same decoded bundle.
        EXPECT_EQ(start, previous_packet_end + header_size);
      }
      previous_packet_end = start + packet.size();
      ++packets_read;
      // The allowance already counts both blocks. Any increase is a temporary.
      EXPECT_EQ(decode_memory_bytes(), decode_memory);
    }
    for (size_t i = 0; i < packets.size(); ++i) {
      EXPECT_EQ(packets[i].GetRawBytesForTesting(),
                expected[(batch_start + i) / kPacketsPerBundle]);
    }
  }
  EXPECT_EQ(packets_read, kNumBundles * kPacketsPerBundle);
  TestPacket packet;
  EXPECT_FALSE(ReadOne(trace_buffer_.get(), &packet));
  trace_buffer_->BeginRead();
  EXPECT_EQ(decode_memory_bytes(), decode_memory);
}

// A batch can stop in the middle of a bundle. The next batch continues in the
// same decoded bytes, even with writes in between.
TEST_F(TraceBufferV3Test, ReadResumesInsideBundle) {
  ResetBuffer(4096, BundleOnlyOnRead());
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
  EXPECT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(num_bundled_sequences(), 1u);

  // A write batch between the two read tasks. It stays in staging.
  WriteChunk(1, 1, 1, {Packet("p5")});
  trace_buffer_->OnWriteCheckpoint();

  trace_buffer_->BeginRead();
  TracePacket p3;
  ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet, &p3));
  EXPECT_EQ(packet.str, "p3");
  // The record of p3 follows p2 in the same memory, so nothing was decoded
  // again. Its three varints take 3 bytes.
  EXPECT_EQ(static_cast<const uint8_t*>(p3.slices()[0].start), p2_end + 3);
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
  params.staging_capacity_bytes = 3 * 1024 * 1024;
  ResetBuffer(3 * 1024 * 1024, params);
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
  // The decoded packet is contiguous, so the buffer splits it for the IPC.
  EXPECT_EQ(large_packet.slices().size(), 20u);
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

// B1 stays decoded while writes overwrite stored B2. B1's unread packet
// precedes the gap, both in the original and in its clone.
TEST_F(TraceBufferV3Test, DecodedBundleSurvivesOverwrite) {
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

  // B2 uses the space released when B1 was decoded.
  WriteChunk(1, 1, 1, {Packet("s3"), Packet("s4")});
  WriteChunk(1, 2, 1, {Packet("t2")});
  BundleNow();
  EXPECT_EQ(stats().bundles_written(), 2u);
  EXPECT_EQ(stats().bundles_overwritten(), 0u);
  WriteChunk(1, 2, 2, {Packet("t3")});
  BundleNow();
  EXPECT_EQ(stats().bundles_overwritten(), 1u);
  EXPECT_EQ(stats().bundles_too_large(), 0u);

  auto clone = trace_buffer_->CloneReadOnly();
  for (TraceBuffer* buffer :
       {trace_buffer_.get(), static_cast<TraceBufferV3*>(clone.get())}) {
    std::vector<TestPacket> packets = perfetto::ReadAll(buffer);
    ASSERT_THAT(Strs(packets), ElementsAre("t1", "t3"));
    EXPECT_EQ(packets[0].loss(), 0u);
    EXPECT_EQ(packets[1].loss(), kLossPresent | kLossOverwrite);
  }
  EXPECT_EQ(num_bundled_sequences(), 0u);
}

// ---------------------------------------------------------------------------
// Data loss
// ---------------------------------------------------------------------------

// Staging loss stays separate from the producer's original packet bytes.
TEST_F(TraceBufferV3Test, StagingLossStaysOutsidePacket) {
  ResetBuffer(4096, OneBundlePerPacket());
  WriteChunk(1, 1, 0, {Packet("a")});
  // Chunk 1 is missing. The producer also flags its own loss on "c".
  WriteChunk(1, 1, 2, {Packet("c", kLossPresent | kLossSmbFull), Packet("d")});
  // A producer flag without loss in staging stays as it is.
  WriteChunk(1, 1, 3, {Packet("e", kLossPresent | kLossSmbFull)});
  trace_buffer_->OnWriteCheckpoint();
  // The completed batch advances across the gap and reports loss on "c".
  EXPECT_EQ(stats().staging_chunks_read(), 3u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "c", "d", "e"));
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[2].loss(), 0u);
  EXPECT_EQ(packets[3].loss_in_packet, kLossPresent | kLossSmbFull);
  EXPECT_EQ(packets[1].loss_in_packet, kLossPresent | kLossSmbFull);
  EXPECT_EQ(packets[1].loss_from_buffer, kLossPresent | kLossReadGap);
}

// Reassembling empty SMB v1 fragments can return an empty packet with loss.
// Skipping that packet must keep its loss for the next nonempty packet.
TEST_F(TraceBufferV3Test, EmptyPacketKeepsLossForNextPacket) {
  ResetBuffer(4096, OneBundlePerPacket());
  WriteChunk(1, 1, 0, {Packet("a")});
  // Chunk 1 is missing. Chunks 2 and 3 hold two empty fragments.
  CreateChunk(1, 1, 2).AddFragment("", kContOnNextChunk).CopyIntoTraceBuffer();
  CreateChunk(1, 1, 3)
      .AddFragment("", kContFromPrevChunk)
      .AddFragment(Packet("b"))
      .AddFragment(Packet("c"))
      .CopyIntoTraceBuffer();
  trace_buffer_->OnWriteCheckpoint();

  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "b", "c"));
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[1].loss_from_buffer, kLossPresent | kLossReadGap);
  EXPECT_EQ(packets[2].loss(), 0u);
}

// Bundle overwrite loss is reported once, separately from the producer's
// original packet bytes.
TEST_F(TraceBufferV3Test, BundleLossStaysOutsidePacket) {
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
  EXPECT_EQ(packets[0].loss_from_buffer, kLossPresent | kLossOverwrite);
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
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_EQ(stats().bundles_overwritten(), 1u);
  EXPECT_EQ(num_bundled_sequences(), 1u);  // Only {2, 1}.

  WriteBatch(Packet(std::string(40, 'b')));  // Overwrites the bundle of {2, 1}.
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].str, std::string(40, 'b'));
  EXPECT_EQ(packets[0].loss_from_buffer, kLossPresent | kLossOverwrite);
}

// A batch larger than staging drains readable packets before overwriting them.
TEST_F(TraceBufferV3Test, StagingPressurePreservesPackets) {
  Params params = OneBundlePerPacket();
  ResetBuffer(64 * 1024, params);
  WriteBatch(Packet("first"));

  // One batch of 80 chunks of 64 bytes into 4096 bytes of staging.
  for (int i = 0; i < 80; i++)
    WriteChunk(1, 1, next_chunk_id_++, {Packet(std::string(40, 'x'))});
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_EQ(stats().bundles_overwritten(), 0u);
  uint64_t passes = 0;
  for (uint64_t count : stats().bundling_time_us_histogram_counts())
    passes += count;
  // The first checkpoint, one pressure drain and the final checkpoint.
  // The 81 bundles built inside those passes are not separate samples.
  EXPECT_EQ(passes, 3u);
  EXPECT_EQ(stats().staging_pressure_passes(), 1u);
  EXPECT_EQ(stats().staging_pressure_compactions_skipped(), 0u);

  std::vector<TestPacket> packets = ReadAll();
  ASSERT_EQ(packets.size(), 81u);
  for (const auto& packet : packets)
    EXPECT_EQ(packet.loss(), 0u);
}

TEST_F(TraceBufferV3Test, StagingPressureCountsSkippedCompactions) {
  Params params = BundleOnlyOnRead();
  params.staging_capacity_bytes = 4096;
  ResetBuffer(64 * 1024, params);

  // Four blocked chunks fill staging.
  // The fifth write runs a pressure pass and wraps.
  // The sixth finds blocked chunks on both sides of the cursor, so its
  // pressure pass skips compaction.
  for (WriterID writer = 1; writer <= 6; ++writer)
    WriteStartOfSplitPacket(writer);
  EXPECT_EQ(stats().staging_pressure_passes(), 2u);
  EXPECT_EQ(stats().staging_pressure_compactions_skipped(), 1u);
  EXPECT_EQ(stats().compactions_skipped(), 1u);
  EXPECT_EQ(stats().bundles_written(), 0u);

  // Only pressure passes ran, so they account for all measured time.
  uint64_t total_us = 0;
  for (int64_t sum : stats().bundling_time_us_histogram_sum())
    total_us += static_cast<uint64_t>(sum);
  const uint64_t pressure_us = stats().staging_pressure_time_us();
  EXPECT_EQ(pressure_us, total_us);

  // Checkpoint and consumer-read passes also skip compaction here, but they
  // do not change the pressure counters.
  BundleNow();
  EXPECT_THAT(ReadAll(), IsEmpty());
  EXPECT_EQ(stats().compactions_skipped(), 3u);
  EXPECT_EQ(stats().staging_pressure_passes(), 2u);
  EXPECT_EQ(stats().staging_pressure_time_us(), pressure_us);
  EXPECT_EQ(stats().staging_pressure_compactions_skipped(), 1u);

  auto clone = trace_buffer_->CloneReadOnly();
  ASSERT_TRUE(clone);
  EXPECT_EQ(clone->stats().staging_pressure_passes(), 2u);
  EXPECT_EQ(clone->stats().staging_pressure_time_us(), pressure_us);
  EXPECT_EQ(clone->stats().staging_pressure_compactions_skipped(), 1u);
}

TEST_F(TraceBufferV3Test, StagingPressureWaitsForOutOfOrderChunks) {
  for (bool first_chunk_bundled : {false, true}) {
    SCOPED_TRACE(first_chunk_bundled);
    Params params = BundleOnlyOnRead();
    params.staging_capacity_bytes = 4096;
    ResetBuffer(64 * 1024, params);
    if (first_chunk_bundled) {
      WriteChunk(1, 1, 0, {Packet("first")});
      BundleNow();
    }
    // The scrape finds chunk 2 before chunk 1, and possibly before chunk 0.
    // Another writer fills staging. Pressure must leave chunk 2 untouched
    // whether we have a consumed predecessor or no sequence history yet.
    WriteChunk(1, 1, 2, {Packet("third")});
    for (ChunkID id = 0; id < 63; ++id)
      WriteChunk(2, 1, id, {Packet(std::string(40, 'x'))});
    WriteChunk(1, 1, 1, {Packet(std::string(80, 's'))});
    if (!first_chunk_bundled)
      WriteChunk(1, 1, 0, {Packet("first")});
    EXPECT_EQ(stats().chunks_overwritten(), 0u);
    EXPECT_EQ(stats().chunks_discarded(), 0u);
    BundleNow();
    std::vector<std::string> writer_packets;
    for (const auto& packet : ReadAll()) {
      EXPECT_EQ(packet.loss(), 0u);
      if (packet.producer_id == 1)
        writer_packets.push_back(packet.str);
    }
    EXPECT_THAT(writer_packets,
                ElementsAre("first", std::string(80, 's'), "third"));
  }
}

// SMB v2 chunks carry gap flags, so a pressure pass does not wait for
// missing chunk IDs.
TEST_F(TraceBufferV3Test, StagingPressureWithSmbV2) {
  Params params = BundleOnlyOnRead();
  params.staging_capacity_bytes = 4096;
  for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
    SCOPED_TRACE(policy);
    ResetBuffer(64 * 1024, params, policy);
    for (int i = 0; i < 80; ++i)
      ASSERT_TRUE(CopyChunkV2(1, 1, {PacketV2(std::string(40, 'x'))}));
    EXPECT_EQ(stats().staging_pressure_passes(), 1u);
    EXPECT_GT(stats().bundles_written(), 0u);
    EXPECT_EQ(stats().chunks_overwritten(), 0u);
    EXPECT_EQ(stats().chunks_discarded(), 0u);
    const auto packets = ReadAll();
    ASSERT_EQ(packets.size(), 80u);
    for (const auto& packet : packets)
      EXPECT_EQ(packet.loss(), 0u);
  }
}

// An already-read scrape needs relocation when its commit adds payload. The
// first attempt must leave it intact while the pressure drain compacts staging.
TEST_F(TraceBufferV3Test, StagingPressureRetriesScrapedChunkRelocation) {
  Params params = OneBundlePerPacket();
  ResetBuffer(64 * 1024, params);
  WriteChunk(2, 1, 0, {Packet(std::string(40, 'x'))});
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("first"))
      .AddFragment(Packet("unfinished"))
      .PadTo(256)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  // Read just these two packets. Each fills one bundle, so readout has not
  // reached the end of staging and compacted it yet. The scrape stays at 64.
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
    EXPECT_EQ(result.loss(), 0u);
    if (result.producer_id == 1)
      writer_packets.push_back(result.str);
  }
  EXPECT_THAT(writer_packets, ElementsAre("second"));
}

// Completing a scrape in place requires no new allocation, even when staging
// is full. It must not start a pressure drain or reject the accepted data.
TEST_F(TraceBufferV3Test, FullStagingAllowsInPlaceRecommit) {
  Params params = BundleOnlyOnRead();
  params.staging_capacity_bytes = 4096;
  ResetBuffer(64 * 1024, params);
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
  EXPECT_EQ(stats().bundles_written(), 0u);
  EXPECT_EQ(stats().chunks_rewritten(), 1u);
  EXPECT_EQ(stats().chunks_relocated(), 0u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  const auto packets = ReadAll();
  ASSERT_EQ(packets.size(), 62u);
  EXPECT_EQ(packets[0].str, "first");
  EXPECT_EQ(packets[1].str, "second");
}

// Reject framing errors before a pressure pass, so rejected input does not
// cause staging packets to be bundled.
TEST_F(TraceBufferV3Test, RejectedInputDoesNotDrainStaging) {
  Params params = BundleOnlyOnRead();
  params.staging_capacity_bytes = 4096;
  ResetBuffer(64 * 1024, params);
  for (ChunkID id = 0; id < 64; ++id)
    WriteChunk(1, 1, id, {Packet(std::string(40, 'x'))});
  ASSERT_EQ(staging()->used_size(), staging()->size());
  SuppressClientDchecksForTesting();
  WriteChunk(2, 1, 0, {Packet(std::string(5000, 'y'))});
  EXPECT_FALSE(CopyChunkV2(2, 1, {std::string(5000, 'z')}));
  EXPECT_FALSE(CopyChunkV2(1, 1, {PacketV2("wrong-format")}));
  EXPECT_EQ(stats().abi_violations(), 3u);
  EXPECT_EQ(num_staging_sequences(), 1u);
  EXPECT_EQ(stats().bundles_written(), 0u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_EQ(ReadAll().size(), 64u);
}

TEST_F(TraceBufferV3Test, PressureRetryAccountsInputOnce) {
  for (bool writer_abort : {false, true}) {
    SCOPED_TRACE(writer_abort);
    Params params = BundleOnlyOnRead();
    params.staging_capacity_bytes = 4096;
    ResetBuffer(64 * 1024, params);
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
    EXPECT_GT(stats().bundles_written(), 0u);
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
      EXPECT_EQ(packets[i].loss(), 0u);
    EXPECT_EQ(
        packets.back().loss(),
        writer_abort
            ? kLossPresent | protos::pbzero::TracePacket::DATA_LOSS_WRITER_ABORT
            : 0u);
  }
}

// A packet that waits for a patch blocks its sequence, so the pressure pass
// bundles nothing. The retry overwrites the blocked chunks, and the first
// packet after them reports the overwrite.
TEST_F(TraceBufferV3Test, BlockedStagingPressureReportsLoss) {
  Params params = BundleOnlyOnRead();
  params.staging_capacity_bytes = 4096;
  ResetBuffer(64 * 1024, params);
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
  EXPECT_EQ(packets[0].loss_from_buffer, kLossPresent | kLossOverwrite);
}

TEST_F(TraceBufferV3Test, CheckpointsDoNotRetainPermanentGaps) {
  ResetBuffer(64 * 1024, OneBundlePerPacket());
  WriteBatch(Packet("first"));
  // Chunk 1 never arrives. Later batches still reach bundled storage, even
  // when their total size exceeds staging capacity.
  for (ChunkID id = 2; id <= 66; ++id) {
    WriteChunk(1, 1, id, {Packet(std::string(40, 'x'))});
    trace_buffer_->OnWriteCheckpoint();
  }
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_EQ(stats().bundles_overwritten(), 0u);
  const auto packets = ReadAll();
  ASSERT_EQ(packets.size(), 66u);
  EXPECT_EQ(packets[0].str, "first");
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[1].loss_from_buffer, kLossPresent | kLossReadGap);
  for (size_t i = 2; i < packets.size(); ++i)
    EXPECT_EQ(packets[i].loss(), 0u);
}

TEST_F(TraceBufferV3Test, DiscardKeepsAlreadyAcceptedStagingAndPatches) {
  const Params params = OneBundlePerPacket();
  const std::string first = Packet(std::string(40, 'a'));
  ResetBuffer(BundleSize(params, {first}) + 4, params, TraceBuffer::kDiscard);
  WriteBatch(first);
  const std::string split = Packet("XXXX-tail");
  CreateChunk(1, 2, 0)
      .AddFragment(split.substr(0, 9), kContOnNextChunk | kChunkNeedsPatching)
      .CopyIntoTraceBuffer();
  CreateChunk(1, 2, 1)
      .AddFragment(split.substr(9), kContFromPrevChunk)
      .CopyIntoTraceBuffer();
  // Both these packets are accepted before the checkpoint stops intake.
  WriteChunk(1, 1, next_chunk_id_++, {Packet("pending"), Packet("staged")});
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_TRUE(has_overflow_bundle());
  EXPECT_FALSE(CopyChunkV2(2, 1, {PacketV2("rejected")}));
  const TraceBuffer::Patch patch{6, {'d', 'o', 'n', 'e'}};
  ASSERT_TRUE(trace_buffer_->TryPatchChunkContents(1, 2, 0, &patch, 1, false));
  const auto packets = ReadAll();
  EXPECT_THAT(Strs(packets), ElementsAre(std::string(40, 'a'), "pending",
                                         "done-tail", "staged"));
  EXPECT_FALSE(CopyChunkV2(2, 1, {PacketV2("still-rejected")}));
}

// Exactly full kDiscard staging does not stop intake. A checkpoint bundles
// it and the next chunk is accepted.
TEST_F(TraceBufferV3Test, DiscardExactlyFullStagingDoesNotStopIntake) {
  const Params params = OneBundlePerPacket();
  const std::string packet = Packet(std::string(40, 'a'));
  ResetBuffer(64 * 1024, params, TraceBuffer::kDiscard);
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

// A pressure pass can stop intake before the write retry.
// The retry must reject the prepared chunk.
TEST_F(TraceBufferV3Test, DiscardPressureStopsAtCompressedCapacity) {
  const Params params = OneBundlePerPacket();
  const std::string payload(40, 'a');
  const std::string packet = Packet(payload);
  for (bool v2 : {false, true}) {
    SCOPED_TRACE(v2);
    const size_t capacity = BundleSize(params, {packet});
    ResetBuffer(capacity, params, TraceBuffer::kDiscard);
    for (ChunkID id = 0; id < 64; ++id) {
      if (v2)
        ASSERT_TRUE(CopyChunkV2(1, 1, {PacketV2(payload)}));
      else
        WriteChunk(1, 1, id, {packet});
    }
    ASSERT_EQ(staging()->used_size(), staging()->size());
    EXPECT_EQ(stats().bundles_written(), 0u);
    EXPECT_FALSE(staging_discard_writes());

    if (v2)
      EXPECT_FALSE(CopyChunkV2(2, 1, {PacketV2("rejected")}));
    else
      WriteChunk(2, 1, 0, {Packet("rejected")});
    EXPECT_EQ(stats().staging_pressure_passes(), 1u);
    EXPECT_EQ(stats().bundles_written(), 1u);
    EXPECT_EQ(stats().bundle_bytes_stored(), capacity);
    EXPECT_EQ(stats().chunks_written(), 64u);
    EXPECT_EQ(stats().chunks_discarded(), 1u);
    EXPECT_TRUE(staging_discard_writes());

    // Checkpoints stop, but accepted packets remain readable.
    trace_buffer_->OnWriteCheckpoint();
    EXPECT_EQ(stats().bundles_written(), 1u);
    const auto packets = ReadAll();
    ASSERT_EQ(packets.size(), 64u);
    for (const auto& result : packets) {
      EXPECT_EQ(result.str, payload);
      EXPECT_EQ(result.loss(), 0u);
    }
    trace_buffer_->MaybeCompact();
    EXPECT_EQ(staging()->used_size(), 0u);
    if (v2)
      EXPECT_FALSE(CopyChunkV2(2, 1, {PacketV2("still-rejected")}));
    else
      WriteChunk(2, 1, 0, {Packet("still-rejected")});
    EXPECT_EQ(stats().chunks_discarded(), 2u);
    EXPECT_EQ(stats().staging_pressure_passes(), 1u);
    EXPECT_THAT(ReadAll(), IsEmpty());
  }
}

// A complete split packet waits for its final patch notification.
// If pressure cannot reclaim its space, the next chunk seals DISCARD intake.
TEST_F(TraceBufferV3Test, DiscardStopsWhenStagingWaitsForPatches) {
  const Params params = OneBundlePerPacket();
  ResetBuffer(64 * 1024, params, TraceBuffer::kDiscard);
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
  EXPECT_EQ(stats().staging_pressure_passes(), 1u);
  EXPECT_EQ(stats().bundles_written(), 0u);
  EXPECT_EQ(stats().chunks_written(), 2u);
  EXPECT_EQ(stats().chunks_discarded(), 1u);
  EXPECT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_TRUE(staging_discard_writes());

  // Patches still unblock accepted data after intake stops.
  ASSERT_TRUE(trace_buffer_->TryPatchChunkContents(1, 1, 0, nullptr, 0, false));
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(stats().bundles_written(), 0u);
  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre(payload));
  EXPECT_EQ(packets[0].loss(), 0u);
  trace_buffer_->MaybeCompact();
  EXPECT_EQ(staging()->used_size(), 0u);
  WriteChunk(2, 1, 0, {Packet("still-rejected")});
  EXPECT_EQ(stats().chunks_discarded(), 2u);
  EXPECT_EQ(stats().staging_pressure_passes(), 1u);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

TEST_F(TraceBufferV3Test, DiscardRejectsScrapedChunkCompletion) {
  const Params params = OneBundlePerPacket();
  ResetBuffer(BundleSize(params, {Packet("a")}) + 4, params,
              TraceBuffer::kDiscard);
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("a"))
      .AddFragment(Packet("b"))
      .AddFragment(Packet("unfinished"))
      .PadTo(256)
      .CopyIntoTraceBuffer(/*chunk_complete=*/false);
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_TRUE(has_overflow_bundle());
  ASSERT_TRUE(staging_discard_writes());
  CreateChunk(1, 1, 0)
      .AddFragment(Packet("a"))
      .AddFragment(Packet("b"))
      .AddFragment(Packet("c"))
      .CopyIntoTraceBuffer();
  EXPECT_EQ(stats().chunks_discarded(), 1u);
  EXPECT_EQ(stats().chunks_rewritten(), 0u);
  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("a", "b"));
  for (const auto& packet : packets)
    EXPECT_EQ(packet.loss(), 0u);
  WriteChunk(1, 1, 1, {Packet("rejected")});
  EXPECT_EQ(stats().chunks_discarded(), 2u);
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// ---------------------------------------------------------------------------
// Invalid packets and corrupted bundles
// ---------------------------------------------------------------------------

// The service receives the exact malformed bytes, with loss outside them.
// In particular, a loss field must not complete the truncated packet.
TEST_F(TraceBufferV3Test, ReturnsInvalidPacketsWithoutStampingLoss) {
  ResetBuffer(4096, OneBundlePerPacket());
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

// Corrupt bundles are dropped and counted once each.
// Their sequences report loss, and the read continues with the next bundle.
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
      {"sequence index out of range", good_record + Record(1, Packet("x")),
       std::nullopt, std::nullopt, 1},
      {"packet ends after the bundle",
       good_record + std::string("\x00\x00\x7f", 3) + "short", std::nullopt,
       std::nullopt, 1},
      {"loss reasons varint is cut off",
       good_record + std::string("\x00\x80", 2), std::nullopt, std::nullopt, 1},
      {"loss reasons exceed 32 bits",
       good_record + std::string("\x00\x80\x80\x80\x80\x10\x01", 7) + "x",
       std::nullopt, std::nullopt, 1},
      {"packet size varint is cut off",
       good_record + std::string("\x00\x00\x80", 3), std::nullopt, std::nullopt,
       1},
      {"sequence index varint is too long",
       good_record + std::string(11, '\xff') + Packet("x"), std::nullopt,
       std::nullopt, 1},
      {"empty packet", good_record + std::string("\x00\x00\x00", 3),
       std::nullopt, std::nullopt, 1},
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    next_chunk_id_ = 0;
    ResetBuffer(4096, OneBundlePerPacket());
    const size_t initial_decode_memory = decode_memory_bytes();
    WriteBatch(Packet("before"));
    AppendRawBundle({id}, test_case.records, test_case.uncompressed_size,
                    test_case.frame);
    // The next bundle is corrupt too, so recovery must keep trying.
    AppendRawBundle({id}, good_record, 0u);  // Header says no records.
    WriteBatch(Packet("after"));
    ASSERT_EQ(num_bundled_sequences(), 1u);

    std::vector<TestPacket> packets = ReadAll();
    std::vector<std::string> expected = {"before"};
    if (test_case.num_packets_before_error)
      expected.push_back("in-bad-bundle");
    expected.push_back("after");
    ASSERT_THAT(Strs(packets), ElementsAreArray(expected));
    EXPECT_EQ(packets.back().loss_from_buffer, kLossPresent | kLossCorrupted);
    EXPECT_EQ(stats().bundles_corrupted(), 2u);
    EXPECT_EQ(stats().bundles_overwritten(), 0u);
    EXPECT_EQ(decode_memory_bytes(), initial_decode_memory);

    // No reference to the sequence leaked, and the buffer still works.
    EXPECT_EQ(num_bundled_sequences(), 0u);
    EXPECT_EQ(trace_buffer_->used_size(), 0u);
    ExpectNoDecodedBundle();
    WriteBatch(Packet("next"));
    packets = ReadAll();
    ASSERT_THAT(Strs(packets), ElementsAre("next"));
    EXPECT_EQ(packets[0].loss(), 0u);
  }
}

// A malformed record drops the rest of the bundle for every sequence.
// Even a sequence whose records were never reached must report that loss.
TEST_F(TraceBufferV3Test, MalformedRecordDropsRestOfBundleForEverySequence) {
  const ProducerAndWriterID s = MkProducerAndWriterID(1, 1);
  const ProducerAndWriterID t = MkProducerAndWriterID(1, 2);
  ResetBuffer(4096, OneBundlePerPacket());
  WriteChunk(1, 1, 0, {Packet("s1")});
  WriteChunk(1, 2, 0, {Packet("t1")});
  trace_buffer_->OnWriteCheckpoint();
  // The malformed record claims 127 bytes, so t2 is never reached.
  AppendRawBundle({s, t}, Record(0, Packet("s2")) +
                              std::string("\x00\x00\x7f", 3) + "short" +
                              Record(1, Packet("t2")));
  WriteChunk(1, 1, 1, {Packet("s3")});
  WriteChunk(1, 2, 1, {Packet("t3")});
  trace_buffer_->OnWriteCheckpoint();

  const auto packets = ReadAll();
  ASSERT_THAT(Strs(packets), ElementsAre("s1", "t1", "s2", "s3", "t3"));
  EXPECT_EQ(packets[2].loss(), 0u);
  EXPECT_EQ(packets[3].loss_from_buffer, kLossPresent | kLossCorrupted);
  EXPECT_EQ(packets[4].loss_from_buffer, kLossPresent | kLossCorrupted);
  EXPECT_EQ(stats().bundles_corrupted(), 1u);
  EXPECT_EQ(num_bundled_sequences(), 0u);
}

// ---------------------------------------------------------------------------
// Sequences
// ---------------------------------------------------------------------------

TEST_F(TraceBufferV3Test, InterleavedSequencesSurviveMapGrowth) {
  Params params = BundleOnlyOnRead();
  params.staging_capacity_bytes = 128 * 1024;
  ResetBuffer(1024 * 1024, params);

  // Grow both sequence maps within one bundle.
  // Consecutive packets also exercise the cached entry after each insertion.
  constexpr WriterID kNumWriters = 1024;
  std::vector<std::pair<WriterID, std::string>> expected;
  std::vector<ProducerAndWriterID> expected_sequences;
  for (WriterID writer = 1; writer <= kNumWriters; ++writer) {
    const std::string str = std::to_string(writer);
    WriteChunk(1, writer, 0, {Packet(str), Packet(str)});
    expected.emplace_back(writer, str);
    expected.emplace_back(writer, str);
    expected_sequences.push_back(MkProducerAndWriterID(1, writer));
  }
  // This writer must reuse its index and reference after the maps grow.
  WriteChunk(1, 1, 1, {Packet("returning-writer")});
  expected.emplace_back(1, "returning-writer");
  BundleNow();
  ASSERT_EQ(stats().bundles_written(), 1u);
  EXPECT_EQ(FrontBundleMetadata().sequence_list, expected_sequences);
  EXPECT_EQ(num_bundled_sequences(), kNumWriters);
  ExpectNoOpenBundle();
  const size_t first_bundle_packets = expected.size();

  // Reuse the construction map with different indexes for these writers.
  // Their shared entries now have references from both bundles.
  WriteChunk(1, kNumWriters, 1, {Packet("next-last")});
  WriteChunk(1, 1, 2, {Packet("next-first")});
  expected.emplace_back(kNumWriters, "next-last");
  expected.emplace_back(1, "next-first");
  BundleNow();
  ASSERT_EQ(stats().bundles_written(), 2u);
  ExpectNoOpenBundle();

  trace_buffer_->BeginRead();
  for (size_t i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    TestPacket packet;
    ASSERT_TRUE(ReadOne(trace_buffer_.get(), &packet));
    EXPECT_EQ(packet.writer_id, expected[i].first);
    EXPECT_EQ(packet.str, expected[i].second);
    EXPECT_EQ(packet.producer_id, 1u);
    EXPECT_EQ(packet.uid, 1001u);
    EXPECT_EQ(packet.loss(), 0u);
    if (i + 1 == first_bundle_packets) {
      EXPECT_EQ(num_bundled_sequences(), 2u);
      ExpectNoDecodedBundle();
    }
  }
  EXPECT_EQ(num_bundled_sequences(), 0u);
  ExpectNoDecodedBundle();
  EXPECT_THAT(ReadAll(), IsEmpty());
}

// Staging keeps the state of the last 1024 sequences that have no chunk and
// prunes older ones. A sequence whose packets are in a bundle has no chunk in
// staging either, but its state must stay to detect gaps and duplicate
// chunks of that writer.
TEST_F(TraceBufferV3Test, SequencesInBundlesAreNotPruned) {
  Params params = OneBundlePerPacket();
  // The later chunks wait at gaps until readout. Give them enough staging
  // space so this test exercises sequence pruning without overwrites.
  params.staging_capacity_bytes = 128 * 1024;
  ResetBuffer(1024 * 1024, params);
  // Exceed the pruning threshold of 1024 retained sequences plus 128.
  constexpr WriterID kNumWriters = 1200;
  for (WriterID writer = 1; writer <= kNumWriters; writer++) {
    WriteChunk(1, writer, 0, {Packet(std::to_string(writer))});
    trace_buffer_->OnWriteCheckpoint();
  }

  // All the chunks were bundled. No sequence counts as empty, so no state was
  // pruned, although their count exceeds the pruning threshold.
  EXPECT_EQ(staging()->used_size(), 0u);
  EXPECT_EQ(num_staging_sequences(), kNumWriters);
  EXPECT_EQ(num_empty_staging_sequences(), 0u);
  EXPECT_EQ(num_bundled_sequences(), kNumWriters);

  // Every writer skips a chunk. Each gap is detected, because the state of
  // each sequence is still there.
  for (WriterID writer = 1; writer <= kNumWriters; writer++) {
    WriteChunk(1, writer, 2, {Packet(std::to_string(writer) + "-gap")});
    trace_buffer_->OnWriteCheckpoint();
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
  trace_buffer_->OnWriteCheckpoint();
  EXPECT_EQ(Strs(ReadAll()).size(), 1u);
}

// The sequences of the overflow bundle of a kDiscard buffer are kept too.
TEST_F(TraceBufferV3Test, SequencesOfOverflowBundleAreKept) {
  const Params params = OneBundlePerPacket();
  const std::string a = Packet(std::string(40, 'a'));
  ResetBuffer(BundleSize(params, {a}) + 4, params, TraceBuffer::kDiscard);
  WriteBatch(a);
  WriteChunk(1, 2, 0, {Packet(std::string(40, 'b'))});
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_TRUE(has_overflow_bundle());
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
  trace_buffer_->OnWriteCheckpoint();
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

// The clone of a kDiscard buffer keeps the overflow bundle in its place.
TEST_F(TraceBufferV3Test, CloneWithOverflowBundle) {
  const Params params = BundleOnlyOnRead();
  const std::string first = RandomBytes(500, 1);
  const std::string a = Packet(first);
  ResetBuffer(BundleSize(params, {a}) + 4, params, TraceBuffer::kDiscard);
  WriteChunk(1, 1, 0, {a});
  BundleNow();
  // Both sequences share an overflow bundle that fits the capacity.
  WriteChunk(1, 2, 0, {Packet(std::string(40, 'b'))});
  WriteChunk(1, 3, 0, {Packet(std::string(40, 'c'))});
  BundleNow();
  ASSERT_TRUE(has_overflow_bundle());
  EXPECT_EQ(num_bundled_sequences(), 3u);

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  ASSERT_TRUE(clone);
  EXPECT_EQ(clone->overwrite_policy(), TraceBuffer::kDiscard);
  EXPECT_GT(clone->GetMemoryUsageBytes(), 0u);
  const uint64_t stored_bytes = stats().bundle_bytes_stored();
  EXPECT_EQ(clone->stats().bundle_bytes_stored(), stored_bytes);
  EXPECT_EQ(ReadAll().size(), 3u);
  EXPECT_FALSE(has_overflow_bundle());
  EXPECT_EQ(stats().bundle_bytes_stored(), stored_bytes);
  trace_buffer_.reset();
  std::vector<TestPacket> packets = perfetto::ReadAll(clone.get());
  ASSERT_THAT(Strs(packets),
              ElementsAre(first, std::string(40, 'b'), std::string(40, 'c')));
  EXPECT_EQ(packets[0].writer_id, 1u);
  EXPECT_EQ(packets[1].writer_id, 2u);
  EXPECT_EQ(packets[2].writer_id, 3u);
  for (const TestPacket& packet : packets)
    EXPECT_EQ(packet.loss(), 0u);
  EXPECT_EQ(clone->stats().bundle_bytes_stored(), stored_bytes);
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

// Staging pressure bundles P2 after P1. Eviction feeds ProtoVM in that order.
TEST_F(TraceBufferV3Test, ProtoVmGetsPressureOverwritesInOrder) {
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
             {p2});  // No OnWriteCheckpoint(), so it stays in staging.

  // Another producer fills staging. Pressure bundles P2 before reusing its
  // space.
  for (ChunkID chunk_id = 0; chunk_id < 64; chunk_id++)
    WriteChunk(2, 1, chunk_id, {other});
  ASSERT_EQ(stats().chunks_overwritten(), 0u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "P2");

  // The checkpoint finishes the other producer's remaining staging data.
  trace_buffer_->OnWriteCheckpoint();
  ASSERT_GT(stats().bundles_overwritten(), 0u);
  ASSERT_EQ(stats().bundles_too_large(), 0u);
  EXPECT_EQ(ProtoVmState(trace_buffer_.get()).string_merged(), "P2");

  // The next packet of the writer reports the loss of P1 and of P2.
  WriteBatch(PatchPacket("P3"));
  std::vector<TestPacket> packets = ReadAll();
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].str, "P3");
  EXPECT_EQ(packets[0].loss(), kLossPresent | kLossOverwrite);
}

// P2 remains in decoded scratch. ProtoVM only sees the newer stored P3 when
// P4 overwrites it. A clone keeps both P2 and the ProtoVM state.
TEST_F(TraceBufferV3Test, ProtoVmDoesNotGetRetainedDecodedPackets) {
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
  ASSERT_EQ(stats().bundles_overwritten(), 0u);
  WriteChunk(1, 1, 2, {PatchPacket(std::string(60, '4'))});
  BundleNow();
  ASSERT_EQ(stats().bundles_overwritten(), 1u);
  ASSERT_EQ(stats().bundles_too_large(), 0u);
  auto state = ProtoVmState(trace_buffer_.get());
  EXPECT_EQ(state.string_merged(), "P3");
  // P1 had int_to_merge. The ProtoVM never saw it.
  EXPECT_FALSE(state.has_int_merged());

  std::unique_ptr<TraceBuffer> clone = trace_buffer_->CloneReadOnly();
  trace_buffer_.reset();
  EXPECT_EQ(ProtoVmState(clone.get()).string_merged(), "P3");
  std::vector<TestPacket> packets = perfetto::ReadAll(clone.get());
  ASSERT_EQ(packets.size(), 2u);
  EXPECT_EQ(packets[0].str, "P2-" + std::string(40, '2'));
  EXPECT_EQ(packets[0].loss(), 0u);
  EXPECT_EQ(packets[1].str, std::string(60, '4'));
  EXPECT_EQ(packets[1].loss(), kLossPresent | kLossOverwrite);
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

  // Three chunks of 64 bytes, each one a bundle with a record of 48 bytes.
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
    EXPECT_EQ(s.bundle_bytes_uncompressed(), 3 * 48u);
    EXPECT_EQ(s.bundle_bytes_stored(), 3 * size_a);
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
  EXPECT_EQ(stats().bytes_read(), 2 * 48u);
  EXPECT_EQ(stats().chunks_read(), 0u);
  EXPECT_EQ(stats().staging_chunks_read(), 3u);

  // The histogram of chunk payload sizes comes from staging.
  size_t num_writers = 0;
  for (auto it = trace_buffer_->writer_stats().GetIterator(); it; ++it)
    num_writers++;
  EXPECT_EQ(num_writers, 1u);

  TraceStats trace_stats;
  *trace_stats.add_buffer_stats() = stats();
  TraceStats decoded;
  ASSERT_TRUE(decoded.ParseFromString(trace_stats.SerializeAsString()));
  const auto& serialized = decoded.buffer_stats()[0];
  EXPECT_THAT(serialized.bundling_time_us_histogram_def(),
              ElementsAre(10, 100, 1000, 10000, 100000));
  ASSERT_EQ(serialized.bundling_time_us_histogram_counts().size(), 6u);
  ASSERT_EQ(serialized.bundling_time_us_histogram_sum().size(), 6u);
  uint64_t count = 0;
  uint64_t sum = 0;
  for (size_t i = 0; i < 6; ++i) {
    count += serialized.bundling_time_us_histogram_counts()[i];
    sum +=
        static_cast<uint64_t>(serialized.bundling_time_us_histogram_sum()[i]);
  }
  // Three checkpoints and the final read attempt on empty staging. Building
  // and evicting a bundle inside a pass does not add another measurement.
  EXPECT_EQ(count, 4u);
  EXPECT_LE(serialized.bundling_max_us(), sum);
  EXPECT_LE(sum, count * serialized.bundling_max_us());
}

// The memory bound covers the memory that is allocated later, on the first
// bundle and the first read, so it does not change when that happens.
TEST_F(TraceBufferV3Test, MemoryUsage) {
  trace_buffer_ = TraceBufferV3::Create(1024 * 1024);
  ASSERT_TRUE(trace_buffer_);
  EXPECT_EQ(params().bundle_target_bytes, 128 * 1024u);
  EXPECT_EQ(params().staging_capacity_bytes, 1024 * 1024u);
  const size_t initial = trace_buffer_->GetMemoryUsageBytes();
  EXPECT_EQ(decode_memory_bytes(), 2 * 1024 * 1024u);
  const size_t buffer_memory = 2 * 1024 * 1024u + decode_memory_bytes();
  EXPECT_GT(initial, buffer_memory);
  EXPECT_LT(initial, buffer_memory + 3 * 1024 * 1024u);

  for (ChunkID chunk_id = 0; chunk_id < 200; chunk_id++) {
    WriteChunk(1, 1, chunk_id, {Packet(RandomBytes(2000, chunk_id))});
    trace_buffer_->OnWriteCheckpoint();
  }
  EXPECT_GT(stats().bundles_written(), 1u);
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
    size_t bundle_target_bytes;
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
    trace_buffer_ =
        TraceBufferV3::Create(test_case.size, TraceBuffer::kOverwrite);
    ASSERT_TRUE(trace_buffer_);
    const size_t capacity = base::AlignUp(test_case.size, 4096);
    EXPECT_EQ(trace_buffer_->size(), capacity);
    EXPECT_EQ(staging()->size(), capacity);
    EXPECT_EQ(params().bundle_target_bytes, test_case.bundle_target_bytes);
    EXPECT_EQ(decode_memory_bytes(), test_case.decode_memory_bytes);
    EXPECT_GE(trace_buffer_->GetMemoryUsageBytes(),
              2 * capacity + test_case.decode_memory_bytes);
  }
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
  params.staging_capacity_bytes = 1024 * 1024;
  params.bundle_target_bytes = 3000;
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
      buffer->OnWriteCheckpoint();
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
