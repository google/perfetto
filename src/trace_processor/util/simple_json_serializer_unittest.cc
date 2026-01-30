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

#include "src/trace_processor/util/simple_json_serializer.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::json {
namespace {

// Comprehensive test covering all the APIs with nested structures
TEST(SimpleJsonSerializerTest, Complex) {
  std::string result = SerializeJson([](JsonValueSerializer&& writer) {
    std::move(writer).WriteDict([](JsonDictSerializer& dict) {
      dict.AddNull("null_val");
      dict.AddBool("bool_val", true);
      dict.AddInt("int_val", int64_t{-42});
      dict.AddUint("uint_val", uint64_t{18446744073709551615ULL});
      dict.AddDouble("double_val", 3.14);
      dict.AddString("string_val", "hello");
      dict.AddString("escaped", "a\"b\\c\nd");
      dict.AddArray("items", [](JsonArraySerializer& arr) {
        arr.AppendDict([](JsonDictSerializer& obj) {
          obj.AddInt("id", int64_t{1});
          obj.AddArray("tags", [](JsonArraySerializer& tags) {
            tags.AppendString("x");
            tags.AppendString("y");
          });
        });
      });
      dict.AddDict("nested", [](JsonDictSerializer& nested) {
        nested.AddInt("inner", int64_t{99});
      });
    });
  });

  // Check key parts of the output
  EXPECT_TRUE(result.find("\"null_val\":null") != std::string::npos);
  EXPECT_TRUE(result.find("\"bool_val\":true") != std::string::npos);
  EXPECT_TRUE(result.find("\"int_val\":-42") != std::string::npos);
  EXPECT_TRUE(result.find("18446744073709551615") != std::string::npos);
  EXPECT_TRUE(result.find("\"escaped\":\"a\\\"b\\\\c\\nd\"") !=
              std::string::npos);
  EXPECT_TRUE(result.find("\"tags\":[\"x\",\"y\"]") != std::string::npos);
  EXPECT_TRUE(result.find("\"inner\":99") != std::string::npos);
}

// Tests special double handling - unique functionality
TEST(SimpleJsonSerializerTest, SpecialDoubles) {
  std::string result = SerializeJson([](JsonValueSerializer&& writer) {
    std::move(writer).WriteDict([](JsonDictSerializer& dict) {
      dict.AddDouble("nan", std::nan(""));
      dict.AddDouble("inf", std::numeric_limits<double>::infinity());
      dict.AddDouble("neg_inf", -std::numeric_limits<double>::infinity());
    });
  });

  EXPECT_TRUE(result.find("\"nan\":\"NaN\"") != std::string::npos);
  EXPECT_TRUE(result.find("\"inf\":\"Infinity\"") != std::string::npos);
  EXPECT_TRUE(result.find("\"neg_inf\":\"-Infinity\"") != std::string::npos);
}

}  // namespace
}  // namespace perfetto::trace_processor::json
