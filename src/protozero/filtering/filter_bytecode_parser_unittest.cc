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

#include "perfetto/ext/base/hash.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protozero/filtering/filter_bytecode_common.h"
#include "src/protozero/filtering/filter_bytecode_parser.h"

namespace protozero {

namespace {

bool LoadBytecode(FilterBytecodeParser* parser,
                  std::initializer_list<uint32_t> bytecode) {
  perfetto::base::Hasher hasher;
  protozero::PackedVarInt words;
  for (uint32_t w : bytecode) {
    words.Append(w);
    hasher.Update(w);
  }
  words.Append(static_cast<uint32_t>(hasher.digest()));
  return parser->Load(words.data(), words.size());
}

TEST(FilterBytecodeParserTest, ParserSimpleFields) {
  FilterBytecodeParser parser;
  EXPECT_FALSE(parser.Load(nullptr, 0));
  EXPECT_FALSE(parser.Query(42, 42).allowed);

  EXPECT_TRUE(LoadBytecode(&parser, {}));
  EXPECT_FALSE(parser.Query(0, 0).allowed);
  EXPECT_FALSE(parser.Query(0, 0xffffffff).allowed);
  EXPECT_FALSE(parser.Query(1, 0).allowed);
  EXPECT_FALSE(parser.Query(0, 1).allowed);
  EXPECT_FALSE(parser.Query(1, 1).allowed);
  EXPECT_FALSE(parser.Query(42, 42).allowed);

  // An invalid field_id (0) in bytecode should cause a parse failure.
  EXPECT_FALSE(LoadBytecode(
      &parser, {kFilterOpcode_SimpleField | 0, kFilterOpcode_EndOfMessage}));

  // A valid bytecode that has only one field.
  EXPECT_TRUE(LoadBytecode(&parser, {kFilterOpcode_SimpleField | (2u << 3),
                                     kFilterOpcode_EndOfMessage}));
  EXPECT_FALSE(parser.Query(0, 0).allowed);
  EXPECT_FALSE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_TRUE(parser.Query(0, 2).simple_field());
  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_FALSE(parser.Query(0, 3).allowed);
  EXPECT_FALSE(parser.Query(1, 1).allowed);
  EXPECT_FALSE(parser.Query(1, 2).allowed);
  EXPECT_FALSE(parser.Query(1, 3).allowed);

  // A valid bytecode that has few sparse fields < 128.
  EXPECT_TRUE(LoadBytecode(&parser, {kFilterOpcode_SimpleField | (1u << 3),
                                     kFilterOpcode_SimpleField | (7u << 3),
                                     kFilterOpcode_SimpleField | (8u << 3),
                                     kFilterOpcode_SimpleField | (127u << 3),
                                     kFilterOpcode_EndOfMessage}));
  EXPECT_FALSE(parser.Query(0, 0).allowed);
  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_FALSE(parser.Query(0, 2).allowed);
  EXPECT_FALSE(parser.Query(0, 3).allowed);
  EXPECT_FALSE(parser.Query(0, 6).allowed);
  EXPECT_TRUE(parser.Query(0, 7).allowed);
  EXPECT_TRUE(parser.Query(0, 8).allowed);
  EXPECT_FALSE(parser.Query(0, 9).allowed);
  EXPECT_FALSE(parser.Query(0, 126).allowed);
  EXPECT_TRUE(parser.Query(0, 127).allowed);
  EXPECT_FALSE(parser.Query(0, 128).allowed);

  // A valid bytecode that has only fields > 128.
  EXPECT_TRUE(LoadBytecode(&parser, {kFilterOpcode_SimpleField | (1000u << 3),
                                     kFilterOpcode_SimpleField | (1001u << 3),
                                     kFilterOpcode_SimpleField | (2000u << 3),
                                     kFilterOpcode_EndOfMessage}));
  for (uint32_t i = 0; i < 1000; ++i)
    EXPECT_FALSE(parser.Query(0, i).allowed);
  EXPECT_TRUE(parser.Query(0, 1000).allowed);
  EXPECT_TRUE(parser.Query(0, 1001).allowed);
  EXPECT_FALSE(parser.Query(0, 1002).allowed);
  EXPECT_FALSE(parser.Query(0, 1999).allowed);
  EXPECT_TRUE(parser.Query(0, 2000).allowed);
  EXPECT_FALSE(parser.Query(0, 2001).allowed);
}

TEST(FilterBytecodeParserTest, ParserSimpleRanges) {
  FilterBytecodeParser parser;

  // Invalid, range length missing.
  EXPECT_FALSE(
      LoadBytecode(&parser, {
                                kFilterOpcode_SimpleFieldRange | (2u << 3),
                            }));

  // Borderline valid: range length = 0.
  EXPECT_TRUE(
      LoadBytecode(&parser, {kFilterOpcode_SimpleFieldRange | (2u << 3), 0u,
                             kFilterOpcode_SimpleFieldRange | (127u << 3), 0u,
                             kFilterOpcode_SimpleFieldRange | (128u << 3), 0u,
                             kFilterOpcode_SimpleFieldRange | (128u << 3), 0u,
                             kFilterOpcode_EndOfMessage}));
  for (uint32_t i = 0; i < 130; ++i)
    EXPECT_FALSE(parser.Query(0, i).allowed) << i;

  // A valid bytecode with two ranges [2,2], [10, 14].
  EXPECT_TRUE(
      LoadBytecode(&parser, {kFilterOpcode_SimpleFieldRange | (2u << 3),
                             1u,  // length of the range,
                             kFilterOpcode_SimpleFieldRange | (10u << 3),
                             5u,  // length of the range,
                             kFilterOpcode_EndOfMessage}));
  EXPECT_FALSE(parser.Query(0, 0).allowed);
  EXPECT_FALSE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_TRUE(parser.Query(0, 2).simple_field());
  EXPECT_FALSE(parser.Query(0, 3).allowed);
  EXPECT_FALSE(parser.Query(0, 9).allowed);
  for (uint32_t i = 10; i <= 14; ++i)
    EXPECT_TRUE(parser.Query(0, i).allowed);
  EXPECT_FALSE(parser.Query(0, 15).allowed);
}

TEST(FilterBytecodeParserTest, ParserSimpleFieldsAndRanges) {
  FilterBytecodeParser parser;

  // Borderline valid: range length = 0.
  EXPECT_TRUE(
      LoadBytecode(&parser, {kFilterOpcode_SimpleFieldRange | (1u << 3),
                             2u,  // [1,2]

                             kFilterOpcode_SimpleField | (4u << 3),

                             kFilterOpcode_SimpleFieldRange | (126u << 3),
                             4u,  // [126, 129]

                             kFilterOpcode_SimpleField | (150u << 3),

                             kFilterOpcode_EndOfMessage}));
  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_FALSE(parser.Query(0, 3).allowed);
  EXPECT_TRUE(parser.Query(0, 4).allowed);
  EXPECT_FALSE(parser.Query(0, 5).allowed);
  EXPECT_FALSE(parser.Query(0, 125).allowed);
  for (uint32_t i = 126; i <= 129; ++i)
    EXPECT_TRUE(parser.Query(0, i).allowed) << i;
  EXPECT_FALSE(parser.Query(0, 130).allowed);
  EXPECT_TRUE(parser.Query(0, 150).allowed);
}

TEST(FilterBytecodeParserTest, ParserNestedMessages) {
  FilterBytecodeParser parser;

  // Invalid because there are 1 messages in total, and message index 1 is
  // out of range.
  EXPECT_FALSE(LoadBytecode(&parser, {kFilterOpcode_NestedField | (4u << 3),
                                      1u,  // message index
                                      kFilterOpcode_EndOfMessage}));

  // A valid bytecode consisting of 4 message, with recursive / cylical
  // dependencies between them.
  EXPECT_TRUE(LoadBytecode(
      &parser, {
                   // Message 0 (root).
                   kFilterOpcode_SimpleFieldRange | (1u << 3),
                   2u,  // [1,2]
                   kFilterOpcode_NestedField | (4u << 3),
                   3u,  // message index
                   kFilterOpcode_SimpleField | (10u << 3),
                   kFilterOpcode_NestedField | (127u << 3),
                   1u,  // message index
                   kFilterOpcode_NestedField | (128u << 3),
                   2u,  // message index
                   kFilterOpcode_EndOfMessage,

                   // Message 1.
                   kFilterOpcode_NestedField | (2u << 3),
                   1u,  // message index (recurse onto itself),
                   kFilterOpcode_SimpleField | (11u << 3),
                   kFilterOpcode_EndOfMessage,

                   // Message 2.
                   kFilterOpcode_NestedField | (2u << 3),
                   3u,  // message index.
                   kFilterOpcode_EndOfMessage,

                   // Message 3.
                   kFilterOpcode_NestedField | (2u << 3),
                   2u,  // message index (create a cycle, 2->3, 3->2).
                   kFilterOpcode_EndOfMessage,
               }));

  // Query message 0 fields.
  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_TRUE(parser.Query(0, 2).simple_field());
  EXPECT_TRUE(parser.Query(0, 4).allowed);
  EXPECT_FALSE(parser.Query(0, 4).simple_field());
  EXPECT_EQ(parser.Query(0, 4).nested_msg_index, 3u);
  EXPECT_TRUE(parser.Query(0, 10).allowed);
  EXPECT_TRUE(parser.Query(0, 10).simple_field());
  EXPECT_TRUE(parser.Query(0, 127).allowed);
  EXPECT_EQ(parser.Query(0, 127).nested_msg_index, 1u);
  EXPECT_TRUE(parser.Query(0, 128).allowed);
  EXPECT_EQ(parser.Query(0, 128).nested_msg_index, 2u);
  EXPECT_FALSE(parser.Query(0, 129).allowed);

  // Query message 1 fields.
  EXPECT_FALSE(parser.Query(1, 1).allowed);
  EXPECT_TRUE(parser.Query(1, 2).allowed);
  EXPECT_EQ(parser.Query(1, 2).nested_msg_index, 1u);
  EXPECT_FALSE(parser.Query(1, 3).allowed);
  EXPECT_TRUE(parser.Query(1, 11).allowed);
  EXPECT_TRUE(parser.Query(1, 11).simple_field());

  // Query message 2 fields.
  EXPECT_FALSE(parser.Query(2, 0).allowed);
  EXPECT_FALSE(parser.Query(2, 1).allowed);
  EXPECT_TRUE(parser.Query(2, 2).allowed);
  EXPECT_EQ(parser.Query(2, 2).nested_msg_index, 3u);
  EXPECT_FALSE(parser.Query(2, 4).allowed);

  // Query message 3 fields.
  EXPECT_FALSE(parser.Query(3, 0).allowed);
  EXPECT_FALSE(parser.Query(3, 1).allowed);
  EXPECT_TRUE(parser.Query(3, 2).allowed);
  EXPECT_EQ(parser.Query(3, 2).nested_msg_index, 2u);
  EXPECT_FALSE(parser.Query(3, 4).allowed);
}

}  // namespace
}  // namespace protozero
