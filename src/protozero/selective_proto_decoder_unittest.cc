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

#include "perfetto/protozero/selective_proto_decoder.h"

#include <string>
#include <vector>

#include "perfetto/protozero/message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "test/gtest_and_gmock.h"

namespace protozero {
namespace {

using StoreUnknownDecoder =
    SelectiveProtoDecoder</*kStoreUnknownFields=*/true, 1, 2, 5>;
using DropUnknownDecoder =
    SelectiveProtoDecoder</*kStoreUnknownFields=*/false, 1, 2, 5>;

TEST(SelectiveProtoDecoderTest, ExplicitFields) {
  HeapBuffered<Message> message;
  message->AppendVarInt(1, 10);
  message->AppendString(2, "payload");
  message->AppendFixed<uint64_t>(5, 0xf00df00df00df00d);
  std::vector<uint8_t> data = message.SerializeAsArray();

  StoreUnknownDecoder decoder(data.data(), data.size());
  EXPECT_TRUE(decoder.HasField(1));
  EXPECT_EQ(decoder.at<1>().as_int32(), 10);
  EXPECT_EQ(decoder.at<2>().as_std_string(), "payload");
  EXPECT_EQ(decoder.at<5>().as_uint64(), 0xf00df00df00df00d);
  EXPECT_EQ(decoder.Get(1).as_int32(), 10);
  EXPECT_EQ(decoder.unknown_fields().size(), 0u);
}

TEST(SelectiveProtoDecoderTest, UnseenExplicitFieldIsInvalid) {
  HeapBuffered<Message> message;
  message->AppendVarInt(1, 10);
  std::vector<uint8_t> data = message.SerializeAsArray();

  StoreUnknownDecoder decoder(data.data(), data.size());
  EXPECT_FALSE(decoder.HasField(2));
  EXPECT_FALSE(decoder.at<2>().valid());
  EXPECT_FALSE(decoder.Get(2).valid());
  EXPECT_FALSE(decoder.Get(5).valid());
}

TEST(SelectiveProtoDecoderTest, ExplicitFieldLastOccurrenceWins) {
  HeapBuffered<Message> message;
  message->AppendVarInt(1, 10);
  message->AppendVarInt(1, 11);
  message->AppendVarInt(1, 12);
  std::vector<uint8_t> data = message.SerializeAsArray();

  StoreUnknownDecoder decoder(data.data(), data.size());
  EXPECT_EQ(decoder.at<1>().as_int32(), 12);
  // Repeated occurrences of explicit fields are not unknown fields.
  EXPECT_EQ(decoder.unknown_fields().size(), 0u);
}

TEST(SelectiveProtoDecoderTest, UnknownFieldsCollectedInWireOrder) {
  HeapBuffered<Message> message;
  message->AppendVarInt(3, 30);               // Unknown, low id.
  message->AppendVarInt(1, 10);               // Explicit.
  message->AppendString(1000, "extension");   // Unknown, high id.
  message->AppendVarInt(3, 31);               // Unknown, repeated.
  message->AppendFixed<uint32_t>(4, 0xabcd);  // Unknown, fixed32.
  std::vector<uint8_t> data = message.SerializeAsArray();

  StoreUnknownDecoder decoder(data.data(), data.size());
  EXPECT_EQ(decoder.at<1>().as_int32(), 10);

  std::vector<std::pair<uint32_t, std::string>> seen;
  for (const Field& f : decoder.unknown_fields()) {
    if (f.type() == proto_utils::ProtoWireType::kLengthDelimited) {
      seen.emplace_back(f.id(), f.as_std_string());
    } else {
      seen.emplace_back(f.id(), std::to_string(f.as_uint64()));
    }
  }
  ASSERT_EQ(seen.size(), 4u);
  EXPECT_EQ(seen[0], std::make_pair(3u, std::string("30")));
  EXPECT_EQ(seen[1], std::make_pair(1000u, std::string("extension")));
  EXPECT_EQ(seen[2], std::make_pair(3u, std::string("31")));
  EXPECT_EQ(seen[3], std::make_pair(4u, std::string("43981")));
}

TEST(SelectiveProtoDecoderTest, DropUnknownFields) {
  HeapBuffered<Message> message;
  message->AppendVarInt(3, 30);
  message->AppendVarInt(1, 10);
  message->AppendString(1000, "extension");
  std::vector<uint8_t> data = message.SerializeAsArray();

  DropUnknownDecoder decoder(data.data(), data.size());
  EXPECT_EQ(decoder.at<1>().as_int32(), 10);
  EXPECT_FALSE(decoder.Get(3).valid());
  EXPECT_EQ(decoder.read_offset(), data.size());
}

TEST(SelectiveProtoDecoderTest, UnknownStorageSpillsToHeap) {
  HeapBuffered<Message> message;
  constexpr int kNumUnknown = 100;  // Way past the inline capacity.
  for (int i = 0; i < kNumUnknown; i++)
    message->AppendVarInt(7, i);
  message->AppendVarInt(1, 10);
  std::vector<uint8_t> data = message.SerializeAsArray();

  StoreUnknownDecoder decoder(data.data(), data.size());
  EXPECT_EQ(decoder.at<1>().as_int32(), 10);
  ASSERT_EQ(decoder.unknown_fields().size(), static_cast<size_t>(kNumUnknown));
  int expected = 0;
  for (const Field& f : decoder.unknown_fields()) {
    EXPECT_EQ(f.id(), 7u);
    EXPECT_EQ(f.as_int32(), expected++);
  }
}

TEST(SelectiveProtoDecoderTest, AllWireTypesAsUnknown) {
  HeapBuffered<Message> message;
  message->AppendVarInt(10, 42);
  message->AppendFixed<uint32_t>(11, 0x11223344);
  message->AppendFixed<uint64_t>(12, 0x5566778899aabbcc);
  message->AppendString(13, "str");
  std::vector<uint8_t> data = message.SerializeAsArray();

  StoreUnknownDecoder decoder(data.data(), data.size());
  auto range = decoder.unknown_fields();
  ASSERT_EQ(range.size(), 4u);
  const Field* it = range.begin();
  EXPECT_EQ(it->id(), 10u);
  EXPECT_EQ(it->as_uint64(), 42u);
  ++it;
  EXPECT_EQ(it->id(), 11u);
  EXPECT_EQ(it->as_uint32(), 0x11223344u);
  ++it;
  EXPECT_EQ(it->id(), 12u);
  EXPECT_EQ(it->as_uint64(), 0x5566778899aabbccu);
  ++it;
  EXPECT_EQ(it->id(), 13u);
  EXPECT_EQ(it->as_std_string(), "str");
}

TEST(SelectiveProtoDecoderTest, TruncatedBufferStopsCleanly) {
  HeapBuffered<Message> message;
  message->AppendVarInt(1, 10);
  message->AppendString(2, "payload");
  std::vector<uint8_t> data = message.SerializeAsArray();

  // Truncate in the middle of the string payload: field 1 parses, field 2
  // aborts and the read offset stays at field 2's start.
  StoreUnknownDecoder decoder(data.data(), data.size() - 3);
  EXPECT_EQ(decoder.at<1>().as_int32(), 10);
  EXPECT_FALSE(decoder.at<2>().valid());
  EXPECT_EQ(decoder.read_offset(), 2u);  // Field 1 is tag + 1-byte varint.
}

TEST(SelectiveProtoDecoderTest, ZeroFieldIdStopsParsing) {
  uint8_t data[] = {0x08, 0x2a, 0x00, 0x08, 0x2b};  // field 1 = 42, tag 0, ...
  StoreUnknownDecoder decoder(data, sizeof(data));
  EXPECT_EQ(decoder.at<1>().as_int32(), 42);
  EXPECT_EQ(decoder.read_offset(), 2u);
}

TEST(SelectiveProtoDecoderTest, HugeFieldIdSkipped) {
  HeapBuffered<Message> message;
  // Field id beyond Field::kMaxId (24 bits): cannot be represented in a
  // Field, must be skipped without corrupting the unknown array.
  message->AppendVarInt(Field::kMaxId + 1, 99);
  message->AppendVarInt(1, 10);
  std::vector<uint8_t> data = message.SerializeAsArray();

  StoreUnknownDecoder decoder(data.data(), data.size());
  EXPECT_EQ(decoder.at<1>().as_int32(), 10);
  EXPECT_EQ(decoder.unknown_fields().size(), 0u);
}

TEST(SelectiveProtoDecoderTest, EmptyBuffer) {
  StoreUnknownDecoder decoder(nullptr, 0);
  EXPECT_FALSE(decoder.at<1>().valid());
  EXPECT_EQ(decoder.unknown_fields().size(), 0u);
}

TEST(SelectiveProtoDecoderTest, EmptyExplicitSetCollectsAllFields) {
  HeapBuffered<Message> message;
  message->AppendVarInt(1, 10);
  message->AppendString(1000, "extension");
  std::vector<uint8_t> data = message.SerializeAsArray();

  SelectiveProtoDecoder</*kStoreUnknownFields=*/true> decoder(data.data(),
                                                              data.size());
  auto range = decoder.unknown_fields();
  ASSERT_EQ(range.size(), 2u);
  EXPECT_EQ(range.begin()->id(), 1u);
  EXPECT_EQ((range.begin() + 1)->id(), 1000u);
}

TEST(SelectiveProtoDecoderTest, FindUnknownField) {
  HeapBuffered<Message> message;
  message->AppendVarInt(1000, 1);
  message->AppendString(1001, "first");
  message->AppendString(1001, "second");
  std::vector<uint8_t> data = message.SerializeAsArray();

  SelectiveProtoDecoder</*kStoreUnknownFields=*/true> decoder(data.data(),
                                                              data.size());
  // First occurrence wins (FindField semantics).
  EXPECT_EQ(decoder.FindUnknownField(1001).as_std_string(), "first");
  EXPECT_EQ(decoder.FindUnknownField(1000).as_int32(), 1);
  EXPECT_FALSE(decoder.FindUnknownField(1002).valid());
}

}  // namespace
}  // namespace protozero
