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
#include "src/trace_processor/proto_trace_parser.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "src/trace_processor/basic_types.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_sorter.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;

class MockTraceParser : public ProtoTraceParser {
 public:
  MockTraceParser(TraceProcessorContext* context) : ProtoTraceParser(context) {}

  MOCK_METHOD4(MOCK_ParseFtracePacket,
               void(uint32_t cpu,
                    uint64_t timestamp,
                    const uint8_t* data,
                    size_t length));

  void ParseFtracePacket(uint32_t cpu,
                         uint64_t timestamp,
                         TraceBlobView tbv) override {
    MOCK_ParseFtracePacket(cpu, timestamp, tbv.data(), tbv.length());
  }

  MOCK_METHOD2(MOCK_ParseTracePacket, void(const uint8_t* data, size_t length));

  void ParseTracePacket(TraceBlobView tbv) override {
    MOCK_ParseTracePacket(tbv.data(), tbv.length());
  }
};

class MockTraceStorage : public TraceStorage {
 public:
  MockTraceStorage() : TraceStorage() {}

  MOCK_METHOD1(InternString, StringId(base::StringView view));
};

class TraceSorterTest : public ::testing::TestWithParam<OptimizationMode> {
 public:
  TraceSorterTest()
      : test_buffer_(std::unique_ptr<uint8_t[]>(new uint8_t[8]), 0, 8) {
    storage_ = new MockTraceStorage();
    context_.storage.reset(storage_);
    context_.sorter.reset(
        new TraceSorter(&context_, GetParam(), 0 /*window_size*/));
    parser_ = new MockTraceParser(&context_);
    context_.proto_parser.reset(parser_);
  }

 protected:
  TraceProcessorContext context_;
  MockTraceParser* parser_;
  MockTraceStorage* storage_;
  TraceBlobView test_buffer_;
};

INSTANTIATE_TEST_CASE_P(OptMode,
                        TraceSorterTest,
                        ::testing::Values(OptimizationMode::kMaxBandwidth,
                                          OptimizationMode::kMinLatency));

TEST_P(TraceSorterTest, TestFtrace) {
  TraceBlobView view = test_buffer_.slice(0, 1);
  EXPECT_CALL(*parser_, MOCK_ParseFtracePacket(0, 1000, view.data(), 1));
  context_.sorter->PushFtracePacket(0 /*cpu*/, 1000 /*timestamp*/,
                                    std::move(view));
  context_.sorter->FlushEventsForced();
}

TEST_P(TraceSorterTest, TestTracePacket) {
  TraceBlobView view = test_buffer_.slice(0, 1);
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(view.data(), 1));
  context_.sorter->PushTracePacket(1000, std::move(view));
  context_.sorter->FlushEventsForced();
}

TEST_P(TraceSorterTest, Ordering) {
  TraceBlobView view_1 = test_buffer_.slice(0, 1);
  TraceBlobView view_2 = test_buffer_.slice(0, 2);
  TraceBlobView view_3 = test_buffer_.slice(0, 3);
  TraceBlobView view_4 = test_buffer_.slice(0, 4);

  InSequence s;

  EXPECT_CALL(*parser_, MOCK_ParseFtracePacket(0, 1000, view_1.data(), 1));
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(view_2.data(), 2));
  EXPECT_CALL(*parser_, MOCK_ParseTracePacket(view_3.data(), 3));
  EXPECT_CALL(*parser_, MOCK_ParseFtracePacket(2, 1200, view_4.data(), 4));

  context_.sorter->set_window_ns_for_testing(200);
  context_.sorter->PushFtracePacket(2 /*cpu*/, 1200 /*timestamp*/,
                                    std::move(view_4));
  context_.sorter->PushTracePacket(1001, std::move(view_2));
  context_.sorter->PushTracePacket(1100, std::move(view_3));
  context_.sorter->PushFtracePacket(0 /*cpu*/, 1000 /*timestamp*/,
                                    std::move(view_1));

  context_.sorter->FlushEventsForced();
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
