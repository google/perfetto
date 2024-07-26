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

#include <cstdio>
#include <string>

#include <json/reader.h>
#include <json/value.h>

#include "perfetto/ext/trace_processor/export_json.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor_storage.h"
#include "src/base/test/status_matchers.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class JsonStringOutputWriter : public json::OutputWriter {
 public:
  util::Status AppendString(const std::string& string) override {
    buffer += string;
    return util::OkStatus();
  }
  std::string buffer;
};

class StorageMinimalSmokeTest : public ::testing::Test {
 public:
  StorageMinimalSmokeTest()
      : storage_(TraceProcessorStorage::CreateInstance(Config())) {}

 protected:
  std::unique_ptr<TraceProcessorStorage> storage_;
};

TEST_F(StorageMinimalSmokeTest, GraphicEventsIgnored) {
  const size_t MAX_SIZE = 1 << 20;
  auto f = fopen(base::GetTestDataPath("test/data/gpu_trace.pb").c_str(), "rb");
  std::unique_ptr<uint8_t[]> buf(new uint8_t[MAX_SIZE]);
  auto rsize = fread(reinterpret_cast<char*>(buf.get()), 1, MAX_SIZE, f);
  util::Status status = storage_->Parse(std::move(buf), rsize);
  ASSERT_TRUE(status.ok());
  ASSERT_OK(storage_->NotifyEndOfFile());

  JsonStringOutputWriter output_writer;
  json::ExportJson(storage_.get(), &output_writer);
  Json::CharReaderBuilder b;
  auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());

  Json::Value result;
  std::string& o = output_writer.buffer;
  ASSERT_TRUE(reader->parse(o.data(), o.data() + o.length(), &result, nullptr));

  // We should only see a single event (the mapping of the idle thread to have
  // name "swapper").
  ASSERT_EQ(result["traceEvents"].size(), 1u);
}

TEST_F(StorageMinimalSmokeTest, SystraceReturnsError) {
  const size_t MAX_SIZE = 1 << 20;
  auto f =
      fopen(base::GetTestDataPath("test/data/systrace.html").c_str(), "rb");
  std::unique_ptr<uint8_t[]> buf(new uint8_t[MAX_SIZE]);
  auto rsize = fread(reinterpret_cast<char*>(buf.get()), 1, MAX_SIZE, f);
  util::Status status = storage_->Parse(std::move(buf), rsize);

  ASSERT_FALSE(status.ok());
}

TEST_F(StorageMinimalSmokeTest, TrackEventsImported) {
  const size_t MAX_SIZE = 1 << 20;
  auto f = fopen("test/data/track_event_typed_args.pb", "rb");
  std::unique_ptr<uint8_t[]> buf(new uint8_t[MAX_SIZE]);
  auto rsize = fread(reinterpret_cast<char*>(buf.get()), 1, MAX_SIZE, f);
  util::Status status = storage_->Parse(std::move(buf), rsize);
  ASSERT_TRUE(status.ok());
  ASSERT_OK(storage_->NotifyEndOfFile());

  JsonStringOutputWriter output_writer;
  json::ExportJson(storage_.get(), &output_writer);
  Json::CharReaderBuilder b;
  auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());

  Json::Value result;
  std::string& o = output_writer.buffer;
  ASSERT_TRUE(reader->parse(o.data(), o.data() + o.length(), &result, nullptr));

  // We have an "extra" event from the mapping of the idle thread to have name
  // "swapper".
  ASSERT_EQ(result["traceEvents"].size(), 5u);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
