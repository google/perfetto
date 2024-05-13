
/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/collect_timeline_events.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {

constexpr uint64_t kSystemPackage = 0;
constexpr uint64_t kUnityUid = 10252;

constexpr int32_t kZygotePid = 1093;
constexpr int32_t kUnityPid = 7105;
constexpr int32_t kUnityTid = 7127;

// TODO(vaage): Need a better name and documentation.
constexpr int32_t kPidWithNoOpen = 32;

constexpr uint64_t kProcessTreeTimestamp = 6702093635419927;

// These two timestamps are used to separate the packet and event times. A
// packet can have time X, but the time can have time Y. Time Y should be used
// for the events.
constexpr uint64_t kThreadFreePacketTimestamp = 6702094703928940;
constexpr uint64_t kThreadFreeOffset = 100;

}  // namespace

// Base class for all collect timeline event tests. Creates a simple trace that
// contains trace elements that should create timeline events.
class CollectTimelineEventsTest : public testing::Test {
 protected:
  void SetUp() {
    CollectTimelineEvents collector;

    ASSERT_OK(collector.Begin(&context_));

    // Minimum ProcessTree information.
    {
      auto timestamp = kProcessTreeTimestamp;

      protos::gen::TracePacket packet;
      packet.set_timestamp(timestamp);

      auto* process_tree = packet.mutable_process_tree();

      auto* zygote = process_tree->add_processes();
      zygote->set_pid(kZygotePid);
      zygote->set_ppid(1);
      zygote->set_uid(kSystemPackage);

      auto* unity = process_tree->add_processes();
      unity->set_pid(kUnityPid);
      unity->set_ppid(1093);
      unity->set_uid(kUnityUid);

      auto* thread = process_tree->add_threads();
      thread->set_tid(kUnityTid);
      thread->set_tgid(kUnityPid);

      process_tree->set_collection_end_timestamp(timestamp);

      auto buffer = packet.SerializeAsString();

      protos::pbzero::TracePacket::Decoder decoder(buffer);
      ASSERT_OK(collector.Collect(decoder, &context_));
    }

    // Minimum proc free informations.
    {
      auto timestamp = kThreadFreePacketTimestamp;

      protos::gen::TracePacket packet;
      packet.set_timestamp(timestamp);

      auto* ftrace_event = packet.mutable_ftrace_events()->add_event();
      ftrace_event->set_timestamp(timestamp + kThreadFreeOffset);
      ftrace_event->set_pid(10);  // kernel thread - e.g. "rcuop/0"

      auto* process_free = ftrace_event->mutable_sched_process_free();
      process_free->set_pid(kUnityTid);

      auto buffer = packet.SerializeAsString();

      protos::pbzero::TracePacket::Decoder decoder(buffer);
      ASSERT_OK(collector.Collect(decoder, &context_));
    }

    // Free a pid that neve started.
    {
      auto timestamp = kThreadFreePacketTimestamp;

      protos::gen::TracePacket packet;
      packet.set_timestamp(timestamp);

      auto* ftrace_event = packet.mutable_ftrace_events()->add_event();
      ftrace_event->set_timestamp(timestamp + kThreadFreeOffset);
      ftrace_event->set_pid(10);  // kernel thread - e.g. "rcuop/0"

      auto* process_free = ftrace_event->mutable_sched_process_free();
      process_free->set_pid(kPidWithNoOpen);

      auto buffer = packet.SerializeAsString();

      protos::pbzero::TracePacket::Decoder decoder(buffer);
      ASSERT_OK(collector.Collect(decoder, &context_));
    }

    ASSERT_OK(collector.End(&context_));
  }

  Context context_;
};

class CollectTimelineFindsOpenEventTest
    : public CollectTimelineEventsTest,
      public testing::WithParamInterface<int32_t> {};

TEST_P(CollectTimelineFindsOpenEventTest, NoOpenEventBeforeProcessTree) {
  auto pid = GetParam();

  auto event =
      context_.timeline->FindPreviousEvent(kProcessTreeTimestamp - 1, pid);
  ASSERT_EQ(event.type, ProcessThreadTimeline::Event::Type::kInvalid);
}

TEST_P(CollectTimelineFindsOpenEventTest, OpenEventOnProcessTree) {
  auto pid = GetParam();

  auto event = context_.timeline->FindPreviousEvent(kProcessTreeTimestamp, pid);
  ASSERT_EQ(event.type, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_EQ(event.pid, pid);
}

TEST_P(CollectTimelineFindsOpenEventTest, OpenEventAfterProcessTree) {
  auto pid = GetParam();

  auto event = context_.timeline->FindPreviousEvent(kProcessTreeTimestamp, pid);
  ASSERT_EQ(event.type, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_EQ(event.pid, pid);
}

INSTANTIATE_TEST_SUITE_P(
    SystemProcess,
    CollectTimelineFindsOpenEventTest,
    testing::Values(kZygotePid,  // System-level process/thread
                    kUnityPid,   // Process
                    kUnityTid    // Child thread. kUnityPid is the parent.
                    ));

class CollectTimelineFindsFreeEventTest : public CollectTimelineEventsTest {};

TEST_F(CollectTimelineFindsFreeEventTest, UsesFtraceEventTime) {
  auto pid = kUnityTid;

  // While this will be a valid event (type != invalid), it won't be the close
  // event.
  auto incorrect =
      context_.timeline->FindPreviousEvent(kThreadFreePacketTimestamp, pid);
  ASSERT_EQ(incorrect.type, ProcessThreadTimeline::Event::Type::kOpen);

  auto correct = context_.timeline->FindPreviousEvent(
      kThreadFreePacketTimestamp + kThreadFreeOffset, pid);
  ASSERT_EQ(correct.type, ProcessThreadTimeline::Event::Type::kClose);
}

TEST_F(CollectTimelineFindsFreeEventTest, NoCloseEventBeforeFree) {
  auto pid = kUnityTid;

  auto event =
      context_.timeline->FindPreviousEvent(kThreadFreePacketTimestamp - 1, pid);
  ASSERT_EQ(event.type, ProcessThreadTimeline::Event::Type::kOpen);
  ASSERT_EQ(event.pid, pid);
}

// Whether or not AddsCloseOnFree and AddsCloseAfterFree are the same close
// event is an implementation detail.
TEST_F(CollectTimelineFindsFreeEventTest, AddsCloseOnFree) {
  auto pid = kUnityTid;

  auto event = context_.timeline->FindPreviousEvent(
      kThreadFreePacketTimestamp + kThreadFreeOffset, pid);
  ASSERT_EQ(event.type, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_EQ(event.pid, pid);
}

TEST_F(CollectTimelineFindsFreeEventTest, AddsCloseAfterFree) {
  auto pid = kUnityTid;

  auto event = context_.timeline->FindPreviousEvent(
      kThreadFreePacketTimestamp + kThreadFreeOffset + 1, pid);
  ASSERT_EQ(event.type, ProcessThreadTimeline::Event::Type::kClose);
  ASSERT_EQ(event.pid, pid);
}

}  // namespace perfetto::trace_redaction
