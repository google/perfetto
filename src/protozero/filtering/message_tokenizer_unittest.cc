/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "test/gtest_and_gmock.h"

#include "perfetto/protozero/message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protozero/filtering/message_tokenizer.h"

namespace protozero {

using proto_utils::ProtoWireType;
using ::testing::ElementsAre;
using Token = MessageTokenizer::Token;

// For ASSERT_THAT(ElementsAre(...))
inline std::ostream& operator<<(std::ostream& stream, const Token& t) {
  stream << "{" << t.field_id << ", ";
  switch (t.type) {
    case ProtoWireType::kVarInt:
      stream << "varint, ";
      break;
    case ProtoWireType::kFixed32:
      stream << "fixed32, ";
      break;
    case ProtoWireType::kFixed64:
      stream << "fixed64, ";
      break;
    case ProtoWireType::kLengthDelimited:
      stream << "lendelim, ";
      break;
    default:
      stream << "???, ";
      break;
  }
  stream << t.value << "}";
  return stream;
}

namespace {

TEST(MessageTokenizerTest, FlatMessage) {
  HeapBuffered<Message> msg;
  msg->AppendVarInt(/*field_id*/ 1, 42u);
  msg->AppendVarInt(/*field_id*/ 1, 1000u);
  msg->AppendVarInt(/*field_id*/ 2, 1000000000ull);
  msg->AppendVarInt(/*field_id*/ 3, 0xFF001234DEADBEEFull);
  msg->AppendString(/*field_id*/ 4, "foo");
  msg->AppendFixed(/*field_id*/ 5, 0xFFAAFFFFu);
  msg->AppendString(/*field_id*/ 4, "foobar");
  msg->AppendFixed(/*field_id*/ 6, uint64_t(1ull << 63));
  msg->AppendVarInt(/*field_id*/ 1000, 1001ull);
  msg->AppendVarInt(/*field_id*/ 1000000, 1000001ull);
  msg->AppendVarInt(/*field_id*/ 1 << 28, uint64_t(1ull << 63));

  // Treat all len-delimited fields as strings/bytes and just eat their payload.
  MessageTokenizer tokenizer;
  std::vector<Token> tokens;
  size_t eat_bytes = 0;
  for (uint8_t octet : msg.SerializeAsArray()) {
    if (eat_bytes > 0) {
      --eat_bytes;
      continue;
    }
    auto token = tokenizer.Push(octet);
    if (token.valid())
      tokens.emplace_back(token);
    if (token.type == ProtoWireType::kLengthDelimited) {
      ASSERT_EQ(eat_bytes, 0u);
      eat_bytes = static_cast<size_t>(token.value);
    }
  }
  EXPECT_TRUE(tokenizer.idle());
  EXPECT_THAT(
      tokens,
      ElementsAre(
          Token{1, ProtoWireType::kVarInt, 42u},
          Token{1, ProtoWireType::kVarInt, 1000u},
          Token{2, ProtoWireType::kVarInt, 1000000000ull},
          Token{3, ProtoWireType::kVarInt, 0xFF001234DEADBEEFull},
          Token{4, ProtoWireType::kLengthDelimited, 3},
          Token{5, ProtoWireType::kFixed32, 0xFFAAFFFFu},
          Token{4, ProtoWireType::kLengthDelimited, 6},
          Token{6, ProtoWireType::kFixed64, uint64_t(1ull << 63)},
          Token{1000, ProtoWireType::kVarInt, 1001ull},
          Token{1000000, ProtoWireType::kVarInt, 1000001ull},
          Token{1 << 28, ProtoWireType::kVarInt, uint64_t(1ull << 63)}));
}

TEST(MessageTokenizerTest, NestedMessage) {
  HeapBuffered<Message> msg;
  msg->AppendVarInt(/*field_id*/ 1, 101u);
  {
    auto* nested = msg->BeginNestedMessage<Message>(2);
    nested->AppendVarInt(/*field_id*/ 3, 103u);
    nested->AppendFixed(/*field_id*/ 4, 104u);
    {
      auto* nested2 = nested->BeginNestedMessage<Message>(5);
      nested2->AppendVarInt(/*field_id*/ 6, 106u);
      nested2->AppendFixed(/*field_id*/ 7, 107u);
      nested2->Finalize();
    }
    nested->AppendFixed(/*field_id*/ 8, 0x42420000u);
    nested->Finalize();
  }
  msg->AppendFixed(/*field_id*/ 9, uint64_t(1ull << 63));

  // Tokenize the message. This treat all len delimited fields as submessage
  // and test the recursion logic.
  MessageTokenizer tokenizer;
  std::vector<Token> tokens;
  for (uint8_t octet : msg.SerializeAsArray()) {
    auto token = tokenizer.Push(octet);
    if (token.valid())
      tokens.emplace_back(token);
  }
  EXPECT_TRUE(tokenizer.idle());
  EXPECT_THAT(
      tokens,
      ElementsAre(Token{1, ProtoWireType::kVarInt, 101u},
                  Token{2, ProtoWireType::kLengthDelimited, 21u},
                  Token{3, ProtoWireType::kVarInt, 103u},
                  Token{4, ProtoWireType::kFixed32, 104u},
                  Token{5, ProtoWireType::kLengthDelimited, 7},
                  Token{6, ProtoWireType::kVarInt, 106u},
                  Token{7, ProtoWireType::kFixed32, 107u},
                  Token{8, ProtoWireType::kFixed32, 0x42420000u},
                  Token{9, ProtoWireType::kFixed64, uint64_t(1ull << 63)}));
}

TEST(MessageTokenizerTest, InvlidCases) {
  {
    // A very large varint.
    MessageTokenizer tokenizer;
    EXPECT_FALSE(tokenizer.Push(0x08).valid());
    for (int i = 0; i < 14; ++i)
      EXPECT_FALSE(tokenizer.Push(0xff).valid());
    EXPECT_FALSE(tokenizer.Push(0x0).valid());
    EXPECT_FALSE(tokenizer.idle());
    EXPECT_EQ(tokenizer.state(), 6u);
  }
  {
    // A very large string.
    MessageTokenizer tokenizer;
    EXPECT_FALSE(tokenizer.Push(0x0A).valid());
    EXPECT_FALSE(tokenizer.Push(0xFF).valid());
    EXPECT_FALSE(tokenizer.Push(0xFF).valid());
    EXPECT_FALSE(tokenizer.Push(0xFF).valid());
    EXPECT_FALSE(tokenizer.Push(0xFF).valid());
    EXPECT_FALSE(tokenizer.Push(0x20).valid());
    EXPECT_FALSE(tokenizer.idle());
    EXPECT_EQ(tokenizer.state(), 5u);
  }
  {
    // A field of unknown type (wire type = 0x3).
    MessageTokenizer tokenizer;
    EXPECT_FALSE(tokenizer.Push(0x0B).valid());
    EXPECT_FALSE(tokenizer.Push(0).valid());
    EXPECT_FALSE(tokenizer.Push(0).valid());
    EXPECT_FALSE(tokenizer.idle());
    EXPECT_EQ(tokenizer.state(), 4u);
  }
}

}  // namespace
}  // namespace protozero
