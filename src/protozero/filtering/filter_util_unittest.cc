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
#include <cstring>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/protozero/filtering/filter_bytecode_generator.h"
#include "src/protozero/filtering/filter_bytecode_parser.h"
#include "test/gtest_and_gmock.h"

namespace protozero {

namespace {

perfetto::base::TempFile MkTemp(const char* str) {
  auto tmp = perfetto::base::TempFile::Create();
  perfetto::base::WriteAll(*tmp, str, strlen(str));
  perfetto::base::FlushFile(*tmp);
  return tmp;
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
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional int32 i32 = 13;
    optional fixed64 f64 = 5;
    optional string str = 71;
  }
  )");
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "Root", ""));
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
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    message Child {
      repeated fixed64 f64 = 3;
      optional Child recurse = 4;
    }
    oneof xxx { int32 i32 = 1; }
    optional Child chld = 2;
  }
  )");
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "", ""));
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
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    message Nested {
      message Child1 {
        optional int32 f1 = 3;
        optional int64 f2 = 4;
      }
      message Child2 {
        optional string f1 = 3;
        optional bytes f2 = 4;
      }
      message ChildNonDedupe {
        optional string f1 = 3;
        optional bytes f2 = 4;
        optional int32 extra = 1;
      }
      optional Child1 chld1 = 1;
      optional Child2 chld2 = 2;
      optional ChildNonDedupe chld3 = 3;
    }
    repeated Nested nested = 1;
  }
  )");
  FilterUtil filter;
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "Root", ""));
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
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    message Nested {
      message Child1 {
        optional int32 f1 = 3;
        optional int64 f2 = 4;
        repeated Child2 c2 = 5;
      }
      message Child2 {
        optional string f3 = 6;
        optional bytes f4 = 7;
        repeated Child1 c1 = 8;
      }
      optional Child1 x1 = 1;
      optional Child2 x2 = 2;
    }
    repeated Nested n = 1;
  }
  )");

  FilterUtil filter;
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "Root", ""));
  std::vector<uint32_t> fld;

  fld = {1, 1, 3};
  ASSERT_EQ(filter.LookupField(fld.data(), fld.size()), ".n.x1.f1");

  fld = {1, 2, 7};
  ASSERT_EQ(filter.LookupField(fld.data(), fld.size()), ".n.x2.f4");

  fld = {1, 2, 8, 5, 8, 5, 7};
  ASSERT_EQ(filter.LookupField(fld.data(), fld.size()), ".n.x2.c1.c2.c1.c2.f4");
}

TEST(SchemaParserTest, PrintAsText) {
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional int32 i32 = 13;
    optional Child1 c1 = 2;
    optional Child2 c2 = 7;
  }
  message Child1 {
    optional int32 f1 = 3;
    optional int64 f2 = 4;
  }
  message Child2 {
    optional int32 f1 = 3;
    optional int64 f2 = 4;
    repeated Root c1 = 5;
    repeated Nested n1 = 6;
    message Nested {
      optional int64 f1 = 1;
    }
  }
  )");

  FilterUtil filter;
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "Root", ""));

  EXPECT_EQ(R"(Root 2 message c1 Child1
Root 7 message c2 Child2
Root 13 int32 i32
Child1 3 int32 f1
Child1 4 int64 f2
Child2 3 int32 f1
Child2 4 int64 f2
Child2 5 message c1 Root
Child2 6 message n1 Child2.Nested
Child2.Nested 1 int64 f1
)",
            FilterToText(filter));

  // If we generate bytecode from the schema itself, all fields are allowed and
  // the result is identical to the unfiltered output.
  EXPECT_EQ(FilterToText(filter),
            FilterToText(filter, filter.GenerateFilterBytecode().bytecode));
}

TEST(SchemaParserTest, PrintAsTextWithBytecodeFiltering) {
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional int32 i32 = 13;
    optional Child1 c1 = 2;
    optional Child2 c2 = 7;
  }
  message Child1 {
    optional int32 f1 = 3;
    optional int64 f2 = 4;
  }
  message Child2 {
    optional int32 f1 = 3;
    optional int64 f2 = 4;
    repeated Root c1 = 5;
    repeated Nested n1 = 6;
    message Nested {
      optional int64 f1 = 1;
    }
  }
  )");

  FilterUtil filter;
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "Root", ""));

  auto schema_subset = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional Child2 c2 = 7;
  }
  message Child1 {
    optional int32 f1 = 3;
    optional int64 f2 = 4;
  }
  message Child2 {
    optional int64 f2 = 4;
    repeated Root c1 = 5;
    repeated Nested n1 = 6;
    message Nested {
      optional int64 f1 = 1;
    }
  }
  )");

  FilterUtil filter_subset;
  ASSERT_TRUE(
      filter_subset.LoadMessageDefinition(schema_subset.path(), "Root", ""));
  std::string bytecode = filter_subset.GenerateFilterBytecode().bytecode;

  // Note: Child1 isn't listed even though the filter allows it, because it
  // isn't reachable from the root message.
  EXPECT_EQ(R"(Root 7 message c2 Child2
Child2 4 int64 f2
Child2 5 message c1 Root
Child2 6 message n1 Child2.Nested
Child2.Nested 1 int64 f1
)",
            FilterToText(filter, bytecode));
}

TEST(SchemaParserTest, Passthrough) {
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional int32 i32 = 13;
    optional TracePacket packet = 7;
  }
  message TraceConfig {
    optional int32 f3 = 3;
    optional int64 f4 = 4;
  }
  message TracePacket {
    optional int32 f1 = 3;
    optional int64 f2 = 4;
    optional TraceConfig cfg = 5;
  }
  )");

  FilterUtil filter;
  std::set<std::string> passthrough{"TracePacket:cfg"};
  ASSERT_TRUE(
      filter.LoadMessageDefinition(schema.path(), "Root", "", passthrough));

  EXPECT_EQ(R"(Root 7 message packet TracePacket
Root 13 int32 i32
TracePacket 3 int32 f1
TracePacket 4 int64 f2
TracePacket 5 bytes cfg
)",
            FilterToText(filter));

  std::string bytecode = filter.GenerateFilterBytecode().bytecode;
  // If we generate bytecode from the schema itself, all fields are allowed and
  // the result is identical to the unfiltered output.
  EXPECT_EQ(FilterToText(filter), FilterToText(filter, bytecode));
}

TEST(SchemaParserTest, FilterString) {
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional int32 i32 = 13;
    optional TracePacket packet = 7;
  }
  message TraceConfig {
    optional string f1 = 1;
  }
  message TracePacket {
    optional int32 f1 = 3;
    optional int64 f2 = 4;
    optional TraceConfig cfg = 5;
  }
  )");

  FilterUtil filter;
  std::set<std::string> filter_string{"TraceConfig:f1"};
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "Root", "", {},
                                           filter_string));

  EXPECT_EQ(R"(Root 7 message packet TracePacket
Root 13 int32 i32
TracePacket 3 int32 f1
TracePacket 4 int64 f2
TracePacket 5 message cfg TraceConfig
TraceConfig 1 string f1 # FILTER STRING
)",
            FilterToText(filter));

  std::string bytecode = filter.GenerateFilterBytecode().bytecode;
  // If we generate bytecode from the schema itself, all fields are allowed and
  // the result is identical to the unfiltered output.
  EXPECT_EQ(FilterToText(filter), FilterToText(filter, bytecode));
}

TEST(SchemaParserTest, FilterStringWithSemanticType) {
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional int32 i32 = 13;
    optional TracePacket packet = 7;
  }
  message TracePacket {
    optional string name = 3;
    optional string category = 4;
  }
  )");

  FilterUtil filter;
  std::set<std::string> filter_string{"TracePacket:name",
                                      "TracePacket:category"};
  std::map<std::string, uint32_t> semantic_types{
      {"TracePacket:name", 1},      // SEMANTIC_TYPE_ATRACE
      {"TracePacket:category", 2},  // SEMANTIC_TYPE_JOB
  };
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "Root", "", {},
                                           filter_string, semantic_types));

  // Generate bytecode with v54 (should use AddFilterStringFieldWithType)
  auto result_v54 = filter.GenerateFilterBytecode(
      FilterBytecodeGenerator::BytecodeVersion::kV54);
  EXPECT_GT(result_v54.bytecode.size(), 0u);
  EXPECT_EQ(result_v54.v54_overlay.size(), 0u);  // No overlay needed for v54

  // Parse the bytecode and verify semantic types are present
  FilterBytecodeParser parser;
  ASSERT_TRUE(
      parser.Load(result_v54.bytecode.data(), result_v54.bytecode.size()));

  // Query the TracePacket message (index 1) for field 3 (name)
  auto query_name = parser.Query(1, 3);
  EXPECT_TRUE(query_name.allowed);
  EXPECT_TRUE(query_name.filter_string_field());
  EXPECT_EQ(query_name.semantic_type, 1u);

  // Query field 4 (category)
  auto query_category = parser.Query(1, 4);
  EXPECT_TRUE(query_category.allowed);
  EXPECT_TRUE(query_category.filter_string_field());
  EXPECT_EQ(query_category.semantic_type, 2u);
}

TEST(SchemaParserTest, FilterStringWithSemanticTypeV2) {
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional TracePacket packet = 1;
  }
  message TracePacket {
    optional string name = 2;
  }
  )");

  FilterUtil filter;
  std::set<std::string> filter_string{"TracePacket:name"};
  std::map<std::string, uint32_t> semantic_types{{"TracePacket:name", 1}};
  ASSERT_TRUE(filter.LoadMessageDefinition(schema.path(), "Root", "", {},
                                           filter_string, semantic_types));

  // Generate bytecode targeting v2 parsers (should generate overlay)
  auto result_v2 = filter.GenerateFilterBytecode(
      FilterBytecodeGenerator::BytecodeVersion::kV2);
  EXPECT_GT(result_v2.bytecode.size(), 0u);
  EXPECT_GT(result_v2.v54_overlay.size(), 0u);  // Overlay should be present

  // Verify base bytecode has FilterString without semantic type
  FilterBytecodeParser parser_base;
  ASSERT_TRUE(
      parser_base.Load(result_v2.bytecode.data(), result_v2.bytecode.size()));
  auto query_base = parser_base.Query(1, 2);
  EXPECT_TRUE(query_base.allowed);
  EXPECT_TRUE(query_base.filter_string_field());
  EXPECT_EQ(query_base.semantic_type, 0u);  // No semantic type in base

  // Verify overlay provides the semantic type
  FilterBytecodeParser parser_overlay;
  ASSERT_TRUE(parser_overlay.Load(
      result_v2.bytecode.data(), result_v2.bytecode.size(),
      result_v2.v54_overlay.data(), result_v2.v54_overlay.size()));
  auto query_overlay = parser_overlay.Query(1, 2);
  EXPECT_TRUE(query_overlay.allowed);
  EXPECT_TRUE(query_overlay.filter_string_field());
  EXPECT_EQ(query_overlay.semantic_type, 1u);  // Semantic type from overlay
}

TEST(SchemaParserTest, SemanticTypeValidation) {
  auto schema = MkTemp(R"(
  syntax = "proto2";
  message Root {
    optional string field = 1;
  }
  )");

  FilterUtil filter;
  // Semantic type without filter_string should fail
  std::map<std::string, uint32_t> semantic_types{{"Root:field", 1}};
  EXPECT_FALSE(filter.LoadMessageDefinition(schema.path(), "Root", "", {}, {},
                                            semantic_types));
}

}  // namespace
}  // namespace protozero
