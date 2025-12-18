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

#include "src/protozero/filtering/filter_bytecode_parser.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "perfetto/ext/base/fnv_hash.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "src/protozero/filtering/filter_bytecode_common.h"
#include "test/gtest_and_gmock.h"

namespace protozero {

namespace {

bool LoadBytecode(FilterBytecodeParser* parser,
                  std::initializer_list<uint32_t> bytecode) {
  perfetto::base::FnvHasher hasher;
  protozero::PackedVarInt words;
  for (uint32_t w : bytecode) {
    words.Append(w);
    hasher.Update(w);
  }
  words.Append(static_cast<uint32_t>(hasher.digest()));
  return parser->Load(words.data(), words.size());
}

std::vector<uint8_t> MakeOverlay(std::initializer_list<uint32_t> words) {
  perfetto::base::FnvHasher hasher;
  protozero::PackedVarInt packed;
  for (uint32_t w : words) {
    packed.Append(w);
    hasher.Update(w);
  }
  packed.Append(static_cast<uint32_t>(hasher.digest()));
  return {packed.data(), packed.data() + packed.size()};
}

bool LoadBytecodeWithOverlay(FilterBytecodeParser* parser,
                             std::initializer_list<uint32_t> bytecode,
                             std::initializer_list<uint32_t> overlay) {
  perfetto::base::FnvHasher hasher;
  protozero::PackedVarInt words;
  for (uint32_t w : bytecode) {
    words.Append(w);
    hasher.Update(w);
  }
  words.Append(static_cast<uint32_t>(hasher.digest()));

  // Build the overlay with checksum.
  auto overlay_bytes = MakeOverlay(overlay);

  return parser->Load(words.data(), words.size(), overlay_bytes.data(),
                      overlay_bytes.size());
}

TEST(FilterBytecodeParserTest, EomHandling) {
  FilterBytecodeParser parser;

  // EOM not being correctly at the end should cause a parse failure.
  EXPECT_FALSE(LoadBytecode(&parser, {kFilterOpcode_SimpleField | 1}));
  EXPECT_FALSE(LoadBytecode(&parser, {kFilterOpcode_SimpleFieldRange | 1,
                                      kFilterOpcode_EndOfMessage}));
  EXPECT_FALSE(LoadBytecode(&parser, {kFilterOpcode_NestedField | (4u << 3),
                                      kFilterOpcode_EndOfMessage}));
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

TEST(FilterBytecodeParserTest, OverlayUpgradeToFilterString) {
  FilterBytecodeParser parser;

  // Base: fields 1 (simple), 2 (simple), 3 (simple)
  // Overlay: upgrade field 2 to FilterString
  EXPECT_TRUE(LoadBytecodeWithOverlay(
      &parser,
      {kFilterOpcode_SimpleField | (1u << 3),
       kFilterOpcode_SimpleField | (2u << 3),
       kFilterOpcode_SimpleField | (3u << 3), kFilterOpcode_EndOfMessage},
      {0u,                                             // msg_index
       kFilterOpcode_FilterString | (2u << 3), 0u}));  // argument (unused)

  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 1).simple_field());

  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_TRUE(parser.Query(0, 2).filter_string_field());

  EXPECT_TRUE(parser.Query(0, 3).allowed);
  EXPECT_TRUE(parser.Query(0, 3).simple_field());
}

TEST(FilterBytecodeParserTest, OverlayAddNewField) {
  FilterBytecodeParser parser;

  // Base: fields 1, 3
  // Overlay: add field 2 as FilterString
  EXPECT_TRUE(LoadBytecodeWithOverlay(
      &parser,
      {kFilterOpcode_SimpleField | (1u << 3),
       kFilterOpcode_SimpleField | (3u << 3), kFilterOpcode_EndOfMessage},
      {0u,                                             // msg_index
       kFilterOpcode_FilterString | (2u << 3), 0u}));  // argument (unused)

  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 1).simple_field());

  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_TRUE(parser.Query(0, 2).filter_string_field());

  EXPECT_TRUE(parser.Query(0, 3).allowed);
  EXPECT_TRUE(parser.Query(0, 3).simple_field());
}

TEST(FilterBytecodeParserTest, OverlayAddFieldAtEnd) {
  FilterBytecodeParser parser;

  // Base: fields 1, 2
  // Overlay: add field 5 as SimpleField
  EXPECT_TRUE(LoadBytecodeWithOverlay(
      &parser,
      {kFilterOpcode_SimpleField | (1u << 3),
       kFilterOpcode_SimpleField | (2u << 3), kFilterOpcode_EndOfMessage},
      {0u,                                            // msg_index
       kFilterOpcode_SimpleField | (5u << 3), 0u}));  // argument (unused)

  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_FALSE(parser.Query(0, 3).allowed);
  EXPECT_FALSE(parser.Query(0, 4).allowed);
  EXPECT_TRUE(parser.Query(0, 5).allowed);
  EXPECT_TRUE(parser.Query(0, 5).simple_field());
}

TEST(FilterBytecodeParserTest, OverlayMultipleEntries) {
  FilterBytecodeParser parser;

  // Base: fields 1, 5, 10
  // Overlay: add field 3, upgrade field 5, add field 7
  EXPECT_TRUE(LoadBytecodeWithOverlay(
      &parser,
      {kFilterOpcode_SimpleField | (1u << 3),
       kFilterOpcode_SimpleField | (5u << 3),
       kFilterOpcode_SimpleField | (10u << 3), kFilterOpcode_EndOfMessage},
      {0u, kFilterOpcode_FilterString | (3u << 3), 0u,    // add field 3
       0u, kFilterOpcode_FilterString | (5u << 3), 0u,    // upgrade field 5
       0u, kFilterOpcode_SimpleField | (7u << 3), 0u}));  // add field 7

  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 1).simple_field());

  EXPECT_FALSE(parser.Query(0, 2).allowed);

  EXPECT_TRUE(parser.Query(0, 3).allowed);
  EXPECT_TRUE(parser.Query(0, 3).filter_string_field());

  EXPECT_FALSE(parser.Query(0, 4).allowed);

  EXPECT_TRUE(parser.Query(0, 5).allowed);
  EXPECT_TRUE(parser.Query(0, 5).filter_string_field());

  EXPECT_FALSE(parser.Query(0, 6).allowed);

  EXPECT_TRUE(parser.Query(0, 7).allowed);
  EXPECT_TRUE(parser.Query(0, 7).simple_field());

  EXPECT_TRUE(parser.Query(0, 10).allowed);
  EXPECT_TRUE(parser.Query(0, 10).simple_field());
}

TEST(FilterBytecodeParserTest, OverlayMultipleMessages) {
  FilterBytecodeParser parser;

  // Base: Message 0 has field 1, Message 1 has field 2
  // Overlay: add field 3 to message 0, add field 4 to message 1
  EXPECT_TRUE(LoadBytecodeWithOverlay(
      &parser,
      {// Message 0
       kFilterOpcode_SimpleField | (1u << 3), kFilterOpcode_EndOfMessage,
       // Message 1
       kFilterOpcode_SimpleField | (2u << 3), kFilterOpcode_EndOfMessage},
      {0u, kFilterOpcode_FilterString | (3u << 3), 0u,     // msg 0, field 3
       1u, kFilterOpcode_FilterString | (4u << 3), 0u}));  // msg 1, field 4

  // Message 0
  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 1).simple_field());
  EXPECT_FALSE(parser.Query(0, 2).allowed);
  EXPECT_TRUE(parser.Query(0, 3).allowed);
  EXPECT_TRUE(parser.Query(0, 3).filter_string_field());

  // Message 1
  EXPECT_FALSE(parser.Query(1, 1).allowed);
  EXPECT_TRUE(parser.Query(1, 2).allowed);
  EXPECT_TRUE(parser.Query(1, 2).simple_field());
  EXPECT_FALSE(parser.Query(1, 3).allowed);
  EXPECT_TRUE(parser.Query(1, 4).allowed);
  EXPECT_TRUE(parser.Query(1, 4).filter_string_field());
}

TEST(FilterBytecodeParserTest, OverlayLargeFieldId) {
  FilterBytecodeParser parser;

  // Base: field 1
  // Overlay: add field 200 (> 128, uses range storage)
  EXPECT_TRUE(LoadBytecodeWithOverlay(
      &parser,
      {kFilterOpcode_SimpleField | (1u << 3), kFilterOpcode_EndOfMessage},
      {0u, kFilterOpcode_FilterString | (200u << 3), 0u}));

  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_FALSE(parser.Query(0, 127).allowed);
  EXPECT_FALSE(parser.Query(0, 128).allowed);
  EXPECT_FALSE(parser.Query(0, 199).allowed);
  EXPECT_TRUE(parser.Query(0, 200).allowed);
  EXPECT_TRUE(parser.Query(0, 200).filter_string_field());
  EXPECT_FALSE(parser.Query(0, 201).allowed);
}

TEST(FilterBytecodeParserTest, OverlayEmptyOverlay) {
  FilterBytecodeParser parser;

  // Empty overlay should behave same as no overlay.
  EXPECT_TRUE(LoadBytecodeWithOverlay(
      &parser,
      {kFilterOpcode_SimpleField | (1u << 3),
       kFilterOpcode_SimpleField | (2u << 3), kFilterOpcode_EndOfMessage},
      {}));  // Empty overlay

  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 1).simple_field());
  EXPECT_TRUE(parser.Query(0, 2).allowed);
  EXPECT_TRUE(parser.Query(0, 2).simple_field());
  EXPECT_FALSE(parser.Query(0, 3).allowed);
}

TEST(FilterBytecodeParserTest, OverlayErrorInvalidOpcode) {
  FilterBytecodeParser parser;
  parser.set_suppress_logs_for_fuzzer(true);

  // Overlay with invalid opcode (EndOfMessage = 0 is not valid in overlay)
  EXPECT_FALSE(LoadBytecodeWithOverlay(
      &parser,
      {kFilterOpcode_SimpleField | (1u << 3), kFilterOpcode_EndOfMessage},
      {0u, kFilterOpcode_EndOfMessage}));  // Invalid opcode
}

TEST(FilterBytecodeParserTest, OverlayErrorTruncated) {
  FilterBytecodeParser parser;
  parser.set_suppress_logs_for_fuzzer(true);

  // Overlay with only msg_index, missing field_word.
  // We need to manually construct this malformed overlay.
  perfetto::base::FnvHasher hasher;
  protozero::PackedVarInt packed;
  packed.Append(0u);  // msg_index only, no field_word
  hasher.Update(0u);
  packed.Append(static_cast<uint32_t>(hasher.digest()));

  perfetto::base::FnvHasher base_hasher;
  protozero::PackedVarInt base;
  base.Append(kFilterOpcode_SimpleField | (1u << 3));
  base_hasher.Update(kFilterOpcode_SimpleField | (1u << 3));
  base.Append(kFilterOpcode_EndOfMessage);
  base_hasher.Update(kFilterOpcode_EndOfMessage);
  base.Append(static_cast<uint32_t>(base_hasher.digest()));

  EXPECT_FALSE(
      parser.Load(base.data(), base.size(), packed.data(), packed.size()));
}

TEST(FilterBytecodeParserTest, OverlayErrorNotSorted) {
  FilterBytecodeParser parser;
  parser.set_suppress_logs_for_fuzzer(true);

  // Overlay entries not sorted by msg_index (entry for msg 0 after msg 1)
  EXPECT_FALSE(LoadBytecodeWithOverlay(
      &parser,
      {// Message 0
       kFilterOpcode_SimpleField | (1u << 3),
       kFilterOpcode_EndOfMessage,  // Message 1
       kFilterOpcode_SimpleField | (2u << 3), kFilterOpcode_EndOfMessage},
      {1u, kFilterOpcode_FilterString | (3u << 3),     // msg 1 first
       0u, kFilterOpcode_FilterString | (4u << 3)}));  // msg 0 after - error!
}

}  // namespace
}  // namespace protozero
