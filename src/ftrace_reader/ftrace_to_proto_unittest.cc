/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/ftrace_reader/ftrace_to_proto.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace {

TEST(FtraceEventParser, GetNameFromTypeAndName) {
  EXPECT_EQ(GetNameFromTypeAndName("int foo"), "foo");
  EXPECT_EQ(GetNameFromTypeAndName("int foo_bar"), "foo_bar");
  EXPECT_EQ(GetNameFromTypeAndName("const char * foo"), "foo");
  EXPECT_EQ(GetNameFromTypeAndName("const char foo[64]"), "foo");
  EXPECT_EQ(GetNameFromTypeAndName("char[] foo[16]"), "foo");
  EXPECT_EQ(GetNameFromTypeAndName("u8 foo[(int)sizeof(struct blah)]"), "foo");

  EXPECT_EQ(GetNameFromTypeAndName(""), "");
  EXPECT_EQ(GetNameFromTypeAndName("]"), "");
  EXPECT_EQ(GetNameFromTypeAndName("["), "");
  EXPECT_EQ(GetNameFromTypeAndName(" "), "");
  EXPECT_EQ(GetNameFromTypeAndName(" []"), "");
  EXPECT_EQ(GetNameFromTypeAndName(" ]["), "");
  EXPECT_EQ(GetNameFromTypeAndName("char"), "");
  EXPECT_EQ(GetNameFromTypeAndName("char *"), "");
  EXPECT_EQ(GetNameFromTypeAndName("char 42"), "");
}

TEST(FtraceEventParser, InferProtoType) {
  using Field = FtraceEvent::Field;
  EXPECT_EQ(InferProtoType(Field{"char * foo", 2, 0, false}), "string");
  EXPECT_EQ(InferProtoType(Field{"char foo[16]", 0, 16, false}), "string");
  EXPECT_EQ(InferProtoType(Field{"char bar_42[64]", 0, 64, false}), "string");

  EXPECT_EQ(InferProtoType(Field{"int foo", 0, 4, true}), "int32");
  EXPECT_EQ(InferProtoType(Field{"s32 signal", 50, 4, true}), "int32");

  EXPECT_EQ(InferProtoType(Field{"unsigned int foo", 0, 4, false}), "uint32");
  EXPECT_EQ(InferProtoType(Field{"u32 control_freq", 44, 4, false}), "uint32");

  EXPECT_EQ(InferProtoType(Field{"char foo", 0, 0, false}), "string");
}

TEST(FtraceEventParser, GenerateProtoName) {
  FtraceEvent input;
  Proto output;
  input.name = "the_snake_case_name";

  GenerateProto(input, &output);

  EXPECT_EQ(output.name, "TheSnakeCaseNameFtraceEvent");
}

}  // namespace
}  // namespace perfetto
