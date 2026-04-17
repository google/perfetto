/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/util/json_serializer.h"

#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::json {
namespace {

// Tests pretty-printing - the only test needed for this feature
TEST(JsonSerializerTest, PrettyPrint) {
  JsonSerializer s(JsonSerializer::kPretty);
  s.OpenObject();
  s.Key("outer");
  s.OpenObject();
  s.Key("inner");
  s.NumberValue(1);
  s.CloseObject();
  s.Key("array");
  s.OpenArray();
  s.NumberValue(2);
  s.NumberValue(3);
  s.CloseArray();
  s.CloseObject();

  std::string expected =
      "{\n"
      "  \"outer\": {\n"
      "    \"inner\": 1\n"
      "  },\n"
      "  \"array\": [\n"
      "    2,\n"
      "    3\n"
      "  ]\n"
      "}";
  EXPECT_EQ(s.ToString(), expected);
}

// Tests UTF-8 surrogate pair encoding (4-byte sequences)
// This is the most complex case that could break
TEST(JsonSerializerTest, Utf8SurrogatePair) {
  JsonSerializer s;
  // U+1D11E (musical G clef) = UTF-8: F0 9D 84 9E
  // Should become surrogate pair: \uD834\uDD1E
  s.StringValue("\xf0\x9d\x84\x9e");
  EXPECT_EQ(s.ToString(), R"("\ud834\udd1e")");
}

}  // namespace
}  // namespace perfetto::trace_processor::json
