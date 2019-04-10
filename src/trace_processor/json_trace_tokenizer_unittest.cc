/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/json_trace_tokenizer.h"

#include <json/value.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(JsonTraceTokenizerTest, Success) {
  const char* start = R"({ "foo": "bar" })";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  Json::Value value;
  ReadDictRes result = ReadOneJsonDict(start, end, &value, &next);

  ASSERT_EQ(result, kFoundDict);
  ASSERT_EQ(next, end);
  ASSERT_EQ(value["foo"].asString(), "bar");
}

TEST(JsonTraceTokenizerTest, QuotedBraces) {
  const char* start = R"({ "foo": "}\"bar{\\" })";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  Json::Value value;
  ReadDictRes result = ReadOneJsonDict(start, end, &value, &next);

  ASSERT_EQ(result, kFoundDict);
  ASSERT_EQ(next, end);
  ASSERT_EQ(value["foo"].asString(), "}\"bar{\\");
}

TEST(JsonTraceTokenizerTest, TwoDicts) {
  const char* start = R"({"foo": 1}, {"bar": 2})";
  const char* middle = start + strlen(R"({"foo": 1})");
  const char* end = start + strlen(start);
  const char* next = nullptr;
  Json::Value value;

  ASSERT_EQ(ReadOneJsonDict(start, end, &value, &next), kFoundDict);
  ASSERT_EQ(next, middle);
  ASSERT_EQ(value["foo"].asInt(), 1);

  ASSERT_EQ(ReadOneJsonDict(next, end, &value, &next), kFoundDict);
  ASSERT_EQ(next, end);
  ASSERT_EQ(value["bar"].asInt(), 2);
}

TEST(JsonTraceTokenizerTest, NeedMoreData) {
  const char* start = R"({"foo": 1)";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  Json::Value value;

  ASSERT_EQ(ReadOneJsonDict(start, end, &value, &next), kNeedsMoreData);
  ASSERT_EQ(next, nullptr);
}

TEST(JsonTraceTokenizerTest, FatalError) {
  const char* start = R"({helloworld})";
  const char* end = start + strlen(start);
  const char* next = nullptr;
  Json::Value value;

  ASSERT_EQ(ReadOneJsonDict(start, end, &value, &next), kFatalError);
  ASSERT_EQ(next, nullptr);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
