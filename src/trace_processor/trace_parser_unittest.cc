/*
 * Copyright (C) 2017 The Android Open foo Project
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

#include "src/trace_processor/trace_parser.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "perfetto/base/logging.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;

class FakeStringBlobReader : public BlobReader {
 public:
  FakeStringBlobReader(const std::string& data) : data_(data) {}
  ~FakeStringBlobReader() override {}

  uint32_t Read(uint64_t offset, uint32_t len, uint8_t* dst) override {
    PERFETTO_CHECK(offset <= data_.size());
    uint32_t rsize =
        std::min(static_cast<uint32_t>(data_.size() - offset), len);
    memcpy(dst, data_.c_str() + offset, rsize);
    return rsize;
  }

 private:
  std::string data_;
};

TEST(TraceParser, LoadSinglePacket) {
  protos::Trace trace;

  auto* bundle = trace.add_packet()->mutable_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);

  auto* sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_next_prio(100);

  FakeStringBlobReader reader(trace.SerializeAsString());
  TraceStorage storage;
  TraceParser parser(&reader, &storage, 1024);
  parser.ParseNextChunk();
}

TEST(TraceParser, LoadMultiplePacket) {
  // TODO(lalitm): write this test.
}

TEST(TraceParser, RepeatedLoadSinglePacket) {
  // TODO(lalitm): write this test.
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
