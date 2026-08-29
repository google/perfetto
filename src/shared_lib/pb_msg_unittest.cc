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

#include "perfetto/public/pb_msg.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/public/pb_macros.h"
#include "src/shared_lib/stream_writer.h"
#include "test/gtest_and_gmock.h"

namespace {

using ::testing::ElementsAreArray;

PERFETTO_PB_MSG(PbMsgTestFields);
PERFETTO_PB_FIELD(PbMsgTestFields, STRING, const char*, bytes, 1);
PERFETTO_PB_FIELD(PbMsgTestFields, PACKED, Uint32, values, 2);

// ---------------------------------------------------------------------------
// The public C ABI.
//
// The implementation reuses the high bits of PerfettoPbMsg::size and
// replaced the leading union with its pointer member. That is meant to preserve
// size, alignment and every field offset for callers that were compiled against
// the old header. These assertions are the evidence for that claim, and they
// evaluate correctly on both pointer widths.
// ---------------------------------------------------------------------------

struct LegacyPerfettoPbMsg {
  uint8_t* size_field;
  uint32_t size;
  struct PerfettoPbMsgWriter* writer;
  struct LegacyPerfettoPbMsg* nested;
  struct LegacyPerfettoPbMsg* parent;
};

static_assert(sizeof(PerfettoPbMsg) == sizeof(LegacyPerfettoPbMsg),
              "PerfettoPbMsg changed size; this breaks the public C ABI");
static_assert(alignof(PerfettoPbMsg) == alignof(LegacyPerfettoPbMsg),
              "PerfettoPbMsg changed alignment; this breaks the public C ABI");
static_assert(offsetof(PerfettoPbMsg, size_field) ==
                  offsetof(LegacyPerfettoPbMsg, size_field),
              "PerfettoPbMsg::size_field moved; this breaks the public C ABI");
static_assert(offsetof(PerfettoPbMsg, size) ==
                  offsetof(LegacyPerfettoPbMsg, size),
              "PerfettoPbMsg::size moved; this breaks the public C ABI");
static_assert(offsetof(PerfettoPbMsg, writer) ==
                  offsetof(LegacyPerfettoPbMsg, writer),
              "PerfettoPbMsg::writer moved; this breaks the public C ABI");
static_assert(offsetof(PerfettoPbMsg, nested) ==
                  offsetof(LegacyPerfettoPbMsg, nested),
              "PerfettoPbMsg::nested moved; this breaks the public C ABI");
static_assert(offsetof(PerfettoPbMsg, parent) ==
                  offsetof(LegacyPerfettoPbMsg, parent),
              "PerfettoPbMsg::parent moved; this breaks the public C ABI");

// The state bits must sit above every size a protobuf message can legally
// reach, so that a plain `size += n` never disturbs them.
static_assert(PERFETTO_PB_MSG_SIZE_MASK >=
                  protozero::proto_utils::kMaxMessageLength,
              "The state bits must not shrink the representable message size");
static_assert(
    (PERFETTO_PB_MSG_PROTO_GROUP_BIT & PERFETTO_PB_MSG_SIZE_MASK) == 0 &&
        (PERFETTO_PB_MSG_FINALIZED_BIT & PERFETTO_PB_MSG_SIZE_MASK) == 0 &&
        (PERFETTO_PB_MSG_BUFFERED_BIT & PERFETTO_PB_MSG_SIZE_MASK) == 0,
    "The state bits must not overlap the size");

// ---------------------------------------------------------------------------
// Test plumbing.
// ---------------------------------------------------------------------------

// A flat, oversized buffer. Nothing written through it comes close to the end,
// so the stream writer's slow paths are never taken; ChunkedCBuffer below is
// what exercises those.
class FlatCBuffer {
 public:
  FlatCBuffer() {
    writer_.writer.impl = nullptr;
    writer_.writer.begin = storage_;
    writer_.writer.write_ptr = storage_;
    writer_.writer.end = storage_ + sizeof(storage_);
  }

  PerfettoPbMsgWriter* writer() { return &writer_; }

  std::vector<uint8_t> bytes() const {
    return std::vector<uint8_t>(
        storage_, static_cast<const uint8_t*>(writer_.writer.write_ptr));
  }

 private:
  uint8_t storage_[4096]{};
  PerfettoPbMsgWriter writer_{};
};

// A real protozero::ScatteredStreamWriter behind the C stream writer, handing
// out deliberately small ranges so that everything the C code writes crosses
// several of them. This is what makes PerfettoPbMsgPatchStack() run.
class ChunkedCBuffer : public protozero::ScatteredStreamWriter::Delegate {
 public:
  explicit ChunkedCBuffer(size_t range_size)
      : range_size_(range_size), stream_writer_(this) {
    stream_writer_.Reset(GetNewBuffer());
    writer_.writer.impl =
        reinterpret_cast<PerfettoStreamWriterImpl*>(&stream_writer_);
    perfetto::UpdateStreamWriter(stream_writer_, &writer_.writer);
  }

  protozero::ContiguousMemoryRange GetNewBuffer() override {
    ranges_.push_back(std::unique_ptr<uint8_t[]>(new uint8_t[range_size_]()));
    uint8_t* const begin = ranges_.back().get();
    return {begin, begin + range_size_};
  }

  PerfettoPbMsgWriter* writer() { return &writer_; }
  size_t num_ranges() const { return ranges_.size(); }

  // Everything written so far, stitched back into one buffer.
  std::vector<uint8_t> bytes() {
    stream_writer_.set_write_ptr(writer_.writer.write_ptr);
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < ranges_.size(); ++i)
      out.insert(out.end(), ranges_[i].get(), ranges_[i].get() + range_size_);
    out.insert(out.end(), ranges_.back().get(), stream_writer_.write_ptr());
    return out;
  }

 private:
  const size_t range_size_;
  std::vector<std::unique_ptr<uint8_t[]>> ranges_;
  protozero::ScatteredStreamWriter stream_writer_;
  PerfettoPbMsgWriter writer_{};
};

// Builds the same message shape through the C++ runtime, so the two can be
// compared byte for byte.
std::vector<uint8_t> BuildWithCpp(protozero::NestedMessageEncoding encoding) {
  class Buffer : public protozero::ScatteredStreamWriter::Delegate {
   public:
    Buffer() : stream_writer(this) {}
    protozero::ContiguousMemoryRange GetNewBuffer() override {
      return {storage, storage + sizeof(storage)};
    }
    uint8_t storage[4096]{};
    protozero::ScatteredStreamWriter stream_writer;
  };
  Buffer buffer;
  buffer.stream_writer.Reset(buffer.GetNewBuffer());

  protozero::RootMessage<protozero::Message> root;
  root.Reset(&buffer.stream_writer, encoding);
  root.AppendVarInt(1, 7);
  protozero::Message* nested = root.BeginNestedMessage<protozero::Message>(2);
  nested->AppendVarInt(3, 8);
  protozero::Message* inner = nested->BeginNestedMessage<protozero::Message>(4);
  inner->AppendBytes(5, "abc", 3);
  root.AppendVarInt(6, 9);
  root.Finalize();
  return std::vector<uint8_t>(buffer.storage, buffer.stream_writer.write_ptr());
}

// The same shape through the C runtime. The call sequences are not identical
// because the two APIs differ in an unrelated way - the C++ runtime closes an
// open nested message when the parent's next field is written, while the C one
// wants an explicit EndNested - but the bytes they produce must match exactly.
std::vector<uint8_t> BuildWithC(enum PerfettoPbMsgEncoding encoding) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  PerfettoPbMsgInitWithEncoding(&root, buffer.writer(), encoding);
  PerfettoPbMsgAppendType0Field(&root, 1, 7);
  PerfettoPbMsg nested;
  PerfettoPbMsgBeginNested(&root, &nested, 2);
  PerfettoPbMsgAppendType0Field(&nested, 3, 8);
  PerfettoPbMsg inner;
  PerfettoPbMsgBeginNested(&nested, &inner, 4);
  PerfettoPbMsgAppendType2Field(&inner, 5,
                                reinterpret_cast<const uint8_t*>("abc"), 3);
  PerfettoPbMsgEndNested(&nested);
  PerfettoPbMsgEndNested(&root);
  PerfettoPbMsgAppendType0Field(&root, 6, 9);
  PerfettoPbMsgFinalize(&root);
  return buffer.bytes();
}

// ---------------------------------------------------------------------------
// Encodings.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, DefaultInitIsLengthDelimited) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  PerfettoPbMsgInit(&root, buffer.writer());
  EXPECT_FALSE(PerfettoPbMsgUsesProtoGroup(&root));

  PerfettoPbMsg nested;
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);
  EXPECT_FALSE(PerfettoPbMsgUsesProtoGroup(&nested));
  PerfettoPbMsgAppendType0Field(&nested, /*field_id=*/2, 2);
  PerfettoPbMsgFinalize(&root);

  // Field 1 preamble, the four-byte redundant length of 2, then the field. The
  // C runtime does not compact, so the length stays four bytes wide.
  const uint8_t kExpected[] = {0x0a, 0x82, 0x80, 0x80, 0x00, 0x10, 0x02};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupEmptyNestedMessage) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  PerfettoPbMsgInitWithEncoding(&root, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  EXPECT_TRUE(PerfettoPbMsgUsesProtoGroup(&root));

  PerfettoPbMsg nested;
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);
  EXPECT_TRUE(PerfettoPbMsgUsesProtoGroup(&nested));
  EXPECT_EQ(nested.size_field, nullptr);
  PerfettoPbMsgFinalize(&root);

  const uint8_t kExpected[] = {0x0b, 0x04};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupNestedInsideNested) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  PerfettoPbMsgInitWithEncoding(&root, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  PerfettoPbMsg nested;
  PerfettoPbMsg inner;
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);
  PerfettoPbMsgBeginNested(&nested, &inner, /*field_id=*/2);
  PerfettoPbMsgFinalize(&root);

  const uint8_t kExpected[] = {0x0b, 0x13, 0x04, 0x04};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupRootEmitsNoEndByte) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  PerfettoPbMsgInitWithEncoding(&root, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  PerfettoPbMsgAppendType0Field(&root, /*field_id=*/1, 42);
  PerfettoPbMsgFinalize(&root);

  const uint8_t kExpected[] = {0x08, 0x2a};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, FinalizeIsIdempotentInBothModes) {
  {
    FlatCBuffer buffer;
    PerfettoPbMsg root;
    PerfettoPbMsgInitWithEncoding(&root, buffer.writer(),
                                  PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
    PerfettoPbMsg nested;
    PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);
    EXPECT_EQ(PerfettoPbMsgFinalize(&nested), 1u);
    // A second call must not emit another end byte.
    EXPECT_EQ(PerfettoPbMsgFinalize(&nested), 1u);
    PerfettoPbMsgEndNested(&root);
    EXPECT_EQ(PerfettoPbMsgFinalize(&root), 2u);
    EXPECT_EQ(PerfettoPbMsgFinalize(&root), 2u);
    const uint8_t kExpected[] = {0x0b, 0x04};
    EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
  }
  {
    FlatCBuffer buffer;
    PerfettoPbMsg root;
    PerfettoPbMsgInit(&root, buffer.writer());
    PerfettoPbMsgAppendType0Field(&root, /*field_id=*/1, 1);
    EXPECT_EQ(PerfettoPbMsgFinalize(&root), 2u);
    EXPECT_EQ(PerfettoPbMsgFinalize(&root), 2u);
    const uint8_t kExpected[] = {0x08, 0x01};
    EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
  }
}

// ---------------------------------------------------------------------------
// The state bits.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, SizeIsReportedWithTheStateBitsMaskedOff) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  PerfettoPbMsgInitWithEncoding(&root, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  EXPECT_EQ(PerfettoPbMsgSize(&root), 0u);
  EXPECT_TRUE(PerfettoPbMsgUsesProtoGroup(&root));
  EXPECT_EQ(root.size & PERFETTO_PB_MSG_FINALIZED_BIT, 0u);

  PerfettoPbMsgAppendType0Field(&root, /*field_id=*/1, 300);
  EXPECT_EQ(PerfettoPbMsgSize(&root), 3u);
  EXPECT_TRUE(PerfettoPbMsgUsesProtoGroup(&root));

  EXPECT_EQ(PerfettoPbMsgFinalize(&root), 3u);
  EXPECT_NE(root.size & PERFETTO_PB_MSG_FINALIZED_BIT, 0u);
  // Finalizing must not have disturbed the encoding or the size.
  EXPECT_TRUE(PerfettoPbMsgUsesProtoGroup(&root));
  EXPECT_EQ(PerfettoPbMsgSize(&root), 3u);
}

// The public field, on the mode every existing caller uses. `size` is
// documented as the current size and callers read it directly, so it has to
// stay a plain byte count on both sides of Finalize().
TEST(PbMsgTest, LengthDelimitedSizeKeepsItsRawMeaning) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  PerfettoPbMsgInit(&root, buffer.writer());
  EXPECT_EQ(root.size, 0u);

  PerfettoPbMsgAppendType0Field(&root, /*field_id=*/1, 300);
  EXPECT_EQ(root.size, 3u);

  EXPECT_EQ(PerfettoPbMsgFinalize(&root), 3u);
  EXPECT_EQ(root.size, 3u);
  EXPECT_EQ(PerfettoPbMsgSize(&root), 3u);
  EXPECT_FALSE(PerfettoPbMsgUsesProtoGroup(&root));
  EXPECT_EQ(root.size & PERFETTO_PB_MSG_FINALIZED_BIT, 0u);

  // And a second finalization is still a no-op.
  EXPECT_EQ(PerfettoPbMsgFinalize(&root), 3u);
  EXPECT_EQ(root.size, 3u);
}

// The same for a nested message, where finalization also patches a length. Both
// the parent's raw size and the nested message's have to be what they were
// before this encoding existed, and the patched bytes have to be identical.
TEST(PbMsgTest, LengthDelimitedNestedSizesAndPatchAreUnchanged) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  PerfettoPbMsgInit(&root, buffer.writer());

  PerfettoPbMsg nested;
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/2);
  PerfettoPbMsgAppendType0Field(&nested, /*field_id=*/1, 7);
  EXPECT_EQ(nested.size, 2u);
  PerfettoPbMsgEndNested(&root);

  // tag + four reserved length bytes + the nested payload.
  EXPECT_EQ(root.size, 1u + 4u + 2u);
  EXPECT_EQ(PerfettoPbMsgFinalize(&root), 7u);
  EXPECT_EQ(root.size, 7u);

  // Field 2, wire type 2; the redundant four-byte length of 2; then the two
  // payload bytes.
  const uint8_t kExpected[] = {0x12, 0x82, 0x80, 0x80, 0x00, 0x08, 0x07};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, LargeSizesDoNotReachTheStateBits) {
  // A size just below the four-byte varint limit must still leave the state
  // bits alone; this is what makes the plain `size += n` in AppendBytes() safe.
  PerfettoPbMsg msg{};
  msg.size = PERFETTO_PB_MSG_PROTO_GROUP_BIT;
  msg.size += static_cast<uint32_t>(protozero::proto_utils::kMaxMessageLength);
  EXPECT_TRUE(PerfettoPbMsgUsesProtoGroup(&msg));
  EXPECT_EQ(msg.size & PERFETTO_PB_MSG_FINALIZED_BIT, 0u);
  EXPECT_EQ(PerfettoPbMsgSize(&msg),
            static_cast<uint32_t>(protozero::proto_utils::kMaxMessageLength));
}

// ---------------------------------------------------------------------------
// C and C++ agree, byte for byte.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, CAndCppEmitIdenticalBytes) {
  EXPECT_EQ(BuildWithC(PERFETTO_PB_MSG_ENCODING_PROTO_GROUP),
            BuildWithCpp(protozero::NestedMessageEncoding::kProtoGroup));

  // The exact expected bytes, so a shared misunderstanding cannot make the
  // comparison above pass:
  //   08 07                 field 1 = 7
  //   13                    start field 2
  //   18 08                 field 3 = 8
  //   23                    start field 4
  //   2a 03 61 62 63        field 5 = "abc"
  //   04 04                 close fields 4 and 2
  //   30 09                 field 6 = 9
  const uint8_t kExpected[] = {0x08, 0x07, 0x13, 0x18, 0x08, 0x23, 0x2a, 0x03,
                               0x61, 0x62, 0x63, 0x04, 0x04, 0x30, 0x09};
  EXPECT_THAT(BuildWithC(PERFETTO_PB_MSG_ENCODING_PROTO_GROUP),
              ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupBuffersIncrementalString) {
  FlatCBuffer buffer;
  PbMsgTestFields root;
  PerfettoPbMsgInitWithEncoding(&root.msg, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);

  PerfettoPbMsg bytes;
  PbMsgTestFields_begin_bytes(&root, &bytes);
  const uint8_t kFirstPart[] = {0x0b, 0x04};
  const uint8_t kSecondPart[] = {'a', 'b', 'c'};
  PerfettoPbMsgAppendBytes(&bytes, kFirstPart, sizeof(kFirstPart));
  PerfettoPbMsgAppendBytes(&bytes, kSecondPart, sizeof(kSecondPart));
  PbMsgTestFields_end_bytes(&root, &bytes);
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x0a, 0x05, 0x0b, 0x04, 'a', 'b', 'c'};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupBuffersEmptyIncrementalString) {
  FlatCBuffer buffer;
  PbMsgTestFields root;
  PerfettoPbMsgInitWithEncoding(&root.msg, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);

  PerfettoPbMsg bytes;
  PbMsgTestFields_begin_bytes(&root, &bytes);
  PbMsgTestFields_end_bytes(&root, &bytes);
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x0a, 0x00};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupWholeValueSettersStayLengthDelimited) {
  FlatCBuffer buffer;
  PbMsgTestFields root;
  PerfettoPbMsgInitWithEncoding(&root.msg, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);

  PbMsgTestFields_set_cstr_bytes(&root, "a");
  const uint8_t kBytes[] = {'b'};
  PbMsgTestFields_set_bytes(&root, kBytes, sizeof(kBytes));
  const uint8_t kPacked[] = {0x01, 0x02};
  PbMsgTestFields_set_values(&root, kPacked, sizeof(kPacked));
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x0a, 0x01, 'a',  0x0a, 0x01,
                               'b',  0x12, 0x02, 0x01, 0x02};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupBuffersIncrementalPackedField) {
  FlatCBuffer buffer;
  PbMsgTestFields root;
  PerfettoPbMsgInitWithEncoding(&root.msg, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);

  PerfettoPbPackedMsgUint32 values;
  PbMsgTestFields_begin_values(&root, &values);
  PerfettoPbPackedMsgUint32Append(&values, 4);
  PerfettoPbPackedMsgUint32Append(&values, 2);
  PbMsgTestFields_end_values(&root, &values);
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x12, 0x02, 0x04, 0x02};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupFinalizesAnOpenIncrementalString) {
  FlatCBuffer buffer;
  PbMsgTestFields root;
  PerfettoPbMsgInitWithEncoding(&root.msg, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);

  PerfettoPbMsg bytes;
  PbMsgTestFields_begin_bytes(&root, &bytes);
  const uint8_t kPayload[] = {'a', 'b', 'c'};
  PerfettoPbMsgAppendBytes(&bytes, kPayload, sizeof(kPayload));
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x0a, 0x03, 'a', 'b', 'c'};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, BufferedFieldFinalizationIsIdempotent) {
  FlatCBuffer buffer;
  PbMsgTestFields root;
  PerfettoPbMsgInitWithEncoding(&root.msg, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);

  PerfettoPbMsg bytes;
  PbMsgTestFields_begin_bytes(&root, &bytes);
  const uint8_t kPayload[] = {'a', 'b', 'c'};
  PerfettoPbMsgAppendBytes(&bytes, kPayload, sizeof(kPayload));
  EXPECT_EQ(PerfettoPbMsgFinalize(&bytes), 3u);
  EXPECT_EQ(PerfettoPbMsgFinalize(&bytes), 3u);
  PbMsgTestFields_end_bytes(&root, &bytes);
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x0a, 0x03, 'a', 'b', 'c'};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, LengthDelimitedIncrementalStringIsUnchanged) {
  FlatCBuffer buffer;
  PbMsgTestFields root;
  PerfettoPbMsgInit(&root.msg, buffer.writer());

  PerfettoPbMsg bytes;
  PbMsgTestFields_begin_bytes(&root, &bytes);
  const uint8_t kPayload[] = {'a', 'b', 'c'};
  PerfettoPbMsgAppendBytes(&bytes, kPayload, sizeof(kPayload));
  PbMsgTestFields_end_bytes(&root, &bytes);
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x0a, 0x83, 0x80, 0x80, 0x00, 'a', 'b', 'c'};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, BufferedIncrementalStringCrossesRanges) {
  ChunkedCBuffer buffer(/*range_size=*/32);
  PbMsgTestFields root;
  PerfettoPbMsgInitWithEncoding(&root.msg, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);

  PerfettoPbMsg bytes;
  PbMsgTestFields_begin_bytes(&root, &bytes);
  const std::vector<uint8_t> payload(5000, 0xab);
  PerfettoPbMsgAppendBytes(&bytes, payload.data(), payload.size());
  PbMsgTestFields_end_bytes(&root, &bytes);
  PerfettoPbMsgFinalize(&root.msg);

  EXPECT_GT(buffer.num_ranges(), 1u);
  const std::vector<uint8_t> encoded = buffer.bytes();
  ASSERT_EQ(encoded.size(), payload.size() + 3u);
  EXPECT_EQ(encoded[0], 0x0a);
  EXPECT_EQ(encoded[1], 0x88);
  EXPECT_EQ(encoded[2], 0x27);
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), encoded.begin() + 3));
}

// ---------------------------------------------------------------------------
// The slow path, where the stream writer rolls to another range.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, ProtoGroupMessagesCrossRanges) {
  // Small ranges force the stream writer through its slow path. Proto-group
  // messages have no size field, so there is nothing to patch on rollover.
  ChunkedCBuffer buffer(/*range_size=*/32);
  PerfettoPbMsg root;
  PerfettoPbMsgInitWithEncoding(&root, buffer.writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  PerfettoPbMsg nested;
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);

  const std::vector<uint8_t> payload(50, 0xab);
  for (int i = 0; i < 8; ++i)
    PerfettoPbMsgAppendType2Field(&nested, 2, payload.data(), payload.size());
  PerfettoPbMsgFinalize(&root);

  // If a future change made the ranges big enough to hold everything, this test
  // would silently stop testing the slow path.
  EXPECT_GT(buffer.num_ranges(), 1u);

  const std::vector<uint8_t> bytes = buffer.bytes();
  ASSERT_EQ(bytes.size(), 1u + 8u * 52u + 1u);
  EXPECT_EQ(bytes.front(), 0x0b);
  EXPECT_EQ(bytes.back(), protozero::proto_utils::kProtoGroupEndByte);
}

TEST(PbMsgTest, LengthDelimitedMessagesStillPatchAcrossRanges) {
  ChunkedCBuffer buffer(/*range_size=*/32);
  PerfettoPbMsg root;
  PerfettoPbMsgInit(&root, buffer.writer());
  PerfettoPbMsg nested;
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);

  const std::vector<uint8_t> payload(50, 0xcd);
  for (int i = 0; i < 8; ++i)
    PerfettoPbMsgAppendType2Field(&nested, 2, payload.data(), payload.size());
  PerfettoPbMsgFinalize(&root);

  EXPECT_GT(buffer.num_ranges(), 1u);
  const std::vector<uint8_t> bytes = buffer.bytes();
  // Field 1 preamble, four-byte length, then the payload fields.
  ASSERT_EQ(bytes.size(), 1u + 4u + 8u * 52u);
  EXPECT_EQ(bytes[0], 0x0a);
  // The length is 8 * 52 = 416, written redundantly: 416 = 0b110100000, so the
  // low seven bits are 0x20 and the next are 0x03.
  EXPECT_EQ(bytes[1], 0xa0);
  EXPECT_EQ(bytes[2], 0x83);
  EXPECT_EQ(bytes[3], 0x80);
  EXPECT_EQ(bytes[4], 0x00);
}

// ---------------------------------------------------------------------------
// An encoding this build has never heard of must fail before any byte is
// written, not quietly become length-delimited.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, UnknownEncodingAborts) {
  FlatCBuffer buffer;
  PerfettoPbMsg root;
  EXPECT_DEATH_IF_SUPPORTED(
      PerfettoPbMsgInitWithEncoding(
          &root, buffer.writer(),
          static_cast<enum PerfettoPbMsgEncoding>(0x7fffffff)),
      "");
}

}  // namespace
