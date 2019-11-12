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

#include "perfetto/ext/trace_processor/export_json.h"
#include "src/trace_processor/export_json.h"

#include <string.h>

#include <limits>

#include <json/reader.h>
#include <json/value.h>

#include "perfetto/ext/base/temp_file.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/track_tracker.h"

#include "test/gtest_and_gmock.h"

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

class StringOutputWriter : public OutputWriter {
 public:
  StringOutputWriter() { str_.reserve(1024); }
  ~StringOutputWriter() override {}

  util::Status AppendString(const std::string& str) override {
    str_ += str;
    return util::OkStatus();
  }

  std::string TakeStr() { return std::move(str_); }

 private:
  std::string str_;
};

class ExportJsonTest : public ::testing::Test {
 public:
  ExportJsonTest() {
    context_.args_tracker.reset(new ArgsTracker(&context_));
    context_.storage.reset(new TraceStorage());
    context_.track_tracker.reset(new TrackTracker(&context_));
  }

  std::string ToJson(ArgumentFilterPredicate argument_filter = nullptr,
                     MetadataFilterPredicate metadata_filter = nullptr,
                     LabelFilterPredicate label_filter = nullptr) {
    StringOutputWriter writer;
    util::Status status =
        ExportJson(context_.storage.get(), &writer, argument_filter,
                   metadata_filter, label_filter);
    EXPECT_TRUE(status.ok());
    return writer.TakeStr();
  }

  Json::Value ToJsonValue(const std::string& json) {
    Json::Reader reader;
    Json::Value result;
    EXPECT_TRUE(reader.parse(json, result));
    return result;
  }

 protected:
  TraceProcessorContext context_;
};

TEST_F(ExportJsonTest, EmptyStorage) {
  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 0u);
}

TEST_F(ExportJsonTest, StorageWithOneSlice) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = 10000;
  const int64_t kThreadTimestamp = 20000000;
  const int64_t kThreadDuration = 20000;
  const int64_t kThreadInstructionCount = 30000000;
  const int64_t kThreadInstructionDelta = 30000;
  const int64_t kThreadID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  UniqueTid utid = context_.storage->AddEmptyThread(kThreadID);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, track, utid, RefType::kRefUtid, cat_id, name_id, 0,
      0, 0);
  context_.storage->mutable_thread_slices()->AddThreadSlice(
      0, kThreadTimestamp, kThreadDuration, kThreadInstructionCount,
      kThreadInstructionDelta);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
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
  EXPECT_TRUE(event["args"].isObject());
  EXPECT_EQ(event["args"].size(), 0u);
}

TEST_F(ExportJsonTest, StorageWithOneUnfinishedSlice) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = -1;
  const int64_t kThreadTimestamp = 20000000;
  const int64_t kThreadDuration = -1;
  const int64_t kThreadInstructionCount = 30000000;
  const int64_t kThreadInstructionDelta = -1;
  const int64_t kThreadID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  UniqueTid utid = context_.storage->AddEmptyThread(kThreadID);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, track, utid, RefType::kRefUtid, cat_id, name_id, 0,
      0, 0);
  context_.storage->mutable_thread_slices()->AddThreadSlice(
      0, kThreadTimestamp, kThreadDuration, kThreadInstructionCount,
      kThreadInstructionDelta);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
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
  EXPECT_TRUE(event["args"].isObject());
  EXPECT_EQ(event["args"].size(), 0u);
}

TEST_F(ExportJsonTest, StorageWithThreadName) {
  const int64_t kThreadID = 100;
  const char* kName = "thread";

  UniqueTid utid = context_.storage->AddEmptyThread(kThreadID);
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->GetMutableThread(utid)->name_id = name_id;

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "M");
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["name"].asString(), "thread_name");
  EXPECT_EQ(event["args"]["name"].asString(), kName);
}

TEST_F(ExportJsonTest, WrongTrackTypeIgnored) {
  constexpr int64_t kCookie = 22;
  TrackId track = context_.track_tracker->InternAndroidAsyncTrack(
      /*name=*/0, /*upid=*/0, kCookie);
  context_.args_tracker->Flush();  // Flush track args.

  StringId cat_id = context_.storage->InternString("cat");
  StringId name_id = context_.storage->InternString("name");
  context_.storage->mutable_nestable_slices()->AddSlice(
      0, 0, track, track, RefType::kRefTrack, cat_id, name_id, 0, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 0u);
}

TEST_F(ExportJsonTest, StorageWithMetadata) {
  const char* kDescription = "description";
  const char* kBenchmarkName = "benchmark name";
  const char* kStoryName = "story name";
  const char* kStoryTag1 = "tag1";
  const char* kStoryTag2 = "tag2";
  const int64_t kBenchmarkStart = 1000000;
  const int64_t kStoryStart = 2000000;
  const bool kHadFailures = true;

  StringId desc_id =
      context_.storage->InternString(base::StringView(kDescription));
  Variadic description = Variadic::String(desc_id);
  context_.storage->SetMetadata(metadata::benchmark_description, description);

  StringId benchmark_name_id =
      context_.storage->InternString(base::StringView(kBenchmarkName));
  Variadic benchmark_name = Variadic::String(benchmark_name_id);
  context_.storage->SetMetadata(metadata::benchmark_name, benchmark_name);

  StringId story_name_id =
      context_.storage->InternString(base::StringView(kStoryName));
  Variadic story_name = Variadic::String(story_name_id);
  context_.storage->SetMetadata(metadata::benchmark_story_name, story_name);

  StringId tag1_id =
      context_.storage->InternString(base::StringView(kStoryTag1));
  StringId tag2_id =
      context_.storage->InternString(base::StringView(kStoryTag2));
  Variadic tag1 = Variadic::String(tag1_id);
  Variadic tag2 = Variadic::String(tag2_id);
  context_.storage->AppendMetadata(metadata::benchmark_story_tags, tag1);
  context_.storage->AppendMetadata(metadata::benchmark_story_tags, tag2);

  Variadic benchmark_start = Variadic::Integer(kBenchmarkStart);
  context_.storage->SetMetadata(metadata::benchmark_start_time_us,
                                benchmark_start);

  Variadic story_start = Variadic::Integer(kStoryStart);
  context_.storage->SetMetadata(metadata::benchmark_story_run_time_us,
                                story_start);

  Variadic had_failures = Variadic::Integer(kHadFailures);
  context_.storage->SetMetadata(metadata::benchmark_had_failures, had_failures);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));

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

TEST_F(ExportJsonTest, StorageWithStats) {
  int64_t kProducers = 10;
  int64_t kBufferSize0 = 1000;
  int64_t kBufferSize1 = 2000;

  context_.storage->SetStats(stats::traced_producers_connected, kProducers);
  context_.storage->SetIndexedStats(stats::traced_buf_buffer_size, 0,
                                    kBufferSize0);
  context_.storage->SetIndexedStats(stats::traced_buf_buffer_size, 1,
                                    kBufferSize1);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);
  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));

  EXPECT_TRUE(result.isMember("metadata"));
  EXPECT_TRUE(result["metadata"].isMember("perfetto_trace_stats"));
  Json::Value stats = result["metadata"]["perfetto_trace_stats"];

  EXPECT_EQ(stats["producers_connected"].asInt(), kProducers);
  EXPECT_EQ(stats["buffer_stats"].size(), 2u);
  EXPECT_EQ(stats["buffer_stats"][0]["buffer_size"].asInt(), kBufferSize0);
  EXPECT_EQ(stats["buffer_stats"][1]["buffer_size"].asInt(), kBufferSize1);
}

TEST_F(ExportJsonTest, StorageWithChromeMetadata) {
  const char* kName1 = "name1";
  const char* kName2 = "name2";
  const char* kValue1 = "value1";
  const int kValue2 = 222;

  TraceStorage* storage = context_.storage.get();

  RowId row_id = storage->mutable_raw_events()->AddRawEvent(
      0, storage->InternString("chrome_event.metadata"), 0, 0);

  StringId name1_id = storage->InternString(base::StringView(kName1));
  StringId name2_id = storage->InternString(base::StringView(kName2));
  StringId value1_id = storage->InternString(base::StringView(kValue1));
  context_.args_tracker->AddArg(row_id, name1_id, name1_id,
                                Variadic::String(value1_id));
  context_.args_tracker->AddArg(row_id, name2_id, name2_id,
                                Variadic::Integer(kValue2));
  context_.args_tracker->Flush();

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(storage, output);
  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));

  EXPECT_TRUE(result.isMember("metadata"));
  Json::Value metadata = result["metadata"];

  EXPECT_EQ(metadata[kName1].asString(), kValue1);
  EXPECT_EQ(metadata[kName2].asInt(), kValue2);
}

TEST_F(ExportJsonTest, StorageWithArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  const char* kSrc = "source_file.cc";

  UniqueTid utid = context_.storage->AddEmptyThread(0);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      0, 0, track, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);

  StringId arg_key_id = context_.storage->InternString(
      base::StringView("task.posted_from.file_name"));
  StringId arg_value_id =
      context_.storage->InternString(base::StringView(kSrc));
  TraceStorage::Args::Arg arg;
  arg.flat_key = arg_key_id;
  arg.key = arg_key_id;
  arg.value = Variadic::String(arg_value_id);
  ArgSetId args = context_.storage->mutable_args()->AddArgSet({arg}, 0, 1);
  context_.storage->mutable_nestable_slices()->set_arg_set_id(0, args);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["src"].asString(), kSrc);
}

TEST_F(ExportJsonTest, StorageWithSliceAndFlowEventArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  const uint64_t kBindId = 0xaa00aa00aa00aa00;
  const char* kFlowDirection = "inout";
  const char* kArgName = "arg_name";
  const int kArgValue = 123;

  TraceStorage* storage = context_.storage.get();

  UniqueTid utid = storage->AddEmptyThread(0);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = storage->InternString(base::StringView(kCategory));
  StringId name_id = storage->InternString(base::StringView(kName));
  RowId row_id = TraceStorage::CreateRowId(
      kNestableSlices,
      storage->mutable_nestable_slices()->AddSlice(
          0, 0, track, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0));

  auto add_arg = [&](const char* key, Variadic value) {
    StringId key_id = storage->InternString(key);
    context_.args_tracker->AddArg(row_id, key_id, key_id, value);
  };

  add_arg("legacy_event.bind_id", Variadic::UnsignedInteger(kBindId));
  add_arg("legacy_event.bind_to_enclosing", Variadic::Boolean(true));
  StringId flow_direction_id = storage->InternString(kFlowDirection);
  add_arg("legacy_event.flow_direction", Variadic::String(flow_direction_id));

  add_arg(kArgName, Variadic::Integer(kArgValue));

  context_.args_tracker->Flush();

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(storage, output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["bind_id"].asString(), "0xaa00aa00aa00aa00");
  EXPECT_EQ(event["bp"].asString(), "e");
  EXPECT_EQ(event["flow_in"].asBool(), true);
  EXPECT_EQ(event["flow_out"].asBool(), true);
  EXPECT_EQ(event["args"][kArgName].asInt(), kArgValue);
  EXPECT_FALSE(event["args"].isMember("legacy_event"));
}

TEST_F(ExportJsonTest, StorageWithListArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  double kValues[] = {1.234, 2.345};

  UniqueTid utid = context_.storage->AddEmptyThread(0);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      0, 0, track, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);

  StringId arg_flat_key_id = context_.storage->InternString(
      base::StringView("debug.draw_duration_ms"));
  StringId arg_key0_id = context_.storage->InternString(
      base::StringView("debug.draw_duration_ms[0]"));
  StringId arg_key1_id = context_.storage->InternString(
      base::StringView("debug.draw_duration_ms[1]"));
  TraceStorage::Args::Arg arg0;
  arg0.flat_key = arg_flat_key_id;
  arg0.key = arg_key0_id;
  arg0.value = Variadic::Real(kValues[0]);
  TraceStorage::Args::Arg arg1;
  arg1.flat_key = arg_flat_key_id;
  arg1.key = arg_key1_id;
  arg1.value = Variadic::Real(kValues[1]);
  ArgSetId args =
      context_.storage->mutable_args()->AddArgSet({arg0, arg1}, 0, 2);
  context_.storage->mutable_nestable_slices()->set_arg_set_id(0, args);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["draw_duration_ms"].size(), 2u);
  EXPECT_DOUBLE_EQ(event["args"]["draw_duration_ms"][0].asDouble(), kValues[0]);
  EXPECT_DOUBLE_EQ(event["args"]["draw_duration_ms"][1].asDouble(), kValues[1]);
}

TEST_F(ExportJsonTest, StorageWithMultiplePointerArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  uint64_t kValue0 = 1;
  uint64_t kValue1 = std::numeric_limits<uint64_t>::max();

  UniqueTid utid = context_.storage->AddEmptyThread(0);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      0, 0, track, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);

  StringId arg_key0_id =
      context_.storage->InternString(base::StringView("arg0"));
  StringId arg_key1_id =
      context_.storage->InternString(base::StringView("arg1"));
  TraceStorage::Args::Arg arg0;
  arg0.flat_key = arg_key0_id;
  arg0.key = arg_key0_id;
  arg0.value = Variadic::Pointer(kValue0);
  TraceStorage::Args::Arg arg1;
  arg1.flat_key = arg_key1_id;
  arg1.key = arg_key1_id;
  arg1.value = Variadic::Pointer(kValue1);
  ArgSetId args =
      context_.storage->mutable_args()->AddArgSet({arg0, arg1}, 0, 2);
  context_.storage->mutable_nestable_slices()->set_arg_set_id(0, args);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["arg0"].asString(), "0x1");
  EXPECT_EQ(event["args"]["arg1"].asString(), "0xffffffffffffffff");
}

TEST_F(ExportJsonTest, StorageWithObjectListArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  int kValues[] = {123, 234};

  UniqueTid utid = context_.storage->AddEmptyThread(0);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      0, 0, track, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);

  StringId arg_flat_key_id =
      context_.storage->InternString(base::StringView("a.b"));
  StringId arg_key0_id =
      context_.storage->InternString(base::StringView("a[0].b"));
  StringId arg_key1_id =
      context_.storage->InternString(base::StringView("a[1].b"));
  TraceStorage::Args::Arg arg0;
  arg0.flat_key = arg_flat_key_id;
  arg0.key = arg_key0_id;
  arg0.value = Variadic::Integer(kValues[0]);
  TraceStorage::Args::Arg arg1;
  arg1.flat_key = arg_flat_key_id;
  arg1.key = arg_key1_id;
  arg1.value = Variadic::Integer(kValues[1]);
  ArgSetId args =
      context_.storage->mutable_args()->AddArgSet({arg0, arg1}, 0, 2);
  context_.storage->mutable_nestable_slices()->set_arg_set_id(0, args);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["a"].size(), 2u);
  EXPECT_EQ(event["args"]["a"][0]["b"].asInt(), kValues[0]);
  EXPECT_EQ(event["args"]["a"][1]["b"].asInt(), kValues[1]);
}

TEST_F(ExportJsonTest, StorageWithNestedListArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";
  int kValues[] = {123, 234};

  UniqueTid utid = context_.storage->AddEmptyThread(0);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      0, 0, track, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);

  StringId arg_flat_key_id =
      context_.storage->InternString(base::StringView("a"));
  StringId arg_key0_id =
      context_.storage->InternString(base::StringView("a[0][0]"));
  StringId arg_key1_id =
      context_.storage->InternString(base::StringView("a[0][1]"));
  TraceStorage::Args::Arg arg0;
  arg0.flat_key = arg_flat_key_id;
  arg0.key = arg_key0_id;
  arg0.value = Variadic::Integer(kValues[0]);
  TraceStorage::Args::Arg arg1;
  arg1.flat_key = arg_flat_key_id;
  arg1.key = arg_key1_id;
  arg1.value = Variadic::Integer(kValues[1]);
  ArgSetId args =
      context_.storage->mutable_args()->AddArgSet({arg0, arg1}, 0, 2);
  context_.storage->mutable_nestable_slices()->set_arg_set_id(0, args);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["a"].size(), 1u);
  EXPECT_EQ(event["args"]["a"][0].size(), 2u);
  EXPECT_EQ(event["args"]["a"][0][0].asInt(), kValues[0]);
  EXPECT_EQ(event["args"]["a"][0][1].asInt(), kValues[1]);
}

TEST_F(ExportJsonTest, StorageWithLegacyJsonArgs) {
  const char* kCategory = "cat";
  const char* kName = "name";

  UniqueTid utid = context_.storage->AddEmptyThread(0);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      0, 0, track, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);

  StringId arg_key_id = context_.storage->InternString(base::StringView("a"));
  StringId arg_value_id =
      context_.storage->InternString(base::StringView("{\"b\":123}"));
  TraceStorage::Args::Arg arg;
  arg.flat_key = arg_key_id;
  arg.key = arg_key_id;
  arg.value = Variadic::Json(arg_value_id);
  ArgSetId args = context_.storage->mutable_args()->AddArgSet({arg}, 0, 1);
  context_.storage->mutable_nestable_slices()->set_arg_set_id(0, args);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"]["a"]["b"].asInt(), 123);
}

TEST_F(ExportJsonTest, InstantEvent) {
  const int64_t kTimestamp = 10000000;
  const char* kCategory = "cat";
  const char* kName = "name";

  TrackId track =
      context_.track_tracker->GetOrCreateLegacyChromeGlobalInstantTrack();
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp, 0, track, 0, RefType::kRefNoRef, cat_id, name_id, 0, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "I");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["s"].asString(), "g");
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
}

TEST_F(ExportJsonTest, InstantEventOnThread) {
  const int64_t kTimestamp = 10000000;
  const int64_t kThreadID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  UniqueTid utid = context_.storage->AddEmptyThread(kThreadID);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));
  context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp, 0, track, utid, RefType::kRefUtid, cat_id, name_id, 0, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["ph"].asString(), "I");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["s"].asString(), "t");
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
}

TEST_F(ExportJsonTest, AsyncEvent) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = 100000;
  const int64_t kProcessID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";
  const char* kArgName = "arg_name";
  const int kArgValue = 123;

  UniquePid upid = context_.storage->AddEmptyProcess(kProcessID);
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));

  constexpr int64_t kSourceId = 235;
  TrackId track = context_.track_tracker->InternLegacyChromeAsyncTrack(
      name_id, upid, kSourceId, /*source_id_is_process_scoped=*/true,
      /*source_scope=*/0);
  context_.args_tracker->Flush();  // Flush track args.

  context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, track, track, RefType::kRefTrack, cat_id, name_id,
      0, 0, 0);
  StringId arg_key_id =
      context_.storage->InternString(base::StringView(kArgName));
  TraceStorage::Args::Arg arg;
  arg.flat_key = arg_key_id;
  arg.key = arg_key_id;
  arg.value = Variadic::Integer(kArgValue);
  ArgSetId args = context_.storage->mutable_args()->AddArgSet({arg}, 0, 1);
  context_.storage->mutable_nestable_slices()->set_arg_set_id(0, args);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 2u);

  Json::Value begin_event = result["traceEvents"][0];
  EXPECT_EQ(begin_event["ph"].asString(), "b");
  EXPECT_EQ(begin_event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(begin_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(begin_event["id2"]["local"].asString(), "0xeb");
  EXPECT_EQ(begin_event["cat"].asString(), kCategory);
  EXPECT_EQ(begin_event["name"].asString(), kName);
  EXPECT_EQ(begin_event["args"][kArgName].asInt(), kArgValue);
  EXPECT_FALSE(begin_event.isMember("tts"));
  EXPECT_FALSE(begin_event.isMember("use_async_tts"));

  Json::Value end_event = result["traceEvents"][1];
  EXPECT_EQ(end_event["ph"].asString(), "e");
  EXPECT_EQ(end_event["ts"].asInt64(), (kTimestamp + kDuration) / 1000);
  EXPECT_EQ(end_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(end_event["id2"]["local"].asString(), "0xeb");
  EXPECT_EQ(end_event["cat"].asString(), kCategory);
  EXPECT_EQ(end_event["name"].asString(), kName);
  EXPECT_TRUE(end_event["args"].isObject());
  EXPECT_EQ(end_event["args"].size(), 0u);
  EXPECT_FALSE(end_event.isMember("tts"));
  EXPECT_FALSE(end_event.isMember("use_async_tts"));
}

TEST_F(ExportJsonTest, AsyncEventWithThreadTimestamp) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = 100000;
  const int64_t kThreadTimestamp = 10000001;
  const int64_t kThreadDuration = 99998;
  const int64_t kProcessID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  UniquePid upid = context_.storage->AddEmptyProcess(kProcessID);
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));

  constexpr int64_t kSourceId = 235;
  TrackId track = context_.track_tracker->InternLegacyChromeAsyncTrack(
      name_id, upid, kSourceId, /*source_id_is_process_scoped=*/true,
      /*source_scope=*/0);
  context_.args_tracker->Flush();  // Flush track args.

  auto slice_id = context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, track, track, RefType::kRefTrack, cat_id, name_id,
      0, 0, 0);
  context_.storage->mutable_virtual_track_slices()->AddVirtualTrackSlice(
      slice_id, kThreadTimestamp, kThreadDuration, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 2u);

  Json::Value begin_event = result["traceEvents"][0];
  EXPECT_EQ(begin_event["ph"].asString(), "b");
  EXPECT_EQ(begin_event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(begin_event["tts"].asInt64(), kThreadTimestamp / 1000);
  EXPECT_EQ(begin_event["use_async_tts"].asInt(), 1);
  EXPECT_EQ(begin_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(begin_event["id2"]["local"].asString(), "0xeb");
  EXPECT_EQ(begin_event["cat"].asString(), kCategory);
  EXPECT_EQ(begin_event["name"].asString(), kName);

  Json::Value end_event = result["traceEvents"][1];
  EXPECT_EQ(end_event["ph"].asString(), "e");
  EXPECT_EQ(end_event["ts"].asInt64(), (kTimestamp + kDuration) / 1000);
  EXPECT_EQ(end_event["tts"].asInt64(),
            (kThreadTimestamp + kThreadDuration) / 1000);
  EXPECT_EQ(end_event["use_async_tts"].asInt(), 1);
  EXPECT_EQ(end_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(end_event["id2"]["local"].asString(), "0xeb");
  EXPECT_EQ(end_event["cat"].asString(), kCategory);
  EXPECT_EQ(end_event["name"].asString(), kName);
}

TEST_F(ExportJsonTest, UnfinishedAsyncEvent) {
  const int64_t kTimestamp = 10000000;
  const int64_t kDuration = -1;
  const int64_t kThreadTimestamp = 10000001;
  const int64_t kThreadDuration = -1;
  const int64_t kProcessID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  UniquePid upid = context_.storage->AddEmptyProcess(kProcessID);
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));

  constexpr int64_t kSourceId = 235;
  TrackId track = context_.track_tracker->InternLegacyChromeAsyncTrack(
      name_id, upid, kSourceId, /*source_id_is_process_scoped=*/true,
      /*source_scope=*/0);
  context_.args_tracker->Flush();  // Flush track args.

  auto slice_id = context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp, kDuration, track, track, RefType::kRefTrack, cat_id, name_id,
      0, 0, 0);
  context_.storage->mutable_virtual_track_slices()->AddVirtualTrackSlice(
      slice_id, kThreadTimestamp, kThreadDuration, 0, 0);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value begin_event = result["traceEvents"][0];
  EXPECT_EQ(begin_event["ph"].asString(), "b");
  EXPECT_EQ(begin_event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(begin_event["tts"].asInt64(), kThreadTimestamp / 1000);
  EXPECT_EQ(begin_event["use_async_tts"].asInt(), 1);
  EXPECT_EQ(begin_event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(begin_event["id2"]["local"].asString(), "0xeb");
  EXPECT_EQ(begin_event["cat"].asString(), kCategory);
  EXPECT_EQ(begin_event["name"].asString(), kName);
}

TEST_F(ExportJsonTest, AsyncInstantEvent) {
  const int64_t kTimestamp = 10000000;
  const int64_t kProcessID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";
  const char* kArgName = "arg_name";
  const int kArgValue = 123;

  UniquePid upid = context_.storage->AddEmptyProcess(kProcessID);
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));

  constexpr int64_t kSourceId = 235;
  TrackId track = context_.track_tracker->InternLegacyChromeAsyncTrack(
      name_id, upid, kSourceId, /*source_id_is_process_scoped=*/true,
      /*source_scope=*/0);
  context_.args_tracker->Flush();  // Flush track args.

  context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp, 0, track, track, RefType::kRefTrack, cat_id, name_id, 0, 0,
      0);
  StringId arg_key_id =
      context_.storage->InternString(base::StringView("arg_name"));
  TraceStorage::Args::Arg arg;
  arg.flat_key = arg_key_id;
  arg.key = arg_key_id;
  arg.value = Variadic::Integer(kArgValue);
  ArgSetId args = context_.storage->mutable_args()->AddArgSet({arg}, 0, 1);
  context_.storage->mutable_nestable_slices()->set_arg_set_id(0, args);

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(context_.storage.get(), output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
  EXPECT_EQ(result["traceEvents"].size(), 1u);

  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "n");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["pid"].asInt64(), kProcessID);
  EXPECT_EQ(event["id2"]["local"].asString(), "0xeb");
  EXPECT_EQ(event["cat"].asString(), kCategory);
  EXPECT_EQ(event["name"].asString(), kName);
  EXPECT_EQ(event["args"][kArgName].asInt(), kArgValue);
}

TEST_F(ExportJsonTest, RawEvent) {
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

  TraceStorage* storage = context_.storage.get();

  UniquePid upid = storage->AddEmptyProcess(kProcessID);
  UniqueTid utid = storage->AddEmptyThread(kThreadID);
  storage->GetMutableThread(utid)->upid = upid;

  RowId row_id = storage->mutable_raw_events()->AddRawEvent(
      kTimestamp, storage->InternString("track_event.legacy_event"), /*cpu=*/0,
      utid);

  auto add_arg = [&](const char* key, Variadic value) {
    StringId key_id = storage->InternString(key);
    context_.args_tracker->AddArg(row_id, key_id, key_id, value);
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

  context_.args_tracker->Flush();

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(storage, output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));
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

TEST_F(ExportJsonTest, LegacyRawEvents) {
  const char* kLegacyFtraceData = "some \"data\"\nsome :data:";
  const char* kLegacyJsonData1 = "{\"us";
  const char* kLegacyJsonData2 = "er\": 1},{\"user\": 2}";

  TraceStorage* storage = context_.storage.get();

  RowId row_id = storage->mutable_raw_events()->AddRawEvent(
      0, storage->InternString("chrome_event.legacy_system_trace"), 0, 0);

  StringId data_id = storage->InternString("data");
  StringId ftrace_data_id = storage->InternString(kLegacyFtraceData);
  context_.args_tracker->AddArg(row_id, data_id, data_id,
                                Variadic::String(ftrace_data_id));

  row_id = storage->mutable_raw_events()->AddRawEvent(
      0, storage->InternString("chrome_event.legacy_user_trace"), 0, 0);
  StringId json_data1_id = storage->InternString(kLegacyJsonData1);
  context_.args_tracker->AddArg(row_id, data_id, data_id,
                                Variadic::String(json_data1_id));

  row_id = storage->mutable_raw_events()->AddRawEvent(
      0, storage->InternString("chrome_event.legacy_user_trace"), 0, 0);
  StringId json_data2_id = storage->InternString(kLegacyJsonData2);
  context_.args_tracker->AddArg(row_id, data_id, data_id,
                                Variadic::String(json_data2_id));

  context_.args_tracker->Flush();

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(storage, output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));

  EXPECT_EQ(result["traceEvents"].size(), 2u);
  EXPECT_EQ(result["traceEvents"][0]["user"].asInt(), 1);
  EXPECT_EQ(result["traceEvents"][1]["user"].asInt(), 2);
  EXPECT_EQ(result["systemTraceEvents"].asString(), kLegacyFtraceData);
}

TEST_F(ExportJsonTest, CpuProfileEvent) {
  const int64_t kProcessID = 100;
  const int64_t kThreadID = 200;
  const int64_t kTimestamp = 10000000;

  TraceStorage* storage = context_.storage.get();

  UniquePid upid = storage->AddEmptyProcess(kProcessID);
  UniqueTid utid = storage->AddEmptyThread(kThreadID);
  storage->GetMutableThread(utid)->upid = upid;

  uint32_t module_row_id_1 = storage->mutable_stack_profile_mappings()->Insert(
      {storage->InternString("foo_module_id"), 0, 0, 0, 0, 0,
       storage->InternString("foo_module_name")});

  uint32_t module_row_id_2 = storage->mutable_stack_profile_mappings()->Insert(
      {storage->InternString("bar_module_id"), 0, 0, 0, 0, 0,
       storage->InternString("bar_module_name")});

  // TODO(140860736): Once we support null values for
  // stack_profile_frame.symbol_set_id remove this hack
  storage->mutable_symbol_table()->Insert({0, 0, 0, 0});

  uint32_t frame_row_id_1 = storage->mutable_stack_profile_frames()->Insert(
      {/*name_id=*/0, module_row_id_1, 0x42});
  uint32_t symbol_set_id = storage->symbol_table().size();
  storage->mutable_symbol_table()->Insert(
      {symbol_set_id, storage->InternString("foo_func"),
       storage->InternString("foo_file"), 66});
  storage->mutable_stack_profile_frames()->SetSymbolSetId(
      static_cast<size_t>(frame_row_id_1), symbol_set_id);

  uint32_t frame_row_id_2 = storage->mutable_stack_profile_frames()->Insert(
      {/*name_id=*/0, module_row_id_2, 0x4242});
  symbol_set_id = storage->symbol_table().size();
  storage->mutable_symbol_table()->Insert(
      {symbol_set_id, storage->InternString("bar_func"),
       storage->InternString("bar_file"), 77});
  storage->mutable_stack_profile_frames()->SetSymbolSetId(
      static_cast<size_t>(frame_row_id_2), symbol_set_id);

  uint32_t frame_callsite_id_1 =
      storage->mutable_stack_profile_callsite_table()->Insert(
          {0, -1, frame_row_id_1});

  uint32_t frame_callsite_id_2 =
      storage->mutable_stack_profile_callsite_table()->Insert(
          {1, frame_callsite_id_1, frame_row_id_2});

  storage->mutable_cpu_profile_stack_samples()->Insert(
      {kTimestamp, frame_callsite_id_2, utid});

  base::TempFile temp_file = base::TempFile::Create();
  FILE* output = fopen(temp_file.path().c_str(), "w+");
  util::Status status = ExportJson(storage, output);

  EXPECT_TRUE(status.ok());

  Json::Value result = ToJsonValue(ReadFile(output));

  EXPECT_EQ(result["traceEvents"].size(), 1u);
  Json::Value event = result["traceEvents"][0];
  EXPECT_EQ(event["ph"].asString(), "n");
  EXPECT_EQ(event["id"].asString(), "0x1");
  EXPECT_EQ(event["ts"].asInt64(), kTimestamp / 1000);
  EXPECT_EQ(event["tid"].asUInt(), kThreadID);
  EXPECT_EQ(event["cat"].asString(), "disabled_by_default-cpu_profiler");
  EXPECT_EQ(event["name"].asString(), "StackCpuSampling");
  EXPECT_EQ(event["s"].asString(), "t");
  EXPECT_EQ(event["args"]["frames"].asString(),
            "foo_func - foo_module_name [foo_module_id]\nbar_func - "
            "bar_module_name [bar_module_id]\n");
}

TEST_F(ExportJsonTest, ArgumentFilter) {
  UniqueTid utid = context_.storage->AddEmptyThread(0);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.

  StringId cat_id = context_.storage->InternString(base::StringView("cat"));
  std::array<StringId, 3> name_ids{
      context_.storage->InternString(base::StringView("name1")),
      context_.storage->InternString(base::StringView("name2")),
      context_.storage->InternString(base::StringView("name3"))};
  StringId arg1_id = context_.storage->InternString(base::StringView("arg1"));
  StringId arg2_id = context_.storage->InternString(base::StringView("arg2"));
  StringId val_id = context_.storage->InternString(base::StringView("val"));

  std::array<RowId, 3> slice_ids;
  for (size_t i = 0; i < name_ids.size(); i++) {
    slice_ids[i] = TraceStorage::CreateRowId(
        kNestableSlices, context_.storage->mutable_nestable_slices()->AddSlice(
                             0, 0, track, utid, RefType::kRefUtid, cat_id,
                             name_ids[i], 0, 0, 0));
  }

  for (RowId row : slice_ids) {
    context_.args_tracker->AddArg(row, arg1_id, arg1_id, Variadic::Integer(5));
    context_.args_tracker->AddArg(row, arg2_id, arg2_id,
                                  Variadic::String(val_id));
  }
  context_.args_tracker->Flush();

  auto arg_filter = [](const char* category_group_name, const char* event_name,
                       ArgumentNameFilterPredicate* arg_name_filter) {
    EXPECT_TRUE(strcmp(category_group_name, "cat") == 0);
    if (strcmp(event_name, "name1") == 0) {
      // Filter all args for name1.
      return false;
    } else if (strcmp(event_name, "name2") == 0) {
      // Filter only the second arg for name2.
      *arg_name_filter = [](const char* arg_name) {
        if (strcmp(arg_name, "arg1") == 0) {
          return true;
        }
        EXPECT_TRUE(strcmp(arg_name, "arg2") == 0);
        return false;
      };
      return true;
    }
    // Filter no args for name3.
    EXPECT_TRUE(strcmp(event_name, "name3") == 0);
    return true;
  };

  Json::Value result = ToJsonValue(ToJson(arg_filter));

  EXPECT_EQ(result["traceEvents"].size(), 3u);

  EXPECT_EQ(result["traceEvents"][0]["cat"].asString(), "cat");
  EXPECT_EQ(result["traceEvents"][0]["name"].asString(), "name1");
  EXPECT_EQ(result["traceEvents"][0]["args"].asString(), "__stripped__");

  EXPECT_EQ(result["traceEvents"][1]["cat"].asString(), "cat");
  EXPECT_EQ(result["traceEvents"][1]["name"].asString(), "name2");
  EXPECT_EQ(result["traceEvents"][1]["args"]["arg1"].asInt(), 5);
  EXPECT_EQ(result["traceEvents"][1]["args"]["arg2"].asString(),
            "__stripped__");

  EXPECT_EQ(result["traceEvents"][2]["cat"].asString(), "cat");
  EXPECT_EQ(result["traceEvents"][2]["name"].asString(), "name3");
  EXPECT_EQ(result["traceEvents"][2]["args"]["arg1"].asInt(), 5);
  EXPECT_EQ(result["traceEvents"][2]["args"]["arg2"].asString(), "val");
}

TEST_F(ExportJsonTest, MetadataFilter) {
  const char* kName1 = "name1";
  const char* kName2 = "name2";
  const char* kValue1 = "value1";
  const int kValue2 = 222;

  TraceStorage* storage = context_.storage.get();

  RowId row_id = storage->mutable_raw_events()->AddRawEvent(
      0, storage->InternString("chrome_event.metadata"), 0, 0);

  StringId name1_id = storage->InternString(base::StringView(kName1));
  StringId name2_id = storage->InternString(base::StringView(kName2));
  StringId value1_id = storage->InternString(base::StringView(kValue1));
  context_.args_tracker->AddArg(row_id, name1_id, name1_id,
                                Variadic::String(value1_id));
  context_.args_tracker->AddArg(row_id, name2_id, name2_id,
                                Variadic::Integer(kValue2));
  context_.args_tracker->Flush();

  auto metadata_filter = [](const char* metadata_name) {
    // Only allow name1.
    return strcmp(metadata_name, "name1") == 0;
  };

  Json::Value result = ToJsonValue(ToJson(nullptr, metadata_filter));

  EXPECT_TRUE(result.isMember("metadata"));
  Json::Value metadata = result["metadata"];

  EXPECT_EQ(metadata[kName1].asString(), kValue1);
  EXPECT_EQ(metadata[kName2].asString(), "__stripped__");
}

TEST_F(ExportJsonTest, LabelFilter) {
  const int64_t kTimestamp1 = 10000000;
  const int64_t kTimestamp2 = 20000000;
  const int64_t kDuration = 10000;
  const int64_t kThreadID = 100;
  const char* kCategory = "cat";
  const char* kName = "name";

  UniqueTid utid = context_.storage->AddEmptyThread(kThreadID);
  TrackId track =
      context_.track_tracker->GetOrCreateDescriptorTrackForThread(utid);
  context_.args_tracker->Flush();  // Flush track args.
  StringId cat_id = context_.storage->InternString(base::StringView(kCategory));
  StringId name_id = context_.storage->InternString(base::StringView(kName));

  context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp1, kDuration, track, utid, RefType::kRefUtid, cat_id, name_id,
      0, 0, 0);
  context_.storage->mutable_nestable_slices()->AddSlice(
      kTimestamp2, kDuration, track, utid, RefType::kRefUtid, cat_id, name_id,
      0, 0, 0);

  auto label_filter = [](const char* label_name) {
    return strcmp(label_name, "traceEvents") == 0;
  };

  Json::Value result =
      ToJsonValue("[" + ToJson(nullptr, nullptr, label_filter) + "]");

  EXPECT_TRUE(result.isArray());
  EXPECT_EQ(result.size(), 2u);

  EXPECT_EQ(result[0]["ph"].asString(), "X");
  EXPECT_EQ(result[0]["ts"].asInt64(), kTimestamp1 / 1000);
  EXPECT_EQ(result[0]["dur"].asInt64(), kDuration / 1000);
  EXPECT_EQ(result[0]["tid"].asUInt(), kThreadID);
  EXPECT_EQ(result[0]["cat"].asString(), kCategory);
  EXPECT_EQ(result[0]["name"].asString(), kName);
  EXPECT_EQ(result[1]["ph"].asString(), "X");
  EXPECT_EQ(result[1]["ts"].asInt64(), kTimestamp2 / 1000);
  EXPECT_EQ(result[1]["dur"].asInt64(), kDuration / 1000);
  EXPECT_EQ(result[1]["tid"].asUInt(), kThreadID);
  EXPECT_EQ(result[1]["cat"].asString(), kCategory);
  EXPECT_EQ(result[1]["name"].asString(), kName);
}

}  // namespace
}  // namespace json
}  // namespace trace_processor
}  // namespace perfetto
