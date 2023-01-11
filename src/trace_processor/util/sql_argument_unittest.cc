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

#include "src/trace_processor/util/sql_argument.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace sql_argument {
namespace {

void ParseArgsSuccessfully(const std::string& args,
                           std::vector<ArgumentDefinition> expected) {
  std::vector<ArgumentDefinition> actual;
  base::Status status = ParseArgumentDefinitions(args, actual);
  ASSERT_TRUE(status.ok()) << status.c_message();
  ASSERT_EQ(expected, actual);
}

void ParseArgsWithFailure(const std::string& args) {
  std::vector<ArgumentDefinition> actual;
  ASSERT_FALSE(ParseArgumentDefinitions(args, actual).ok());
}

TEST(SqlArgumentTest, IsValidName) {
  ASSERT_TRUE(IsValidName("foo"));
  ASSERT_TRUE(IsValidName("bar"));
  ASSERT_TRUE(IsValidName("foo_bar"));
  ASSERT_TRUE(IsValidName("foo1234"));
  ASSERT_TRUE(IsValidName("1234Foo"));
  ASSERT_FALSE(IsValidName("foo-bar"));
  ASSERT_FALSE(IsValidName("foo#123"));
}

TEST(SqlArgumentTest, ParseType) {
  ASSERT_EQ(ParseType("PROTO"), Type::kProto);
  ASSERT_EQ(ParseType("BOOL"), Type::kBool);
  ASSERT_EQ(ParseType("UNKNOWN"), base::nullopt);
  ASSERT_EQ(ParseType("UINT"), Type::kUint);
}

TEST(SqlArgumentTest, TypeToFriendlyString) {
  ASSERT_STREQ(TypeToHumanFriendlyString(Type::kProto), "PROTO");
  ASSERT_STREQ(TypeToHumanFriendlyString(Type::kBool), "BOOL");
  ASSERT_STREQ(TypeToHumanFriendlyString(Type::kUint), "UINT");
}

TEST(SqlArgumentTest, TypeToSqlValueType) {
  ASSERT_EQ(TypeToSqlValueType(Type::kProto), SqlValue::Type::kBytes);
  ASSERT_EQ(TypeToSqlValueType(Type::kBool), SqlValue::Type::kLong);
  ASSERT_EQ(TypeToSqlValueType(Type::kUint), SqlValue::Type::kLong);
}

TEST(SqlArgumentTest, ParseArguments) {
  ParseArgsSuccessfully("", {});
  ParseArgsSuccessfully("foo UINT", {ArgumentDefinition("$foo", Type::kUint)});
  ParseArgsSuccessfully("foo UINT, bar LONG, baz PROTO",
                        {ArgumentDefinition("$foo", Type::kUint),
                         ArgumentDefinition("$bar", Type::kLong),
                         ArgumentDefinition("$baz", Type::kProto)});
  ParseArgsSuccessfully("\nfoo UINT,\n bar LONG, baz PROTO\n",
                        {ArgumentDefinition("$foo", Type::kUint),
                         ArgumentDefinition("$bar", Type::kLong),
                         ArgumentDefinition("$baz", Type::kProto)});
  ParseArgsSuccessfully("foo123 UINT",
                        {ArgumentDefinition("$foo123", Type::kUint)});

  ParseArgsWithFailure("foo");
  ParseArgsWithFailure("foo bar UINT, baz UINT");
  ParseArgsWithFailure("foo UINT32");
  ParseArgsWithFailure("foo#bar UINT");
  ParseArgsWithFailure("foo-bar UINT");
}

}  // namespace
}  // namespace sql_argument
}  // namespace trace_processor
}  // namespace perfetto
