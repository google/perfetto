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
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/sched_tracker.h"

#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::_;
using ::testing::Args;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Pointwise;

class MockSchedTracker : public SchedTracker {
 public:
  MockSchedTracker(TraceProcessorContext* context) : SchedTracker(context) {}
  virtual ~MockSchedTracker() = default;

  MOCK_METHOD6(PushSchedSwitch,
               void(uint32_t cpu,
                    uint64_t timestamp,
                    uint32_t prev_pid,
                    uint32_t prev_state,
                    base::StringView prev_comm,
                    uint32_t next_pid));
};

class MockProcessTracker : public ProcessTracker {
 public:
  MockProcessTracker(TraceProcessorContext* context)
      : ProcessTracker(context) {}

  MOCK_METHOD2(UpdateProcess,
               UniquePid(uint32_t pid, base::StringView process_name));

  MOCK_METHOD2(UpdateThread, UniqueTid(uint32_t tid, uint32_t tgid));
};

void ParseTraceProto(const protos::Trace& trace, ProtoTraceParser* parser) {
  const size_t trace_size = static_cast<size_t>(trace.ByteSize());
  std::unique_ptr<uint8_t[]> buf(new uint8_t[trace_size]);
  trace.SerializeWithCachedSizesToArray(&buf[0]);
  parser->Parse(std::move(buf), trace_size);
}

TEST(ProtoTraceParserTest, LoadSingleEvent_CpuStart) {
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
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32,
                                      base::StringView(kProcName), 100));

  ProtoTraceParser parser(&context);
  ParseTraceProto(trace, &parser);
}

TEST(ProtoTraceParserTest, LoadSingleEvent_CpuMiddle) {
  protos::Trace trace;

  auto* bundle = trace.add_packet()->mutable_ftrace_events();
  bundle->set_overwrite_count(999);
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
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32,
                                      base::StringView(kProcName), 100));

  ProtoTraceParser parser(&context);
  ParseTraceProto(trace, &parser);
}

TEST(ProtoTraceParserTest, LoadSingleEvent_CpuSecondFromEnd) {
  protos::Trace trace;

  auto* bundle = trace.add_packet()->mutable_ftrace_events();
  auto* event = bundle->add_event();
  event->set_timestamp(1000);

  static const char kProcName[] = "proc1";
  auto* sched_switch = event->mutable_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_state(32);
  sched_switch->set_prev_comm(kProcName);
  sched_switch->set_next_pid(100);

  bundle->set_cpu(10);
  bundle->set_overwrite_count(999);

  TraceProcessorContext context;
  MockSchedTracker* sched = new MockSchedTracker(&context);
  context.sched_tracker.reset(sched);
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32,
                                      base::StringView(kProcName), 100));

  ProtoTraceParser parser(&context);
  ParseTraceProto(trace, &parser);
}

TEST(ProtoTraceParserTest, LoadMultipleEvents) {
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
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32,
                                      base::StringView(kProcName1), 100));

  EXPECT_CALL(*sched, PushSchedSwitch(10, 1001, 100, 32,
                                      base::StringView(kProcName2), 10));

  ProtoTraceParser parser(&context);
  ParseTraceProto(trace, &parser);
}

TEST(ProtoTraceParserTest, LoadMultiplePackets) {
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
  EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32,
                                      base::StringView(kProcName1), 100));

  EXPECT_CALL(*sched, PushSchedSwitch(10, 1001, 100, 32,
                                      base::StringView(kProcName2), 10));

  ProtoTraceParser parser(&context);
  ParseTraceProto(trace, &parser);
}

TEST(ProtoTraceParserTest, RepeatedLoadSinglePacket) {
  TraceProcessorContext context;
  MockSchedTracker* sched = new MockSchedTracker(&context);
  context.sched_tracker.reset(sched);
  ProtoTraceParser parser(&context);

  {
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
    EXPECT_CALL(*sched, PushSchedSwitch(10, 1000, 10, 32,
                                        base::StringView(kProcName1), 100));
    ParseTraceProto(trace, &parser);
  }
  {
    protos::Trace trace;
    auto* bundle = trace.add_packet()->mutable_ftrace_events();
    bundle->set_cpu(10);
    auto* event = bundle->add_event();
    event->set_timestamp(1001);
    static const char kProcName2[] = "proc2";
    auto* sched_switch = event->mutable_sched_switch();
    sched_switch->set_prev_pid(100);
    sched_switch->set_prev_state(32);
    sched_switch->set_prev_comm(kProcName2);
    sched_switch->set_next_pid(10);
    EXPECT_CALL(*sched, PushSchedSwitch(10, 1001, 100, 32,
                                        base::StringView(kProcName2), 10));
    ParseTraceProto(trace, &parser);
  }
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
  EXPECT_CALL(*process_tracker, UpdateProcess(1, base::StringView(kProcName1)));
  ProtoTraceParser parser(&context);
  ParseTraceProto(trace, &parser);
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
  EXPECT_CALL(*process_tracker, UpdateProcess(1, base::StringView(kProcName1)));
  ProtoTraceParser parser(&context);
  ParseTraceProto(trace, &parser);
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
  ProtoTraceParser parser(&context);
  ParseTraceProto(trace, &parser);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
