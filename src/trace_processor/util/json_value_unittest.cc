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

#include "src/trace_processor/util/json_value.h"

#include <limits>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::json {
namespace {

// The most valuable test - exercises the entire serialize/parse pipeline
// with nested structures. If basic functionality breaks, this catches it.
TEST(DomTest, RoundTrip) {
  Dom original(Type::kObject);
  original["string"] = "test";
  original["int"] = 42;
  original["double"] = 3.14159;
  original["bool"] = true;
  original["null"] = Dom();
  original["nested"] = Dom(Type::kObject);
  original["nested"]["inner"] = "value";
  original["array"] = Dom(Type::kArray);
  original["array"].Append(1);
  original["array"].Append("two");

  std::string json = Serialize(original);
  auto parsed = Parse(json);
  ASSERT_TRUE(parsed.ok());

  EXPECT_EQ((*parsed)["string"].AsString(), "test");
  EXPECT_EQ((*parsed)["int"].AsInt(), 42);
  EXPECT_DOUBLE_EQ((*parsed)["double"].AsDouble(), 3.14159);
  EXPECT_TRUE((*parsed)["bool"].AsBool());
  EXPECT_TRUE((*parsed)["null"].IsNull());
  EXPECT_EQ((*parsed)["nested"]["inner"].AsString(), "value");
  EXPECT_EQ((*parsed)["array"][0].AsInt(), 1);
  EXPECT_EQ((*parsed)["array"][1].AsString(), "two");
}

// Tests deep copy independence - could catch shallow copy bugs
TEST(DomTest, CopyDeepNested) {
  Dom original(Type::kObject);
  original["a"] = Dom(Type::kObject);
  original["a"]["b"] = 42;

  Dom copy = original.Copy();
  original["a"]["b"] = 999;

  EXPECT_EQ(copy["a"]["b"].AsInt(), 42);  // Copy unaffected
}

// Tests auto-conversion behavior which is non-obvious and could regress
TEST(DomTest, AutoConversion) {
  // Mutable access on non-object converts to object
  Dom v1(42);
  v1["key"] = "value";
  EXPECT_TRUE(v1.IsObject());

  // Const access doesn't create entries
  const Dom obj(Type::kObject);
  EXPECT_TRUE(obj["missing"].IsNull());
  EXPECT_EQ(obj.size(), 0u);
}

// Tests escape sequence handling in parsing
TEST(DomTest, ParseEscapes) {
  auto r1 = Parse(R"({"a":"line\nwith\ttabs\"and\\slashes"})");
  ASSERT_TRUE(r1.ok());
  EXPECT_EQ((*r1)["a"].AsString(), "line\nwith\ttabs\"and\\slashes");

  auto r2 = Parse(R"({"u":"\u0048\u0065\u006c\u006c\u006f"})");
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ((*r2)["u"].AsString(), "Hello");
}

TEST(DomTest, ParseErrors) {
  EXPECT_FALSE(Parse("{invalid}").ok());
  EXPECT_FALSE(Parse("").ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::json
