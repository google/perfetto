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

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/protozero/filtering/filter_bytecode_parser.h"
#include "src/protozero/filtering/filter_util.h"

namespace protozero {

namespace {

perfetto::base::TempFile MkTemp(const char* str) {
  auto tmp = perfetto::base::TempFile::Create();
  perfetto::base::WriteAll(*tmp, str, strlen(str));
  perfetto::base::FlushFile(*tmp);
  return tmp;
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
  std::string bytecode = filter.GenerateFilterBytecode();
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
  std::string bytecode = filter.GenerateFilterBytecode();
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
  std::string bytecode = filter.GenerateFilterBytecode();
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

}  // namespace
}  // namespace protozero
