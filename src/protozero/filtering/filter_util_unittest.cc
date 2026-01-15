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

#include "src/protozero/filtering/filter_util.h"

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/protozero/filtering/filter_bytecode_generator.h"
#include "src/protozero/filtering/filter_bytecode_parser.h"
#include "src/protozero/filtering/filter_util_test_messages.descriptor.h"
#include "test/gtest_and_gmock.h"

namespace protozero {

namespace {

const uint8_t* TestDescriptor() {
  return perfetto::kFilterUtilTestMessagesDescriptor.data();
}

size_t TestDescriptorSize() {
  return perfetto::kFilterUtilTestMessagesDescriptor.size();
}

std::string FilterToText(FilterUtil& filter,
                         const std::optional<std::string>& bytecode = {}) {
  std::string tmp_path = perfetto::base::TempFile::Create().path();
  {
    perfetto::base::ScopedFstream tmp_stream(
        perfetto::base::OpenFstream(tmp_path, "w"));
    PERFETTO_CHECK(!!tmp_stream);
    filter.set_print_stream_for_testing(*tmp_stream);
    filter.PrintAsText(bytecode);
    filter.set_print_stream_for_testing(stdout);
  }
  std::string output;
  PERFETTO_CHECK(perfetto::base::ReadFile(tmp_path, &output));
  // Make the output a bit more compact.
  output = std::regex_replace(output, std::regex(" +"), " ");
  return std::regex_replace(output, std::regex(" +\\n"), "\n");
}

TEST(SchemaParserTest, SchemaToBytecode_Simple) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.SimpleRoot"));
  std::string bytecode = filter.GenerateFilterBytecode().bytecode;
  FilterBytecodeParser fbp;
  ASSERT_TRUE(fbp.Load(bytecode.data(), bytecode.size()));
  EXPECT_TRUE(fbp.Query(0, 13).allowed);
  EXPECT_TRUE(fbp.Query(0, 13).simple_field());
  EXPECT_TRUE(fbp.Query(0, 5).allowed);
  EXPECT_TRUE(fbp.Query(0, 5).simple_field());
  EXPECT_TRUE(fbp.Query(0, 71).allowed);
  EXPECT_TRUE(fbp.Query(0, 71).simple_field());
  EXPECT_FALSE(fbp.Query(0, 1).allowed);
  EXPECT_FALSE(fbp.Query(0, 12).allowed);
  EXPECT_FALSE(fbp.Query(0, 70).allowed);
}

TEST(SchemaParserTest, SchemaToBytecode_Nested) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.NestedRoot"));
  std::string bytecode = filter.GenerateFilterBytecode().bytecode;
  FilterBytecodeParser fbp;
  ASSERT_TRUE(fbp.Load(bytecode.data(), bytecode.size()));
  EXPECT_TRUE(fbp.Query(0, 1).allowed);
  EXPECT_TRUE(fbp.Query(0, 1).simple_field());
  EXPECT_TRUE(fbp.Query(0, 2).allowed);
  EXPECT_FALSE(fbp.Query(0, 2).simple_field());
  // False as those fields exist only in Child, not in the root (0).
  EXPECT_FALSE(fbp.Query(0, 3).allowed);
  EXPECT_FALSE(fbp.Query(0, 4).allowed);

  EXPECT_TRUE(fbp.Query(1, 3).allowed);
  EXPECT_TRUE(fbp.Query(1, 3).simple_field());
  EXPECT_TRUE(fbp.Query(1, 4).allowed);
  EXPECT_FALSE(fbp.Query(1, 4).simple_field());
  EXPECT_EQ(fbp.Query(1, 4).nested_msg_index, 1u);  // Self
}

TEST(SchemaParserTest, SchemaToBytecode_Dedupe) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.DedupeRoot"));
  filter.Dedupe();
  std::string bytecode = filter.GenerateFilterBytecode().bytecode;
  FilterBytecodeParser fbp;
  ASSERT_TRUE(fbp.Load(bytecode.data(), bytecode.size()));

  // 0: Root
  EXPECT_TRUE(fbp.Query(0, 1).allowed);
  EXPECT_FALSE(fbp.Query(0, 1).simple_field());

  // 1: Nested
  EXPECT_TRUE(fbp.Query(1, 1).allowed);
  EXPECT_FALSE(fbp.Query(1, 1).simple_field());
  EXPECT_TRUE(fbp.Query(1, 2).allowed);
  EXPECT_FALSE(fbp.Query(1, 2).simple_field());
  EXPECT_TRUE(fbp.Query(1, 3).allowed);
  EXPECT_FALSE(fbp.Query(1, 3).simple_field());

  // Check deduping.
  // Fields chld1 and chld2 should point to the same sub-filter because they
  // have the same field ids.
  EXPECT_EQ(fbp.Query(1, 1).nested_msg_index, fbp.Query(1, 2).nested_msg_index);

  // Field chld3 should point to a different one because it has an extra field.
  EXPECT_NE(fbp.Query(1, 1).nested_msg_index, fbp.Query(1, 3).nested_msg_index);
}

TEST(SchemaParserTest, FieldLookup) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.LookupRoot"));
  std::vector<uint32_t> fld;

  fld = {1, 1, 3};
  ASSERT_EQ(filter.LookupField(fld.data(), fld.size()), ".n.x1.f1");

  fld = {1, 2, 7};
  ASSERT_EQ(filter.LookupField(fld.data(), fld.size()), ".n.x2.f4");

  fld = {1, 2, 8, 5, 8, 5, 7};
  ASSERT_EQ(filter.LookupField(fld.data(), fld.size()), ".n.x2.c1.c2.c1.c2.f4");
}

TEST(SchemaParserTest, PrintAsText) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.PrintRoot"));

  EXPECT_EQ(R"(PrintRoot 2 message c1 PrintChild1
PrintRoot 7 message c2 PrintChild2
PrintRoot 13 int32 i32
PrintChild1 3 int32 f1
PrintChild1 4 int64 f2
PrintChild2 3 int32 f1
PrintChild2 4 int64 f2
PrintChild2 5 message c1 PrintRoot
PrintChild2 6 message n1 PrintChild2.Nested
PrintChild2.Nested 1 int64 f1
)",
            FilterToText(filter));

  // If we generate bytecode from the schema itself, all fields are allowed and
  // the result is identical to the unfiltered output.
  EXPECT_EQ(FilterToText(filter),
            FilterToText(filter, filter.GenerateFilterBytecode().bytecode));
}

TEST(SchemaParserTest, PrintAsTextWithBytecodeFiltering) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.PrintRoot"));

  FilterUtil filter_subset;
  ASSERT_TRUE(filter_subset.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(),
      "protozero.test.PrintRootSubset"));
  std::string bytecode = filter_subset.GenerateFilterBytecode().bytecode;

  // Note: PrintChild1 isn't listed even though the filter allows it, because
  // it isn't reachable from the root message.
  EXPECT_EQ(R"(PrintRoot 7 message c2 PrintChild2
PrintChild2 4 int64 f2
PrintChild2 5 message c1 PrintRoot
PrintChild2 6 message n1 PrintChild2.Nested
PrintChild2.Nested 1 int64 f1
)",
            FilterToText(filter, bytecode));
}

TEST(SchemaParserTest, Passthrough) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.PassthroughRoot"));

  EXPECT_EQ(R"(PassthroughRoot 7 message packet PassthroughPacket
PassthroughRoot 13 int32 i32
PassthroughPacket 3 int32 f1
PassthroughPacket 4 int64 f2
PassthroughPacket 5 bytes cfg
)",
            FilterToText(filter));

  std::string bytecode = filter.GenerateFilterBytecode().bytecode;
  // If we generate bytecode from the schema itself, all fields are allowed and
  // the result is identical to the unfiltered output.
  EXPECT_EQ(FilterToText(filter), FilterToText(filter, bytecode));
}

TEST(SchemaParserTest, FilterString) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.FilterStringRoot"));

  EXPECT_EQ(R"(FilterStringRoot 7 message packet FilterStringPacket
FilterStringRoot 13 int32 i32
FilterStringPacket 3 int32 f1
FilterStringPacket 4 int64 f2
FilterStringPacket 5 message cfg FilterStringConfig
FilterStringConfig 1 string f1 # FILTER STRING
)",
            FilterToText(filter));

  std::string bytecode = filter.GenerateFilterBytecode().bytecode;
  // If we generate bytecode from the schema itself, all fields are allowed and
  // the result is identical to the unfiltered output.
  EXPECT_EQ(FilterToText(filter), FilterToText(filter, bytecode));
}

TEST(SchemaParserTest, FilterStringWithSemanticType) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.SemanticTypeRoot"));

  // Generate bytecode with v54 (should use AddFilterStringFieldWithType)
  auto result_v54 = filter.GenerateFilterBytecode(
      FilterBytecodeGenerator::BytecodeVersion::kV54);
  EXPECT_GT(result_v54.bytecode.size(), 0u);
  EXPECT_EQ(result_v54.v54_overlay.size(), 0u);  // No overlay needed for v54

  // Parse the bytecode and verify semantic types are present
  FilterBytecodeParser parser;
  ASSERT_TRUE(
      parser.Load(result_v54.bytecode.data(), result_v54.bytecode.size()));

  // Query the SemanticTypePacket message (index 1) for field 3 (name)
  auto query_name = parser.Query(1, 3);
  EXPECT_TRUE(query_name.allowed);
  EXPECT_TRUE(query_name.filter_string_field());
  EXPECT_EQ(query_name.semantic_type, 1u);  // SEMANTIC_TYPE_ATRACE

  // Query field 4 (category)
  auto query_category = parser.Query(1, 4);
  EXPECT_TRUE(query_category.allowed);
  EXPECT_TRUE(query_category.filter_string_field());
  EXPECT_EQ(query_category.semantic_type, 2u);  // SEMANTIC_TYPE_JOB
}

TEST(SchemaParserTest, FilterStringWithSemanticTypeV2) {
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(),
      "protozero.test.SemanticTypeV2Root"));

  // Generate bytecode targeting v2 parsers (should generate overlay)
  auto result_v2 = filter.GenerateFilterBytecode(
      FilterBytecodeGenerator::BytecodeVersion::kV2);
  EXPECT_GT(result_v2.bytecode.size(), 0u);
  EXPECT_GT(result_v2.v54_overlay.size(), 0u);  // Overlay should be present

  // Verify base bytecode denies the field (v2 doesn't support semantic types)
  FilterBytecodeParser parser_base;
  ASSERT_TRUE(
      parser_base.Load(result_v2.bytecode.data(), result_v2.bytecode.size()));
  auto query_base = parser_base.Query(1, 2);
  EXPECT_FALSE(query_base.allowed);  // Field is denied in v2
  EXPECT_FALSE(query_base.filter_string_field());

  // Verify overlay provides the semantic type
  FilterBytecodeParser parser_overlay;
  ASSERT_TRUE(parser_overlay.Load(
      result_v2.bytecode.data(), result_v2.bytecode.size(),
      result_v2.v54_overlay.data(), result_v2.v54_overlay.size()));
  auto query_overlay = parser_overlay.Query(1, 2);
  EXPECT_TRUE(query_overlay.allowed);
  EXPECT_TRUE(query_overlay.filter_string_field());
  EXPECT_EQ(query_overlay.semantic_type, 1u);  // SEMANTIC_TYPE_ATRACE
}

TEST(SchemaParserTest, SemanticTypeValidation) {
  FilterUtil filter;
  // Semantic type on non-string field should fail (ValidationRoot has
  // semantic_type annotation on an int32 field).
  EXPECT_FALSE(filter.LoadFromDescriptorSet(
      TestDescriptor(), TestDescriptorSize(), "protozero.test.ValidationRoot"));
}

}  // namespace
}  // namespace protozero
