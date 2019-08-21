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

#include "perfetto/ext/base/temp_file.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include "test/gtest_and_gmock.h"

#include <json/reader.h>
#include <json/value.h>

#include <limits>

namespace perfetto {
namespace trace_processor {
namespace json {
namespace {

std::string ReadFile(FILE* input) {
  fseek(input, 0, SEEK_SET);
  const int kBufSize = 1000;
  char buffer[kBufSize];
  size_t ret = fread(buffer, sizeof(char), kBufSize, input);
  EXPECT_GT(ret, 0u);
  return std::string(buffer, ret);
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
  EXPECT_EQ(result["traceEvents"].size(), 0u);
}

TEST(ExportJsonTest, StorageWithOneSlice) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = 10000;
  const int64_t kThreadTimestamp = 20000000;
  const int64_t kThreadDuration = 20000;
  const int64_t kThreadInstructionCount = 30000000;
  const int64_t kThreadInstructionDelta = 30000;
  const int64_t kThreadID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(kThreadID);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);
  storage.mutable_thread_slices()->AddThreadSlice(
      0, kThreadTimestamp, kThreadDuration, kThreadInstructionCount,
      kThreadInstructionDelta);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "X");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["dur"].asInt64(), kDuration / 1000);
  EXPECT_EQ(event["tts"].asInt64(), kThreadTimestamp / 1000);
  EXPECT_EQ(event["tdur"].asInt64(), kThreadDuration / 1000);
  EXPECT_EQ(event["ticount"].asInt64(), kThreadInstructionCount);
  EXPECT_EQ(event["tidelta"].asInt64(), kThreadInstructionDelta);
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_FALSE(event.isMember("args"));
}
TEST(ExportJsonTest, StorageWithOneUnfinishedSlice) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = -1;
  const int64_t kThreadTimestamp = 20000000;
  const int64_t kThreadDuration = -1;
  const int64_t kThreadInstructionCount = 30000000;
  const int64_t kThreadInstructionDelta = -1;
  const int64_t kThreadID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(kThreadID);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);
  storage.mutable_thread_slices()->AddThreadSlice(
      0, kThreadTimestamp, kThreadDuration, kThreadInstructionCount,
      kThreadInstructionDelta);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "B");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_FALSE(event.isMember("dur"));
  EXPECT_EQ(event["tts"].asInt64(), kThreadTimestamp / 1000);
  EXPECT_FALSE(event.isMember("tdur"));
  EXPECT_EQ(event["ticount"].asInt64(), kThreadInstructionCount);
  EXPECT_FALSE(event.isMember("tidelta"));
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_FALSE(event.isMember("args"));
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
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "M");
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["name"].asString(), "thread_name");
  EXPECT_EQ(event["args"]["name"].asString(), kName);
}

TEST(ExportJsonTest, WrongRefType) {
  TraceStorage storage;
  StringId cat_id = storage.InternString("cat");
  StringId name_id = storage.InternString("name");
  storage.mutable_nestable_slices()->AddSlice(0, 0, 0, RefType::kRefCpuId,
                                              cat_id, name_id, 0, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultWrongRefType);
}

TEST(ExportJsonTest, StorageWithMetadata) {
  const char* kDescription = "description";
  const char* kBenchmarkName = "benchmark name";
  const char* kStoryName = "story name";
  const char* kStoryTag1 = "tag1";
  const char* kStoryTag2 = "tag2";
  const int64_t kBenchmarkStart = 1000000;
  const int64_t kStoryStart = 2000000;
  const bool kHadFailures = true;

  TraceStorage storage;

  StringId desc_id = storage.InternString(base::StringView(kDescription));
  Variadic description = Variadic::String(desc_id);
  storage.SetMetadata(metadata::benchmark_description, description);

  StringId benchmark_name_id =
      storage.InternString(base::StringView(kBenchmarkName));
  Variadic benchmark_name = Variadic::String(benchmark_name_id);
  storage.SetMetadata(metadata::benchmark_name, benchmark_name);

  StringId story_name_id = storage.InternString(base::StringView(kStoryName));
  Variadic story_name = Variadic::String(story_name_id);
  storage.SetMetadata(metadata::benchmark_story_name, story_name);

  StringId tag1_id = storage.InternString(base::StringView(kStoryTag1));
  StringId tag2_id = storage.InternString(base::StringView(kStoryTag2));
  Variadic tag1 = Variadic::String(tag1_id);
  Variadic tag2 = Variadic::String(tag2_id);
  storage.AppendMetadata(metadata::benchmark_story_tags, tag1);
  storage.AppendMetadata(metadata::benchmark_story_tags, tag2);

  Variadic benchmark_start = Variadic::Integer(kBenchmarkStart);
  storage.SetMetadata(metadata::benchmark_start_time_us, benchmark_start);

  Variadic story_start = Variadic::Integer(kStoryStart);
  storage.SetMetadata(metadata::benchmark_story_run_time_us, story_start);

  Variadic had_failures = Variadic::Integer(kHadFailures);
  storage.SetMetadata(metadata::benchmark_had_failures, had_failures);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;

  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_TRUE(result.isMember("metadata"));
  EXPECT_TRUE(result["metadata"].isMember("telemetry"));
  Json::Value telemetry_metadata = result["metadata"]["telemetry"];

  EXPECT_EQ(telemetry_metadata["benchmarkDescriptions"].size(), 1u);
  EXPECT_EQ(telemetry_metadata["benchmarkDescriptions"][0].asString(),
            kDescription);

  EXPECT_EQ(telemetry_metadata["benchmarks"].size(), 1u);
  EXPECT_EQ(telemetry_metadata["benchmarks"][0].asString(), kBenchmarkName);

  EXPECT_EQ(telemetry_metadata["stories"].size(), 1u);
  EXPECT_EQ(telemetry_metadata["stories"][0].asString(), kStoryName);

  EXPECT_EQ(telemetry_metadata["storyTags"].size(), 2u);
  EXPECT_EQ(telemetry_metadata["storyTags"][0].asString(), kStoryTag1);
  EXPECT_EQ(telemetry_metadata["storyTags"][1].asString(), kStoryTag2);

  EXPECT_DOUBLE_EQ(telemetry_metadata["benchmarkStart"].asInt(),
                   kBenchmarkStart / 1000.0);

  EXPECT_DOUBLE_EQ(telemetry_metadata["traceStart"].asInt(),
                   kStoryStart / 1000.0);

  EXPECT_EQ(telemetry_metadata["hadFailures"].size(), 1u);
  EXPECT_EQ(telemetry_metadata["hadFailures"][0].asBool(), kHadFailures);
}

TEST(ExportJsonTest, StorageWithStats) {
  int64_t kProducers = 10;
  int64_t kBufferSize0 = 1000;
  int64_t kBufferSize1 = 2000;

  TraceStorage storage;

  storage.SetStats(stats::traced_producers_connected, kProducers);
  storage.SetIndexedStats(stats::traced_buf_buffer_size, 0, kBufferSize0);
  storage.SetIndexedStats(stats::traced_buf_buffer_size, 1, kBufferSize1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);
  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;

  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_TRUE(result.isMember("metadata"));
  EXPECT_TRUE(result["metadata"].isMember("perfetto_trace_stats"));
  Json::Value stats = result["metadata"]["perfetto_trace_stats"];

  EXPECT_EQ(stats["producers_connected"].asInt(), kProducers);
  EXPECT_EQ(stats["buffer_stats"].size(), 2u);
  EXPECT_EQ(stats["buffer_stats"][0]["buffer_size"].asInt(), kBufferSize0);
  EXPECT_EQ(stats["buffer_stats"][1]["buffer_size"].asInt(), kBufferSize1);
}

TEST(ExportJsonTest, StorageWithChromeMetadata) {
  const char* kName1 = "name1";
  const char* kName2 = "name2";
  const char* kValue1 = "value1";
  const int kValue2 = 222;

  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  TraceStorage* storage = context.storage.get();
  ArgsTracker args(&context);

  RowId row_id = storage->mutable_raw_events()->AddRawEvent(
      0, storage->InternString("chrome_event.metadata"), 0, 0);

  StringId name1_id = storage->InternString(base::StringView(kName1));
  StringId name2_id = storage->InternString(base::StringView(kName2));
  StringId value1_id = storage->InternString(base::StringView(kValue1));
  args.AddArg(row_id, name1_id, name1_id, Variadic::String(value1_id));
  args.AddArg(row_id, name2_id, name2_id, Variadic::Integer(kValue2));
  args.Flush();

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(storage, output);
  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;

  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_TRUE(result.isMember("metadata"));
  Json::Value metadata = result["metadata"];

  EXPECT_EQ(metadata[kName1].asString(), kValue1);
  EXPECT_EQ(metadata[kName2].asInt(), kValue2);
}

TEST(ExportJsonTest, StorageWithArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  const char* kSrc = "source_file.cc";

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(0);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(0, 0, utid, RefType::kRefUtid,
                                              cat_id, name_id, 0, 0, 0);

  StringId arg_key_id =
      storage.InternString(base::StringView("task.posted_from.file_name"));
  StringId arg_value_id = storage.InternString(base::StringView(kSrc));
  TraceStorage::Args::Arg arg;
  arg.flat_key = arg_key_id;
  arg.key = arg_key_id;
  arg.value = Variadic::String(arg_value_id);
  storage.mutable_args()->AddArgSet({arg}, 0, 1);
  storage.mutable_nestable_slices()->set_arg_set_id(0, 1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["src"].asString(), kSrc);
}

TEST(ExportJsonTest, StorageWithListArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  double kValues[] = {1.234, 2.345};

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(0);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(0, 0, utid, RefType::kRefUtid,
                                              cat_id, name_id, 0, 0, 0);

  StringId arg_flat_key_id =
      storage.InternString(base::StringView("debug.draw_duration_ms"));
  StringId arg_key0_id =
      storage.InternString(base::StringView("debug.draw_duration_ms[0]"));
  StringId arg_key1_id =
      storage.InternString(base::StringView("debug.draw_duration_ms[1]"));
  TraceStorage::Args::Arg arg0;
  arg0.flat_key = arg_flat_key_id;
  arg0.key = arg_key0_id;
  arg0.value = Variadic::Real(kValues[0]);
  TraceStorage::Args::Arg arg1;
  arg1.flat_key = arg_flat_key_id;
  arg1.key = arg_key1_id;
  arg1.value = Variadic::Real(kValues[1]);
  storage.mutable_args()->AddArgSet({arg0, arg1}, 0, 2);
  storage.mutable_nestable_slices()->set_arg_set_id(0, 1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["draw_duration_ms"].size(), 2u);
  EXPECT_DOUBLE_EQ(event["args"]["draw_duration_ms"][0].asDouble(), kValues[0]);
  EXPECT_DOUBLE_EQ(event["args"]["draw_duration_ms"][1].asDouble(), kValues[1]);
}

TEST(ExportJsonTest, StorageWithMultiplePointerArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  uint64_t kValue0 = 1;
  uint64_t kValue1 = std::numeric_limits<uint64_t>::max();

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(0);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(0, 0, utid, RefType::kRefUtid,
                                              cat_id, name_id, 0, 0, 0);

  StringId arg_key0_id = storage.InternString(base::StringView("arg0"));
  StringId arg_key1_id = storage.InternString(base::StringView("arg1"));
  TraceStorage::Args::Arg arg0;
  arg0.flat_key = arg_key0_id;
  arg0.key = arg_key0_id;
  arg0.value = Variadic::Pointer(kValue0);
  TraceStorage::Args::Arg arg1;
  arg1.flat_key = arg_key1_id;
  arg1.key = arg_key1_id;
  arg1.value = Variadic::Pointer(kValue1);
  storage.mutable_args()->AddArgSet({arg0, arg1}, 0, 2);
  storage.mutable_nestable_slices()->set_arg_set_id(0, 1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["arg0"].asString(), "0x1");
  EXPECT_EQ(event["args"]["arg1"].asString(), "0xffffffffffffffff");
}

TEST(ExportJsonTest, StorageWithObjectListArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  int kValues[] = {123, 234};

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(0);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(0, 0, utid, RefType::kRefUtid,
                                              cat_id, name_id, 0, 0, 0);

  StringId arg_flat_key_id = storage.InternString(base::StringView("a.b"));
  StringId arg_key0_id = storage.InternString(base::StringView("a[0].b"));
  StringId arg_key1_id = storage.InternString(base::StringView("a[1].b"));
  TraceStorage::Args::Arg arg0;
  arg0.flat_key = arg_flat_key_id;
  arg0.key = arg_key0_id;
  arg0.value = Variadic::Integer(kValues[0]);
  TraceStorage::Args::Arg arg1;
  arg1.flat_key = arg_flat_key_id;
  arg1.key = arg_key1_id;
  arg1.value = Variadic::Integer(kValues[1]);
  storage.mutable_args()->AddArgSet({arg0, arg1}, 0, 2);
  storage.mutable_nestable_slices()->set_arg_set_id(0, 1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["a"].size(), 2u);
  EXPECT_EQ(event["args"]["a"][0]["b"].asInt(), kValues[0]);
  EXPECT_EQ(event["args"]["a"][1]["b"].asInt(), kValues[1]);
}

TEST(ExportJsonTest, StorageWithNestedListArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  int kValues[] = {123, 234};

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(0);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(0, 0, utid, RefType::kRefUtid,
                                              cat_id, name_id, 0, 0, 0);

  StringId arg_flat_key_id = storage.InternString(base::StringView("a"));
  StringId arg_key0_id = storage.InternString(base::StringView("a[0][0]"));
  StringId arg_key1_id = storage.InternString(base::StringView("a[0][1]"));
  TraceStorage::Args::Arg arg0;
  arg0.flat_key = arg_flat_key_id;
  arg0.key = arg_key0_id;
  arg0.value = Variadic::Integer(kValues[0]);
  TraceStorage::Args::Arg arg1;
  arg1.flat_key = arg_flat_key_id;
  arg1.key = arg_key1_id;
  arg1.value = Variadic::Integer(kValues[1]);
  storage.mutable_args()->AddArgSet({arg0, arg1}, 0, 2);
  storage.mutable_nestable_slices()->set_arg_set_id(0, 1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["a"].size(), 1u);
  EXPECT_EQ(event["args"]["a"][0].size(), 2u);
  EXPECT_EQ(event["args"]["a"][0][0].asInt(), kValues[0]);
  EXPECT_EQ(event["args"]["a"][0][1].asInt(), kValues[1]);
}

TEST(ExportJsonTest, StorageWithLegacyJsonArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";

  TraceStorage storage;
  UniqueTid utid = storage.AddEmptyThread(0);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(0, 0, utid, RefType::kRefUtid,
                                              cat_id, name_id, 0, 0, 0);

  StringId arg_key_id = storage.InternString(base::StringView("a"));
  StringId arg_value_id = storage.InternString(base::StringView("{\"b\":123}"));
  TraceStorage::Args::Arg arg;
  arg.flat_key = arg_key_id;
  arg.key = arg_key_id;
  arg.value = Variadic::Json(arg_value_id);
  storage.mutable_args()->AddArgSet({arg}, 0, 1);
  storage.mutable_nestable_slices()->set_arg_set_id(0, 1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["a"]["b"].asInt(), 123);
}

TEST(ExportJsonTest, InstantEvent) {
  const int64_t kTimestamp = 10000000;
  const char* kCategory = "cat";
  const char* kName = "name";

  TraceStorage storage;
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  storage.mutable_nestable_slices()->AddSlice(
      kTimestamp, 0, 0, RefType::kRefNoRef, cat_id, name_id, 0, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "i");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["s"].asString(), "g");
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
}

TEST(ExportJsonTest, AsyncEvent) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = 100000;
  const int64_t kProcessID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";
  const char* kArgName = "arg_name";
  const int kArgValue = 123;

  TraceStorage storage;
  UniquePid upid = storage.AddEmptyProcess(kProcessID);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  TrackId track_id = storage.mutable_tracks()->AddTrack(name_id);
  storage.mutable_virtual_tracks()->AddVirtualTrack(
      track_id, VirtualTrackScope::kProcess, upid);
  storage.mutable_nestable_slices()->AddSlice(kTimestamp, kDuration, track_id,
                                              RefType::kRefTrack, cat_id,
                                              name_id, 0, 0, 0);
  StringId arg_key_id = storage.InternString(base::StringView(kArgName));
  TraceStorage::Args::Arg arg;
  arg.flat_key = arg_key_id;
  arg.key = arg_key_id;
  arg.value = Variadic::Integer(kArgValue);
  storage.mutable_args()->AddArgSet({arg}, 0, 1);
  storage.mutable_nestable_slices()->set_arg_set_id(0, 1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 2u);

  Json::Value begin_event = result["traceEvents"][0];
  EXPECT_EQ(begin_event["ph"].asString(), "b");
  EXPECT_EQ(begin_event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(begin_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(begin_event["id2"]["local"].asString(), "0x0");
  EXPECT_EQ(begin_event["cat"].asString(), kCategory);
  EXPECT_EQ(begin_event["name"].asString(), kName);
  EXPECT_EQ(begin_event["args"][kArgName].asInt(), kArgValue);
  EXPECT_FALSE(begin_event.isMember("tts"));
  EXPECT_FALSE(begin_event.isMember("use_async_tts"));

  Json::Value end_event = result["traceEvents"][1];
  EXPECT_EQ(end_event["ph"].asString(), "e");
  EXPECT_EQ(end_event["ts"].asInt64(), (kTimestamp + kDuration) / 1000);
  EXPECT_EQ(end_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(end_event["id2"]["local"].asString(), "0x0");
  EXPECT_EQ(end_event["cat"].asString(), kCategory);
  EXPECT_EQ(end_event["name"].asString(), kName);
  EXPECT_FALSE(end_event.isMember("args"));
  EXPECT_FALSE(end_event.isMember("tts"));
  EXPECT_FALSE(end_event.isMember("use_async_tts"));
}

TEST(ExportJsonTest, AsyncEventWithThreadTimestamp) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = 100000;
  const int64_t kThreadTimestamp = 10000001;
  const int64_t kThreadDuration = 99998;
  const int64_t kProcessID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  TraceStorage storage;
  UniquePid upid = storage.AddEmptyProcess(kProcessID);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  TrackId track_id = storage.mutable_tracks()->AddTrack(name_id);
  storage.mutable_virtual_tracks()->AddVirtualTrack(
      track_id, VirtualTrackScope::kProcess, upid);
  auto slice_id = storage.mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, track_id, RefType::kRefTrack, cat_id, name_id, 0,
      0, 0);
  storage.mutable_virtual_track_slices()->AddVirtualTrackSlice(
      slice_id, kThreadTimestamp, kThreadDuration, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 2u);

  Json::Value begin_event = result["traceEvents"][0];
  EXPECT_EQ(begin_event["ph"].asString(), "b");
  EXPECT_EQ(begin_event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(begin_event["tts"].asInt64(), kThreadTimestamp / 1000);
  EXPECT_EQ(begin_event["use_async_tts"].asInt(), 1);
  EXPECT_EQ(begin_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(begin_event["id2"]["local"].asString(), "0x0");
  EXPECT_EQ(begin_event["cat"].asString(), kCategory);
  EXPECT_EQ(begin_event["name"].asString(), kName);

  Json::Value end_event = result["traceEvents"][1];
  EXPECT_EQ(end_event["ph"].asString(), "e");
  EXPECT_EQ(end_event["ts"].asInt64(), (kTimestamp + kDuration) / 1000);
  EXPECT_EQ(end_event["tts"].asInt64(),
            (kThreadTimestamp + kThreadDuration) / 1000);
  EXPECT_EQ(end_event["use_async_tts"].asInt(), 1);
  EXPECT_EQ(end_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(end_event["id2"]["local"].asString(), "0x0");
  EXPECT_EQ(end_event["cat"].asString(), kCategory);
  EXPECT_EQ(end_event["name"].asString(), kName);
}

TEST(ExportJsonTest, UnfinishedAsyncEvent) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = -1;
  const int64_t kThreadTimestamp = 10000001;
  const int64_t kThreadDuration = -1;
  const int64_t kProcessID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  TraceStorage storage;
  UniquePid upid = storage.AddEmptyProcess(kProcessID);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  TrackId track_id = storage.mutable_tracks()->AddTrack(name_id);
  storage.mutable_virtual_tracks()->AddVirtualTrack(
      track_id, VirtualTrackScope::kProcess, upid);
  auto slice_id = storage.mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, track_id, RefType::kRefTrack, cat_id, name_id, 0,
      0, 0);
  storage.mutable_virtual_track_slices()->AddVirtualTrackSlice(
      slice_id, kThreadTimestamp, kThreadDuration, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value begin_event = result["traceEvents"][0];
  EXPECT_EQ(begin_event["ph"].asString(), "b");
  EXPECT_EQ(begin_event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(begin_event["tts"].asInt64(), kThreadTimestamp / 1000);
  EXPECT_EQ(begin_event["use_async_tts"].asInt(), 1);
  EXPECT_EQ(begin_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(begin_event["id2"]["local"].asString(), "0x0");
  EXPECT_EQ(begin_event["cat"].asString(), kCategory);
  EXPECT_EQ(begin_event["name"].asString(), kName);
}

TEST(ExportJsonTest, AsyncInstantEvent) {
  const int64_t kTimestamp = 10000000;
  const int64_t kProcessID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";
  const char* kArgName = "arg_name";
  const int kArgValue = 123;

  TraceStorage storage;
  UniquePid upid = storage.AddEmptyProcess(kProcessID);
  StringId cat_id = storage.InternString(base::StringView(kCategory));
  StringId name_id = storage.InternString(base::StringView(kName));
  TrackId track_id = storage.mutable_tracks()->AddTrack(name_id);
  storage.mutable_virtual_tracks()->AddVirtualTrack(
      track_id, VirtualTrackScope::kProcess, upid);
  storage.mutable_nestable_slices()->AddSlice(
      kTimestamp, 0, track_id, RefType::kRefTrack, cat_id, name_id, 0, 0, 0);
  StringId arg_key_id = storage.InternString(base::StringView("arg_name"));
  TraceStorage::Args::Arg arg;
  arg.flat_key = arg_key_id;
  arg.key = arg_key_id;
  arg.value = Variadic::Integer(kArgValue);
  storage.mutable_args()->AddArgSet({arg}, 0, 1);
  storage.mutable_nestable_slices()->set_arg_set_id(0, 1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(&storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "n");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(event["id2"]["local"].asString(), "0x0");
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"][kArgName].asInt(), kArgValue);
}

TEST(ExportJsonTest, RawEvent) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = 10000;
  const int64_t kThreadTimestamp = 20000000;
  const int64_t kThreadDuration = 20000;
  const int64_t kThreadInstructionCount = 30000000;
  const int64_t kThreadInstructionDelta = 30000;
  const int64_t kProcessID = 100;
  const int64_t kThreadID = 200;
  const char* kCategory = "cat";
  const char* kName = "name";
  const char* kPhase = "?";
  const uint64_t kGlobalId = 0xaaffaaffaaffaaff;
  const char* kIdScope = "my_id";
  const uint64_t kBindId = 0xaa00aa00aa00aa00;
  const char* kFlowDirection = "inout";
  const char* kArgName = "arg_name";
  const int kArgValue = 123;

  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  TraceStorage* storage = context.storage.get();

  UniquePid upid = storage->AddEmptyProcess(kProcessID);
  UniqueTid utid = storage->AddEmptyThread(kThreadID);
  storage->GetMutableThread(utid)->upid = upid;

  RowId row_id = storage->mutable_raw_events()->AddRawEvent(
      kTimestamp, storage->InternString("track_event.legacy_event"), /*cpu=*/0,
      utid);

  ArgsTracker args(&context);
  auto add_arg = [&](const char* key, Variadic value) {
    StringId key_id = storage->InternString(key);
    args.AddArg(row_id, key_id, key_id, value);
  };

  StringId cat_id = storage->InternString(base::StringView(kCategory));
  add_arg("legacy_event.category", Variadic::String(cat_id));
  StringId name_id = storage->InternString(base::StringView(kName));
  add_arg("legacy_event.name", Variadic::String(name_id));
  StringId phase_id = storage->InternString(base::StringView(kPhase));
  add_arg("legacy_event.phase", Variadic::String(phase_id));

  add_arg("legacy_event.duration_ns", Variadic::Integer(kDuration));
  add_arg("legacy_event.thread_timestamp_ns",
          Variadic::Integer(kThreadTimestamp));
  add_arg("legacy_event.thread_duration_ns",
          Variadic::Integer(kThreadDuration));
  add_arg("legacy_event.thread_instruction_count",
          Variadic::Integer(kThreadInstructionCount));
  add_arg("legacy_event.thread_instruction_delta",
          Variadic::Integer(kThreadInstructionDelta));
  add_arg("legacy_event.use_async_tts", Variadic::Boolean(true));
  add_arg("legacy_event.global_id", Variadic::UnsignedInteger(kGlobalId));
  StringId scope_id = storage->InternString(base::StringView(kIdScope));
  add_arg("legacy_event.id_scope", Variadic::String(scope_id));
  add_arg("legacy_event.bind_id", Variadic::UnsignedInteger(kBindId));
  add_arg("legacy_event.bind_to_enclosing", Variadic::Boolean(true));
  StringId flow_direction_id = storage->InternString(kFlowDirection);
  add_arg("legacy_event.flow_direction", Variadic::String(flow_direction_id));

  add_arg(kArgName, Variadic::Integer(kArgValue));

  args.Flush();

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), kPhase);
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["dur"].asInt64(), kDuration / 1000);
  EXPECT_EQ(event["tts"].asInt64(), kThreadTimestamp / 1000);
  EXPECT_EQ(event["tdur"].asInt64(), kThreadDuration / 1000);
  EXPECT_EQ(event["ticount"].asInt64(), kThreadInstructionCount);
  EXPECT_EQ(event["tidelta"].asInt64(), kThreadInstructionDelta);
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["use_async_tts"].asInt(), 1);
  EXPECT_EQ(event["id2"]["global"].asString(), "0xaaffaaffaaffaaff");
  EXPECT_EQ(event["scope"].asString(), kIdScope);
  EXPECT_EQ(event["bind_id"].asString(), "0xaa00aa00aa00aa00");
  EXPECT_EQ(event["bp"].asString(), "e");
  EXPECT_EQ(event["flow_in"].asBool(), true);
  EXPECT_EQ(event["flow_out"].asBool(), true);
  EXPECT_EQ(event["args"][kArgName].asInt(), kArgValue);
}

TEST(ExportJsonTest, LegacyRawEvents) {
  const char* kLegacyFtraceData = "some \"data\"\nsome :data:";
  const char* kLegacyJsonData = "{\"user\": 1}";

  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  TraceStorage* storage = context.storage.get();

  RowId row_id = storage->mutable_raw_events()->AddRawEvent(
      0, storage->InternString("chrome_event.legacy_system_trace"), 0, 0);

  ArgsTracker args(&context);
  StringId data_id = storage->InternString("data");
  StringId ftrace_data_id = storage->InternString(kLegacyFtraceData);
  args.AddArg(row_id, data_id, data_id, Variadic::String(ftrace_data_id));

  row_id = storage->mutable_raw_events()->AddRawEvent(
      0, storage->InternString("chrome_event.legacy_user_trace"), 0, 0);
  StringId json_data_id = storage->InternString(kLegacyJsonData);
  args.AddArg(row_id, data_id, data_id, Variadic::Json(json_data_id));

  args.Flush();

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  int code = ExportJson(storage, output);

  EXPECT_EQ(code, kResultOk);

  Json::Reader reader;
  Json::Value result;
  EXPECT_TRUE(reader.parse(ReadFile(output), result));

  EXPECT_EQ(result["traceEvents"].size(), 1u);
  Json::Value user_event = result["traceEvents"][0];
  EXPECT_EQ(user_event["user"].asInt(), 1);
  EXPECT_EQ(result["systemTraceEvents"].asString(), kLegacyFtraceData);
}

}  // namespace
}  // namespace json
}  // namespace trace_processor
}  // namespace perfetto
