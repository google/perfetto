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
#include "src/protozero/filtering/filter_bytecode_generator.h"
#include "src/protozero/filtering/filter_bytecode_parser.h"

// This file tests the generator, assuming the parser is good.
// The parser is tested separately (without the generator) in
// filter_bytecode_parser_unittest.cc

namespace protozero {

namespace {

TEST(FilterBytecodeGeneratorTest, SimpleFields) {
  FilterBytecodeGenerator gen;
  gen.AddSimpleField(1u);
  gen.AddSimpleField(127u);
  gen.AddSimpleField(128u);
  gen.AddSimpleField(1000u);
  gen.EndMessage();

  FilterBytecodeParser parser;
  std::string bytecode = gen.Serialize();
  ASSERT_TRUE(parser.Load(reinterpret_cast<const uint8_t*>(bytecode.data()),
                          bytecode.size()));
  EXPECT_FALSE(parser.Query(0, 0).allowed);
  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_FALSE(parser.Query(0, 126).allowed);
  EXPECT_TRUE(parser.Query(0, 127).allowed);
  EXPECT_TRUE(parser.Query(0, 128).allowed);
  EXPECT_FALSE(parser.Query(0, 129).allowed);
  EXPECT_TRUE(parser.Query(0, 1000).allowed);
  EXPECT_FALSE(parser.Query(0, 1001).allowed);
}

TEST(FilterBytecodeGeneratorTest, SimpleAndRanges) {
  FilterBytecodeGenerator gen;
  gen.AddSimpleField(1u);
  gen.AddSimpleFieldRange(10, 10);
  gen.AddSimpleField(30u);
  gen.AddSimpleFieldRange(120, 20);
  gen.AddSimpleField(1000u);
  gen.EndMessage();

  FilterBytecodeParser parser;
  std::string bytecode = gen.Serialize();
  ASSERT_TRUE(parser.Load(reinterpret_cast<const uint8_t*>(bytecode.data()),
                          bytecode.size()));
  EXPECT_FALSE(parser.Query(0, 0).allowed);
  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_FALSE(parser.Query(0, 9).allowed);
  for (uint32_t i = 10; i <= 19; ++i)
    EXPECT_TRUE(parser.Query(0, i).allowed);
  EXPECT_TRUE(parser.Query(0, 30).allowed);
  for (uint32_t i = 120; i <= 139; ++i)
    EXPECT_TRUE(parser.Query(0, i).allowed);
  EXPECT_FALSE(parser.Query(0, 140).allowed);
  EXPECT_FALSE(parser.Query(0, 999).allowed);
  EXPECT_TRUE(parser.Query(0, 1000).allowed);
  EXPECT_FALSE(parser.Query(0, 1001).allowed);
}

TEST(FilterBytecodeGeneratorTest, Nested) {
  FilterBytecodeGenerator gen;
  // Message 0.
  gen.AddSimpleField(1u);
  gen.AddSimpleFieldRange(10, 1);
  gen.AddNestedField(11, 3);
  gen.AddNestedField(12, 1);
  gen.EndMessage();

  // Message 1.
  gen.AddNestedField(11, 1);  // Recursive
  gen.AddNestedField(12, 2);  // Recursive
  gen.AddNestedField(13, 3);  // Recursive
  gen.EndMessage();

  // Message 2.
  gen.AddSimpleField(21);
  gen.EndMessage();

  // Message 3.
  gen.AddNestedField(1, 0);  // Recurse in the root message (sneaky).
  gen.AddSimpleField(31);
  gen.EndMessage();

  FilterBytecodeParser parser;
  std::string bytecode = gen.Serialize();
  ASSERT_TRUE(parser.Load(reinterpret_cast<const uint8_t*>(bytecode.data()),
                          bytecode.size()));

  // Check root message.
  EXPECT_TRUE(parser.Query(0, 1).allowed);
  EXPECT_TRUE(parser.Query(0, 1).simple_field());
  EXPECT_TRUE(parser.Query(0, 10).allowed);
  EXPECT_TRUE(parser.Query(0, 10).simple_field());
  EXPECT_TRUE(parser.Query(0, 11).allowed);
  EXPECT_EQ(parser.Query(0, 11).nested_msg_index, 3u);
  EXPECT_TRUE(parser.Query(0, 12).allowed);
  EXPECT_EQ(parser.Query(0, 12).nested_msg_index, 1u);
  EXPECT_FALSE(parser.Query(0, 13).allowed);

  // Check message 1.
  EXPECT_FALSE(parser.Query(1, 10).allowed);
  EXPECT_TRUE(parser.Query(1, 11).allowed);
  EXPECT_EQ(parser.Query(1, 11).nested_msg_index, 1u);
  EXPECT_TRUE(parser.Query(1, 12).allowed);
  EXPECT_EQ(parser.Query(1, 12).nested_msg_index, 2u);
  EXPECT_TRUE(parser.Query(1, 13).allowed);
  EXPECT_EQ(parser.Query(1, 13).nested_msg_index, 3u);

  // Check message 2.
  EXPECT_FALSE(parser.Query(2, 11).allowed);
  EXPECT_TRUE(parser.Query(2, 21).allowed);
  EXPECT_TRUE(parser.Query(2, 21).simple_field());

  // Check message 3.
  EXPECT_TRUE(parser.Query(3, 1).allowed);
  EXPECT_EQ(parser.Query(3, 1).nested_msg_index, 0u);
  EXPECT_TRUE(parser.Query(3, 31).allowed);
  EXPECT_TRUE(parser.Query(3, 31).simple_field());
}

}  // namespace
}  // namespace protozero
