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

#include "src/trace_processor/export_json.h"

#include "perfetto/base/temp_file.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <json/reader.h>
#include <json/value.h>

namespace perfetto {
namespace trace_processor {
namespace json {
namespace {

std::string ReadFile(FILE* input) {
  fseek(input, 0, SEEK_SET);
  const int kBufSize = 1000;
  char buffer[kBufSize];
  fread(buffer, sizeof(char), kBufSize, input);
  return std::string(buffer);
}

TEST(ExportJsonTest, EmptyStorage) {
  TraceStorage storage;

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;

  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 0);
}

TEST(ExportJsonTest, StorageWithOneSlice) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = 10000;
  const int64_t kThreadID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(kThreadID);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "X");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["dur"].asInt64(), kDuration / 1000);
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
}

TEST(ExportJsonTest, StorageWithThreadName) {
  const int64_t kThreadID = 100;
  const char* kName = "thread";

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(kThreadID);
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.GetMutableThread(utid)->name_id = name_id;

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "M");
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["name"].asString(), "thread_name");
  EXPECT_EQ(event["args"]["name"].asString(), kName);
}

TEST(ExportJsonTest, WrongRefType) {
  TraceStorage storage;
  storage.mutable_nestable_slices()->AddSlice(0, 0, 0, RefType::kRefCpuId, 0, 0,
                                              0, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultWrongRefType);
}

}  // namespace
}  // namespace json
}  // namespace trace_processor
}  // namespace perfetto
