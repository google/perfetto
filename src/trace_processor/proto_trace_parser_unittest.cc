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
#include "src/trace_processor/blob_reader.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/sched_tracker.h"

#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::Args;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Pointwise;
using ::testing::_;

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

class MockSchedTracker : public SchedTracker {
 public:
  MockSchedTracker(TraceProcessorContext* context) : SchedTracker(context) {}
  virtual ~MockSchedTracker() = default;

  MOCK_METHOD7(PushSchedSwitch,
               void(uint32_t cpu,
                    uint64_t timestamp,
                    uint32_t prev_pid,
                    uint32_t prev_state,
                    const char* prev_comm,
                    size_t prev_comm_len,
                    uint32_t next_pid));
};

class MockProcessTracker : public ProcessTracker {
 public:
  MockProcessTracker(TraceProcessorContext* context)
      : ProcessTracker(context) {}

  MOCK_METHOD3(UpdateProcess,
               UniquePid(uint32_t pid,
                         const char* process_name,
                         size_t process_name_len));

  MOCK_METHOD2(UpdateThread, UniqueTid(uint32_t tid, uint32_t tgid));
};

TEST(ProtoTraceParser, LoadSingleEvent) {
  protos::Trace trace;

  auto* bundle = trace.add_packet()->mutable_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);

  static const char kProcName[] = "proc1";
  auto* sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_state(32);
  sched_switch->set_prev_comm(kProcName);
  sched_switch->set_next_pid(100);

  TraceProcessorContext context;
  MockSchedTracker* sched = new MockSchedTracker(&context);
  context.sched_tracker.reset(sched);
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32, _, _, 100))
      .With(Args<4, 5>(ElementsAreArray(kProcName, sizeof(kProcName) - 1)));

  FakeStringBlobReader reader(trace.SerializeAsString());
  ProtoTraceParser parser(&reader, &context);
  parser.ParseNextChunk();
}

TEST(ProtoTraceParser, LoadMultipleEvents) {
  protos::Trace trace;

  auto* bundle = trace.add_packet()->mutable_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);

  static const char kProcName1[] = "proc1";
  auto* sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_state(32);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_next_pid(100);

  event = bundle->add_event();
  event->set_timestamp(1001);

  static const char kProcName2[] = "proc2";
  sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_state(32);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_next_pid(10);

  TraceProcessorContext context;
  MockSchedTracker* sched = new MockSchedTracker(&context);
  context.sched_tracker.reset(sched);
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32, _, _, 100))
      .With(Args<4, 5>(ElementsAreArray(kProcName1, sizeof(kProcName1) - 1)));

  EXPECT_CALL(*sched, PushSchedSwitch(10, 1001, 100, 32, _, _, 10))
      .With(Args<4, 5>(ElementsAreArray(kProcName2, sizeof(kProcName2) - 1)));

  FakeStringBlobReader reader(trace.SerializeAsString());
  ProtoTraceParser parser(&reader, &context);
  parser.ParseNextChunk();
}

TEST(ProtoTraceParser, LoadMultiplePackets) {
  protos::Trace trace;

  auto* bundle = trace.add_packet()->mutable_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);

  static const char kProcName1[] = "proc1";
  auto* sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_state(32);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_next_pid(100);

  bundle = trace.add_packet()->mutable_ftrace_events();
  bundle->set_cpu(10);

  event = bundle->add_event();
  event->set_timestamp(1001);

  static const char kProcName2[] = "proc2";
  sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_state(32);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_next_pid(10);

  TraceProcessorContext context;
  MockSchedTracker* sched = new MockSchedTracker(&context);
  context.sched_tracker.reset(sched);
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32, _, _, 100))
      .With(Args<4, 5>(ElementsAreArray(kProcName1, sizeof(kProcName1) - 1)));

  EXPECT_CALL(*sched, PushSchedSwitch(10, 1001, 100, 32, _, _, 10))
      .With(Args<4, 5>(ElementsAreArray(kProcName2, sizeof(kProcName2) - 1)));

  FakeStringBlobReader reader(trace.SerializeAsString());
  ProtoTraceParser parser(&reader, &context);
  parser.ParseNextChunk();
}

TEST(ProtoTraceParser, RepeatedLoadSinglePacket) {
  protos::Trace trace;

  auto* bundle = trace.add_packet()->mutable_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);

  static const char kProcName1[] = "proc1";
  auto* sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_state(32);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_next_pid(100);

  // Make the chunk size the size of the first packet.
  uint32_t chunk_size = static_cast<uint32_t>(trace.ByteSize());

  bundle = trace.add_packet()->mutable_ftrace_events();
  bundle->set_cpu(10);

  event = bundle->add_event();
  event->set_timestamp(1001);

  static const char kProcName2[] = "proc2";
  sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_state(32);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_next_pid(10);

  TraceProcessorContext context;
  MockSchedTracker* sched = new MockSchedTracker(&context);
  context.sched_tracker.reset(sched);
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32, _, _, 100))
      .With(Args<4, 5>(ElementsAreArray(kProcName1, sizeof(kProcName1) - 1)));

  FakeStringBlobReader reader(trace.SerializeAsString());
  ProtoTraceParser parser(&reader, &context);
  parser.set_chunk_size_for_testing(chunk_size);
  parser.ParseNextChunk();

  EXPECT_CALL(*sched, PushSchedSwitch(10, 1001, 100, 32, _, _, 10))
      .With(Args<4, 5>(ElementsAreArray(kProcName2, sizeof(kProcName2) - 1)));

  parser.ParseNextChunk();
}

TEST(ProtoTraceParserTest, LoadProcessPacket) {
  protos::Trace trace;

  auto* tree = trace.add_packet()->mutable_process_tree();
  auto* process = tree->add_processes();
  static const char kProcName1[] = "proc1";

  process->add_cmdline(kProcName1);
  process->set_pid(1);
  process->set_ppid(2);

  TraceProcessorContext context;
  MockProcessTracker* process_tracker = new MockProcessTracker(&context);
  context.process_tracker.reset(process_tracker);
  EXPECT_CALL(*process_tracker, UpdateProcess(1, _, _))
      .With(Args<1, 2>(ElementsAreArray(kProcName1, sizeof(kProcName1) - 1)));
  FakeStringBlobReader reader(trace.SerializeAsString());
  ProtoTraceParser parser(&reader, &context);
  parser.ParseNextChunk();
}

TEST(ProtoTraceParserTest, LoadProcessPacket_FirstCmdline) {
  protos::Trace trace;

  auto* tree = trace.add_packet()->mutable_process_tree();
  auto* process = tree->add_processes();
  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";

  process->add_cmdline(kProcName1);
  process->add_cmdline(kProcName2);
  process->set_pid(1);
  process->set_ppid(2);

  TraceProcessorContext context;
  MockProcessTracker* process_tracker = new MockProcessTracker(&context);
  context.process_tracker.reset(process_tracker);
  EXPECT_CALL(*process_tracker, UpdateProcess(1, _, _))
      .With(Args<1, 2>(ElementsAreArray(kProcName1, sizeof(kProcName1) - 1)));
  FakeStringBlobReader reader(trace.SerializeAsString());
  ProtoTraceParser parser(&reader, &context);
  parser.ParseNextChunk();
}

TEST(ProtoTraceParserTest, LoadThreadPacket) {
  protos::Trace trace;

  auto* tree = trace.add_packet()->mutable_process_tree();
  auto* thread = tree->add_threads();
  thread->set_tid(1);
  thread->set_tgid(2);

  TraceProcessorContext context;
  MockProcessTracker* process_tracker = new MockProcessTracker(&context);
  context.process_tracker.reset(process_tracker);
  EXPECT_CALL(*process_tracker, UpdateThread(1, 2));
  FakeStringBlobReader reader(trace.SerializeAsString());
  ProtoTraceParser parser(&reader, &context);
  parser.ParseNextChunk();
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
