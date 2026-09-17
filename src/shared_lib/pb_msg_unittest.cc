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

#include <memory>
#include <vector>

#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/public/data_source.h"
#include "perfetto/public/pb_macros.h"
#include "src/shared_lib/stream_writer.h"
#include "test/gtest_and_gmock.h"

namespace {

using ::testing::ElementsAreArray;

PERFETTO_PB_MSG(PbMsgTestFields);
PERFETTO_PB_FIELD(PbMsgTestFields, STRING, const char*, bytes, 1);
PERFETTO_PB_FIELD(PbMsgTestFields, PACKED, Uint32, values, 2);

// ---------------------------------------------------------------------------
// Test plumbing.
// ---------------------------------------------------------------------------

// One buffer large enough for each test message. These tests do not request a
// second range. ChunkedCBuffer below exercises the stream writer's slow paths.
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

// Uses protozero::ScatteredStreamWriter to supply small ranges to the C writer.
// Messages cross range boundaries, which makes the C runtime call
// PerfettoPbMsgPatchStack() before it requests another range.
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

  // Returns all written bytes in one contiguous buffer.
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

template <typename Buffer>
void InitProtoGroupRoot(PerfettoPbMsg* root, Buffer* buffer) {
  PerfettoPbMsgInitWithEncoding(root, buffer->writer(),
                                PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
}

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

// Writes the same message through the C runtime. The two APIs close children
// differently:
// - C++ closes an open child when the caller writes the parent's next field.
// - C requires an explicit PerfettoPbMsgEndNested() call.
// The resulting bytes must match despite these different call sequences.
std::vector<uint8_t> BuildWithC(enum PerfettoPbMsgEncoding encoding) {
  FlatCBuffer buffer;
  PerfettoPbMsg root{};
  PerfettoPbMsgInitWithEncoding(&root, buffer.writer(), encoding);
  PerfettoPbMsgAppendType0Field(&root, 1, 7);
  PerfettoPbMsg nested{};
  PerfettoPbMsgBeginNested(&root, &nested, 2);
  PerfettoPbMsgAppendType0Field(&nested, 3, 8);
  PerfettoPbMsg inner{};
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
  PerfettoPbMsg root{};
  PerfettoPbMsgInit(&root, buffer.writer());
  EXPECT_EQ(root.encoding, PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED);

  PerfettoPbMsg nested{};
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);
  EXPECT_EQ(nested.encoding, PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED);
  PerfettoPbMsgAppendType0Field(&nested, /*field_id=*/2, 2);
  PerfettoPbMsgFinalize(&root);

  // Field 1 preamble, the four-byte redundant length of 2, then the field. The
  // C runtime does not compact, so the length stays four bytes wide.
  const uint8_t kExpected[] = {0x0a, 0x82, 0x80, 0x80, 0x00, 0x10, 0x02};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupEmptyNestedMessage) {
  FlatCBuffer buffer;
  PerfettoPbMsg root{};
  InitProtoGroupRoot(&root, &buffer);
  EXPECT_EQ(root.encoding, PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);

  PerfettoPbMsg nested{};
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);
  EXPECT_EQ(nested.encoding, PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  EXPECT_EQ(nested.size_field, nullptr);
  PerfettoPbMsgFinalize(&root);

  const uint8_t kExpected[] = {0x0b, 0x04};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ProtoGroupRootEmitsNoEndByte) {
  FlatCBuffer buffer;
  PerfettoPbMsg root{};
  InitProtoGroupRoot(&root, &buffer);
  PerfettoPbMsgAppendType0Field(&root, /*field_id=*/1, 42);
  PerfettoPbMsgFinalize(&root);

  const uint8_t kExpected[] = {0x08, 0x2a};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, FinalizeIsIdempotentInBothModes) {
  {
    FlatCBuffer buffer;
    PerfettoPbMsg root{};
    InitProtoGroupRoot(&root, &buffer);
    PerfettoPbMsg nested{};
    PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);
    EXPECT_FALSE(nested.proto_group_end_written);
    EXPECT_EQ(PerfettoPbMsgFinalize(&nested), 1u);
    EXPECT_TRUE(nested.proto_group_end_written);
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
    PerfettoPbMsg root{};
    PerfettoPbMsgInit(&root, buffer.writer());
    PerfettoPbMsgAppendType0Field(&root, /*field_id=*/1, 1);
    EXPECT_EQ(PerfettoPbMsgFinalize(&root), 2u);
    EXPECT_EQ(PerfettoPbMsgFinalize(&root), 2u);
    const uint8_t kExpected[] = {0x08, 0x01};
    EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
  }
}

// ---------------------------------------------------------------------------
// Message size and encoding state.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, SizeIsAPlainByteCountInBothModes) {
  for (auto encoding : {PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED,
                        PERFETTO_PB_MSG_ENCODING_PROTO_GROUP}) {
    FlatCBuffer buffer;
    PerfettoPbMsg root{};
    PerfettoPbMsgInitWithEncoding(&root, buffer.writer(), encoding);
    EXPECT_EQ(root.size, 0u);

    PerfettoPbMsgAppendType0Field(&root, /*field_id=*/1, 300);
    EXPECT_EQ(root.size, 3u);
    EXPECT_EQ(PerfettoPbMsgFinalize(&root), 3u);
    EXPECT_EQ(root.size, 3u);
    EXPECT_EQ(PerfettoPbMsgFinalize(&root), 3u);
    EXPECT_EQ(root.size, 3u);
    EXPECT_EQ(root.encoding, encoding);
    EXPECT_FALSE(root.proto_group_end_written);
    const uint8_t kExpected[] = {0x08, 0xac, 0x02};
    EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
  }
}

TEST(PbMsgTest, LengthDelimitedNestedSizesAndPatch) {
  FlatCBuffer buffer;
  PerfettoPbMsg root{};
  PerfettoPbMsgInit(&root, buffer.writer());

  PerfettoPbMsg nested{};
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/2);
  PerfettoPbMsgAppendType0Field(&nested, /*field_id=*/1, 7);
  EXPECT_EQ(nested.size, 2u);
  PerfettoPbMsgEndNested(&root);

  // tag + four reserved length bytes + the nested payload.
  EXPECT_EQ(root.size, 1u + 4u + 2u);
  EXPECT_EQ(PerfettoPbMsgFinalize(&root), 7u);
  EXPECT_EQ(root.size, 7u);

  // Field 2 with wire type 2, followed by the four-byte encoding of length 2
  // and the two payload bytes.
  const uint8_t kExpected[] = {0x12, 0x82, 0x80, 0x80, 0x00, 0x08, 0x07};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, ReusedMessagesResetEncodingAndClosingState) {
  FlatCBuffer buffer;
  PerfettoPbMsg root{};
  PerfettoPbMsg nested{};
  // Reuse both objects across packets, including a switch back to the default
  // encoding. Reusing the child must allow it to write another closing byte.
  for (auto encoding : {PERFETTO_PB_MSG_ENCODING_PROTO_GROUP,
                        PERFETTO_PB_MSG_ENCODING_PROTO_GROUP,
                        PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED}) {
    PerfettoPbMsgInitWithEncoding(&root, buffer.writer(), encoding);
    EXPECT_EQ(root.size, 0u);
    EXPECT_EQ(root.encoding, encoding);
    EXPECT_FALSE(root.proto_group_end_written);
    PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);
    EXPECT_EQ(nested.size, 0u);
    EXPECT_EQ(nested.encoding, encoding);
    EXPECT_FALSE(nested.proto_group_end_written);
    PerfettoPbMsgAppendType0Field(&nested, /*field_id=*/2, 7);
    PerfettoPbMsgFinalize(&root);
    EXPECT_EQ(root.size,
              encoding == PERFETTO_PB_MSG_ENCODING_PROTO_GROUP ? 4u : 7u);
    EXPECT_EQ(nested.size,
              encoding == PERFETTO_PB_MSG_ENCODING_PROTO_GROUP ? 3u : 2u);
    EXPECT_FALSE(root.proto_group_end_written);
    EXPECT_EQ(nested.proto_group_end_written,
              encoding == PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
  }
  const uint8_t kExpected[] = {0x0b, 0x10, 0x07, 0x04, 0x0b, 0x10, 0x07, 0x04,
                               0x0a, 0x82, 0x80, 0x80, 0x00, 0x10, 0x07};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

// ---------------------------------------------------------------------------
// C and C++ agree, byte for byte.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, CAndCppEmitIdenticalBytes) {
  EXPECT_EQ(BuildWithC(PERFETTO_PB_MSG_ENCODING_PROTO_GROUP),
            BuildWithCpp(protozero::NestedMessageEncoding::kProtoGroup));

  // Check explicit bytes as well, since both runtimes can have the same bug:
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

// ---------------------------------------------------------------------------
// STRING and PACKED fields contain arbitrary bytes, so group tags cannot frame
// them. They require a length before their payload.
// Complete setters know that length and emit a length-delimited field in either
// encoding. Incremental builders know the length only when the field closes,
// so the append-only proto group encoding rejects them.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, ProtoGroupWholeValueSettersStayLengthDelimited) {
  FlatCBuffer buffer;
  PbMsgTestFields root{};
  InitProtoGroupRoot(&root.msg, &buffer);

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

// Incremental builders reserve a length field and fill it at the end.
// Append-only writes cannot update that length. Reject the begin call before
// a tag or payload byte reaches the packet.
TEST(PbMsgTest, ProtoGroupRejectsIncrementalString) {
  FlatCBuffer buffer;
  PbMsgTestFields root{};
  InitProtoGroupRoot(&root.msg, &buffer);

  PerfettoPbMsg bytes{};
  EXPECT_DEATH_IF_SUPPORTED(PbMsgTestFields_begin_bytes(&root, &bytes), "");
}

TEST(PbMsgTest, ProtoGroupRejectsIncrementalPackedField) {
  FlatCBuffer buffer;
  PbMsgTestFields root{};
  InitProtoGroupRoot(&root.msg, &buffer);

  PerfettoPbPackedMsgUint32 values{};
  EXPECT_DEATH_IF_SUPPORTED(PbMsgTestFields_begin_values(&root, &values), "");
}

TEST(PbMsgTest, LengthDelimitedIncrementalStringIsUnchanged) {
  FlatCBuffer buffer;
  PbMsgTestFields root{};
  PerfettoPbMsgInit(&root.msg, buffer.writer());

  PerfettoPbMsg bytes{};
  PbMsgTestFields_begin_bytes(&root, &bytes);
  const uint8_t kPayload[] = {'a', 'b', 'c'};
  PerfettoPbMsgAppendBytes(&bytes, kPayload, sizeof(kPayload));
  PbMsgTestFields_end_bytes(&root, &bytes);
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x0a, 0x83, 0x80, 0x80, 0x00, 'a', 'b', 'c'};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

TEST(PbMsgTest, LengthDelimitedIncrementalPackedFieldIsUnchanged) {
  FlatCBuffer buffer;
  PbMsgTestFields root{};
  PerfettoPbMsgInit(&root.msg, buffer.writer());

  PerfettoPbPackedMsgUint32 values{};
  PbMsgTestFields_begin_values(&root, &values);
  PerfettoPbPackedMsgUint32Append(&values, 4);
  PerfettoPbPackedMsgUint32Append(&values, 2);
  PbMsgTestFields_end_values(&root, &values);
  PerfettoPbMsgFinalize(&root.msg);

  const uint8_t kExpected[] = {0x12, 0x82, 0x80, 0x80, 0x00, 0x04, 0x02};
  EXPECT_THAT(buffer.bytes(), ElementsAreArray(kExpected));
}

// ---------------------------------------------------------------------------
// The slow path, where the stream writer rolls to another range.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, ProtoGroupMessagesCrossRanges) {
  // Small ranges force the stream writer through its slow path. Messages in
  // proto group mode have no size field, so range changes require no patches.
  ChunkedCBuffer buffer(/*range_size=*/32);
  PerfettoPbMsg root{};
  InitProtoGroupRoot(&root, &buffer);
  PerfettoPbMsg nested{};
  PerfettoPbMsgBeginNested(&root, &nested, /*field_id=*/1);

  const std::vector<uint8_t> payload(50, 0xab);
  for (int i = 0; i < 8; ++i)
    PerfettoPbMsgAppendType2Field(&nested, 2, payload.data(), payload.size());
  PerfettoPbMsgFinalize(&root);

  // Require a range change so the test continues to exercise the slow path
  // even if the fixture's buffer sizes change.
  EXPECT_GT(buffer.num_ranges(), 1u);

  const std::vector<uint8_t> bytes = buffer.bytes();
  ASSERT_EQ(bytes.size(), 1u + 8u * 52u + 1u);
  EXPECT_EQ(bytes.front(), 0x0b);
  EXPECT_EQ(bytes.back(), protozero::proto_utils::kProtoGroupEndByte);
}

TEST(PbMsgTest, LengthDelimitedMessagesStillPatchAcrossRanges) {
  ChunkedCBuffer buffer(/*range_size=*/32);
  PerfettoPbMsg root{};
  PerfettoPbMsgInit(&root, buffer.writer());
  PerfettoPbMsg nested{};
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
// Reject an unknown encoding before any byte reaches the stream.
// ---------------------------------------------------------------------------

TEST(PbMsgTest, UnknownEncodingAborts) {
  FlatCBuffer buffer;
  PerfettoPbMsg root{};
  EXPECT_DEATH_IF_SUPPORTED(
      PerfettoPbMsgInitWithEncoding(
          &root, buffer.writer(),
          static_cast<enum PerfettoPbMsgEncoding>(0x7fffffff)),
      "");
}

TEST(PbMsgTest, PacketEncodingsTranslateExplicitly) {
  EXPECT_EQ(PerfettoDsPacketEncodingToPbMsg(
                PERFETTO_DS_PACKET_ENCODING_LENGTH_DELIMITED),
            PERFETTO_PB_MSG_ENCODING_LENGTH_DELIMITED);
  EXPECT_EQ(
      PerfettoDsPacketEncodingToPbMsg(PERFETTO_DS_PACKET_ENCODING_PROTO_GROUP),
      PERFETTO_PB_MSG_ENCODING_PROTO_GROUP);
}

TEST(PbMsgTest, UnknownPacketEncodingAborts) {
  EXPECT_DEATH_IF_SUPPORTED(PerfettoDsPacketEncodingToPbMsg(UINT32_MAX), "");
}

}  // namespace
