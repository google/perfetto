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

#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protozero/filtering/filter_bytecode_parser.h"

#include "protos/perfetto/config/proto_filter.pbzero.h"

namespace protozero {

namespace {

using PF = perfetto::protos::pbzero::ProtoFilter;

bool LoadBytecode(FilterBytecodeParser* parser,
                  std::initializer_list<uint32_t> bytecode) {
  protozero::PackedVarInt words;
  for (uint32_t w : bytecode)
    words.Append(w);

  HeapBuffered<PF> filter;
  filter->set_bytecode(words);
  auto filter_msg = filter.SerializeAsArray();
  return parser->Load(filter_msg.data(), filter_msg.size());
}

TEST(FilterBytecodeParserTest, ParserSimpleFields) {
  FilterBytecodeParser parser;
  EXPECT_TRUE(parser.Load(nullptr, 0));
  EXPECT_FALSE(parser.Query(42, 42).allowed);

  EXPECT_TRUE(LoadBytecode(&parser, {}));
  EXPECT_FALSE(parser.Query(0, 0).allowed);
  EXPECT_FALSE(parser.Query(0, 0xffffffff).allowed);
  EXPECT_FALSE(parser.Query(1, 0).allowed);
  EXPECT_FALSE(parser.Query(0, 1).allowed);
  EXPECT_FALSE(parser.Query(1, 1).allowed);
  EXPECT_FALSE(parser.Query(42, 42).allowed);

  // An invalid field_id (0) in bytecode should cause a parse failure.
  EXPECT_FALSE(LoadBytecode(&parser, {PF::FILTER_OPCODE_SIMPLE_FIELD | 0,
                                      PF::FILTER_OPCODE_END_OF_MESSAGE}));

  // A valid bytecode that has only one field.
  EXPECT_TRUE(LoadBytecode(&parser, {PF::FILTER_OPCODE_SIMPLE_FIELD | (2u << 3),
                                     PF::FILTER_OPCODE_END_OF_MESSAGE}));
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
  EXPECT_TRUE(
      LoadBytecode(&parser, {PF::FILTER_OPCODE_SIMPLE_FIELD | (1u << 3),
                             PF::FILTER_OPCODE_SIMPLE_FIELD | (7u << 3),
                             PF::FILTER_OPCODE_SIMPLE_FIELD | (8u << 3),
                             PF::FILTER_OPCODE_SIMPLE_FIELD | (127u << 3),
                             PF::FILTER_OPCODE_END_OF_MESSAGE}));
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
  EXPECT_TRUE(
      LoadBytecode(&parser, {PF::FILTER_OPCODE_SIMPLE_FIELD | (1000u << 3),
                             PF::FILTER_OPCODE_SIMPLE_FIELD | (1001u << 3),
                             PF::FILTER_OPCODE_SIMPLE_FIELD | (2000u << 3),
                             PF::FILTER_OPCODE_END_OF_MESSAGE}));
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
  EXPECT_FALSE(LoadBytecode(
      &parser, {
                   PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (2u << 3),
               }));

  // Borderline valid: range length = 0.
  EXPECT_TRUE(LoadBytecode(
      &parser, {PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (2u << 3), 0u,
                PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (127u << 3), 0u,
                PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (128u << 3), 0u,
                PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (128u << 3), 0u,
                PF::FILTER_OPCODE_END_OF_MESSAGE}));
  for (uint32_t i = 0; i < 130; ++i)
    EXPECT_FALSE(parser.Query(0, i).allowed) << i;

  // A valid bytecode with two ranges [2,2], [10, 14].
  EXPECT_TRUE(
      LoadBytecode(&parser, {PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (2u << 3),
                             1u,  // length of the range,
                             PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (10u << 3),
                             5u,  // length of the range,
                             PF::FILTER_OPCODE_END_OF_MESSAGE}));
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
      LoadBytecode(&parser, {PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (1u << 3),
                             2u,  // [1,2]

                             PF::FILTER_OPCODE_SIMPLE_FIELD | (4u << 3),

                             PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (126u << 3),
                             4u,  // [126, 129]

                             PF::FILTER_OPCODE_SIMPLE_FIELD | (150u << 3),

                             PF::FILTER_OPCODE_END_OF_MESSAGE}));
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
  EXPECT_FALSE(
      LoadBytecode(&parser, {PF::FILTER_OPCODE_NESTED_FIELD | (4u << 3),
                             1u,  // message index
                             PF::FILTER_OPCODE_END_OF_MESSAGE}));

  // A valid bytecode consisting of 4 message, with recursive / cylical
  // dependencies between them.
  EXPECT_TRUE(LoadBytecode(
      &parser, {
                   // Message 0 (root).
                   PF::FILTER_OPCODE_SIMPLE_FIELD_RANGE | (1u << 3),
                   2u,  // [1,2]
                   PF::FILTER_OPCODE_NESTED_FIELD | (4u << 3),
                   3u,  // message index
                   PF::FILTER_OPCODE_SIMPLE_FIELD | (10u << 3),
                   PF::FILTER_OPCODE_NESTED_FIELD | (127u << 3),
                   1u,  // message index
                   PF::FILTER_OPCODE_NESTED_FIELD | (128u << 3),
                   2u,  // message index
                   PF::FILTER_OPCODE_END_OF_MESSAGE,

                   // Message 1.
                   PF::FILTER_OPCODE_NESTED_FIELD | (2u << 3),
                   1u,  // message index (recurse onto itself),
                   PF::FILTER_OPCODE_SIMPLE_FIELD | (11u << 3),
                   PF::FILTER_OPCODE_END_OF_MESSAGE,

                   // Message 2.
                   PF::FILTER_OPCODE_NESTED_FIELD | (2u << 3),
                   3u,  // message index.
                   PF::FILTER_OPCODE_END_OF_MESSAGE,

                   // Message 3.
                   PF::FILTER_OPCODE_NESTED_FIELD | (2u << 3),
                   2u,  // message index (create a cycle, 2->3, 3->2).
                   PF::FILTER_OPCODE_END_OF_MESSAGE,
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
