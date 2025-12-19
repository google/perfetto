/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/util/json_parser.h"

#include <string>
#include <string_view>

#include "perfetto/base/status.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::json {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

class JsonParserTest : public ::testing::Test {
 protected:
  void Parse(std::string_view str,
             JsonValue& value,
             std::string& unescaped_str,
             bool expect_ok = true) {
    const char* begin = str.data();
    const char* end = begin + str.size();
    const char* cur = begin;
    base::Status status;
    if (!internal::SkipWhitespace(cur, end)) {
      return;
    }
    auto res = ParseValue(cur, end, value, unescaped_str, status);
    if (expect_ok) {
      EXPECT_EQ(res, ReturnCode::kOk);
      EXPECT_TRUE(status.ok()) << status.message();
    } else {
      EXPECT_NE(res, ReturnCode::kOk);
      EXPECT_FALSE(status.ok());
    }
  }
};

TEST_F(JsonParserTest, ParseNull) {
  constexpr std::string_view kJson = "null";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  EXPECT_TRUE(std::holds_alternative<Null>(value));
}

TEST_F(JsonParserTest, ParseTrue) {
  constexpr std::string_view kJson = "true";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<bool>(value));
  EXPECT_TRUE(std::get<bool>(value));
}

TEST_F(JsonParserTest, ParseFalse) {
  constexpr std::string_view kJson = "false";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<bool>(value));
  EXPECT_FALSE(std::get<bool>(value));
}

TEST_F(JsonParserTest, ParseInteger) {
  constexpr std::string_view kJson = "12345,";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<int64_t>(value));
  EXPECT_EQ(std::get<int64_t>(value), 12345);
}

TEST_F(JsonParserTest, ParseNegativeInteger) {
  constexpr std::string_view kJson = "-12345,";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<int64_t>(value));
  EXPECT_EQ(std::get<int64_t>(value), -12345);
}

TEST_F(JsonParserTest, ParseDouble) {
  constexpr std::string_view kJson = "123.45,";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<double>(value));
  EXPECT_DOUBLE_EQ(std::get<double>(value), 123.45);
}

TEST_F(JsonParserTest, ParseLargeDouble) {
  constexpr std::string_view kJson = "1750244461563845.0,";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<double>(value));
  EXPECT_DOUBLE_EQ(std::get<double>(value), 1750244461563845.0);
}

TEST_F(JsonParserTest, ParseString) {
  constexpr std::string_view kJson = "\"hello world\"";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(value));
  EXPECT_EQ(std::get<std::string_view>(value), "hello world");
}

TEST_F(JsonParserTest, ParseStringWithEscapes) {
  constexpr std::string_view kJson = "\"hello \\\"world\\\"\"";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(value));
  EXPECT_EQ(std::get<std::string_view>(value), "hello \"world\"");
}

TEST_F(JsonParserTest, ParseStringEndingWithBackslash) {
  constexpr std::string_view kJson = "\"value\\\\\"";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(value));
  EXPECT_EQ(std::get<std::string_view>(value), "value\\");
}

TEST_F(JsonParserTest, ParseStringWithEscapesInMiddle) {
  constexpr std::string_view kJson = "\"hello\\nworld\"";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(value));
  EXPECT_EQ(std::get<std::string_view>(value), "hello\nworld");
}

TEST_F(JsonParserTest, ParseEmptyString) {
  constexpr std::string_view kJson = "\"\"";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(value));
  EXPECT_EQ(std::get<std::string_view>(value), "");
}

TEST_F(JsonParserTest, ParseObject) {
  constexpr std::string_view kJson = "{\"key\": \"value\"}";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<Object>(value));
  EXPECT_EQ(std::get<Object>(value).contents, "{\"key\": \"value\"}");
}

TEST_F(JsonParserTest, ParseArray) {
  constexpr std::string_view kJson = "[1, 2, 3]";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str);
  ASSERT_TRUE(std::holds_alternative<Array>(value));
  EXPECT_EQ(std::get<Array>(value).contents, "[1, 2, 3]");
}

TEST_F(JsonParserTest, InvalidToken) {
  constexpr std::string_view kJson = "invalid";
  JsonValue value;
  std::string str;
  Parse(kJson, value, str, false);
}

TEST(JsonParserIteratorTest, EmptyObject) {
  constexpr std::string_view kJson = "{}";
  Iterator it;
  it.Reset(kJson.data(), kJson.data() + kJson.size());
  ASSERT_TRUE(it.ParseStart());
  ASSERT_EQ(it.ParseObjectFieldWithoutRecursing(), ReturnCode::kEndOfScope);
  ASSERT_TRUE(it.eof());
}

TEST(JsonParserIteratorTest, SimpleObject) {
  constexpr std::string_view kJson = R"({"key": "value", "key2": 123})";
  Iterator it;
  it.Reset(kJson.data(), kJson.data() + kJson.size());
  ASSERT_TRUE(it.ParseStart());

  ASSERT_EQ(it.ParseObjectFieldWithoutRecursing(), ReturnCode::kOk);
  EXPECT_EQ(it.key(), "key");
  ASSERT_TRUE(std::holds_alternative<std::string_view>(it.value()));
  EXPECT_EQ(std::get<std::string_view>(it.value()), "value");

  ASSERT_EQ(it.ParseObjectFieldWithoutRecursing(), ReturnCode::kOk);
  EXPECT_EQ(it.key(), "key2");
  ASSERT_TRUE(std::holds_alternative<int64_t>(it.value()));
  EXPECT_EQ(std::get<int64_t>(it.value()), 123);

  ASSERT_EQ(it.ParseObjectFieldWithoutRecursing(), ReturnCode::kEndOfScope);
  ASSERT_TRUE(it.eof());
}

TEST(JsonParserIteratorTest, NestedObject) {
  constexpr std::string_view kJson =
      R"({"key": {"nested_key": "nested_value"}})";
  Iterator it;
  it.Reset(kJson.data(), kJson.data() + kJson.size());
  ASSERT_TRUE(it.ParseStart());

  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kOk);
  EXPECT_EQ(it.key(), "key");
  ASSERT_TRUE(std::holds_alternative<Object>(it.value()));

  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kOk);
  EXPECT_EQ(it.key(), "nested_key");
  ASSERT_TRUE(std::holds_alternative<std::string_view>(it.value()));
  EXPECT_EQ(std::get<std::string_view>(it.value()), "nested_value");

  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kEndOfScope);
  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kEndOfScope);
  ASSERT_TRUE(it.eof());
}

TEST(JsonParserIteratorTest, SimpleArray) {
  constexpr std::string_view kJson = R"(["value", 123, true, null])";
  Iterator it;
  it.Reset(kJson.data(), kJson.data() + kJson.size());
  ASSERT_TRUE(it.ParseStart());

  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kOk);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(it.value()));
  EXPECT_EQ(std::get<std::string_view>(it.value()), "value");

  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kOk);
  ASSERT_TRUE(std::holds_alternative<int64_t>(it.value()));
  EXPECT_EQ(std::get<int64_t>(it.value()), 123);

  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kOk);
  ASSERT_TRUE(std::holds_alternative<bool>(it.value()));
  EXPECT_TRUE(std::get<bool>(it.value()));

  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kOk);
  ASSERT_TRUE(std::holds_alternative<Null>(it.value()));

  ASSERT_EQ(it.ParseAndRecurse(), ReturnCode::kEndOfScope);
  ASSERT_TRUE(it.eof());
}

TEST(JsonParserUnescapeTest, Unescape) {
  std::string res;
  base::Status status;
  constexpr std::string_view kEscaped = R"(\"\\\/\b\f\n\r\t\u1234)";
  auto ret = internal::UnescapeString(
      kEscaped.data(), kEscaped.data() + kEscaped.size(), res, status);
  ASSERT_EQ(ret, internal::ReturnCode::kOk);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(res, "\"\\/\b\f\n\r\t\xe1\x88\xb4");
}

}  // namespace
}  // namespace perfetto::trace_processor::json
