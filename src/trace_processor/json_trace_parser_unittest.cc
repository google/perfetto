/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/json_trace_parser.h"

#include <json/value.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(JsonTraceParserTest, CoerceToUint32) {
  uint32_t n = 0;

  ASSERT_TRUE(CoerceToUint32(Json::Value(42), &n));
  EXPECT_EQ(n, 42);

  ASSERT_TRUE(CoerceToUint32(Json::Value("42"), &n));
  EXPECT_EQ(n, 42);
}

TEST(JsonTraceParserTest, CoerceToUint64) {
  int64_t n = 0;

  ASSERT_TRUE(CoerceToInt64(Json::Value(42), &n));
  EXPECT_EQ(n, 42);

  ASSERT_TRUE(CoerceToInt64(Json::Value("42"), &n));
  EXPECT_EQ(n, 42);

  ASSERT_FALSE(CoerceToInt64(Json::Value("foo"), &n));
  ASSERT_FALSE(CoerceToInt64(Json::Value("1234!"), &n));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
