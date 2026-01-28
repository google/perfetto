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

#include "src/trace_processor/util/simple_json_parser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::json {
namespace {

using ::testing::ElementsAre;

// Comprehensive test - array of objects with nested arrays
// Tests ForEachField, ForEachArrayElement, and nested parsing together.
TEST(SimpleJsonParserTest, ArrayOfObjectsWithNestedArrays) {
  std::string json = R"({
    "events":[
      {"id":1,"name":"a","tags":["x","y"]},
      {"id":2,"name":"b","tags":["z"]}
    ]
  })";
  SimpleJsonParser parser(json);
  ASSERT_TRUE(parser.Parse().ok());

  struct Event {
    int64_t id;
    std::string name;
    std::vector<std::string> tags;
  };
  std::vector<Event> events;

  auto status = parser.ForEachField([&](std::string_view key) -> FieldResult {
    if (key == "events") {
      auto s = parser.ForEachArrayElement([&]() {
        Event e{};
        auto s2 = parser.ForEachField([&](std::string_view k) -> FieldResult {
          if (k == "id") {
            e.id = parser.GetInt64().value_or(0);
          } else if (k == "name") {
            e.name = std::string(parser.GetString().value_or(""));
          } else if (k == "tags") {
            auto tags = parser.CollectStringArray();
            if (tags.ok())
              e.tags = std::move(*tags);
          }
          return FieldResult::Handled{};
        });
        if (!s2.ok())
          return s2;
        events.push_back(std::move(e));
        return base::OkStatus();
      });
      if (!s.ok())
        return s;
      return FieldResult::Handled{};
    }
    return FieldResult::Skip{};
  });

  ASSERT_TRUE(status.ok());
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].id, 1);
  EXPECT_EQ(events[0].name, "a");
  EXPECT_THAT(events[0].tags, ElementsAre("x", "y"));
  EXPECT_EQ(events[1].id, 2);
  EXPECT_THAT(events[1].tags, ElementsAre("z"));
}

// Tests that Skip properly skips deeply nested unknown fields
TEST(SimpleJsonParserTest, SkipDeeplyNested) {
  std::string json = R"({
    "skip_me":{"a":{"b":{"c":[1,2,{"d":3}]}}},
    "keep":"value"
  })";
  SimpleJsonParser parser(json);
  ASSERT_TRUE(parser.Parse().ok());

  std::string keep;
  auto status = parser.ForEachField([&](std::string_view key) -> FieldResult {
    if (key == "keep") {
      keep = std::string(parser.GetString().value_or(""));
      return FieldResult::Handled{};
    }
    return FieldResult::Skip{};
  });

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(keep, "value");
}

// Tests error propagation during iteration
TEST(SimpleJsonParserTest, ParseErrorDuringIteration) {
  std::string json = R"({"key": invalid_value})";
  SimpleJsonParser parser(json);
  if (parser.Parse().ok()) {
    auto status = parser.ForEachField(
        [](std::string_view) -> FieldResult { return FieldResult::Handled{}; });
    EXPECT_FALSE(status.ok());
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::json
