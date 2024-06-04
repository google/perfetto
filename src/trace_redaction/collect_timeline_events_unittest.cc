
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

#include "src/trace_redaction/collect_timeline_events.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/task.gen.h"
#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {

constexpr uint64_t kPackage = 0;
constexpr int32_t kPid = 1093;

constexpr uint64_t kFullStep = 1000;
constexpr uint64_t kTimeA = 0;
constexpr uint64_t kTimeB = kFullStep;
constexpr uint64_t kTimeC = kFullStep * 2;

}  // namespace

// Base class for all collect timeline event tests. Creates a simple trace that
// contains trace elements that should create timeline events.
class CollectTimelineEventsTest : public testing::Test {
 protected:
  void SetUp() { ASSERT_OK(collector_.Begin(&context_)); }

  Context context_;
  CollectTimelineEvents collector_;
};

TEST_F(CollectTimelineEventsTest, OpenEventForProcessTreeProcess) {
  {
    protos::gen::TracePacket packet;
    packet.set_timestamp(kTimeA);

    auto* process_tree = packet.mutable_process_tree();

    auto* process = process_tree->add_processes();
    process->set_pid(kPid);
    process->set_ppid(1);
    process->set_uid(kPackage);

    auto buffer = packet.SerializeAsString();

    protos::pbzero::TracePacket::Decoder decoder(buffer);

    ASSERT_OK(collector_.Collect(decoder, &context_));
  }

  ASSERT_OK(collector_.End(&context_));

  const auto* event = context_.timeline->GetOpeningEvent(kTimeA, kPid);
  ASSERT_TRUE(event);
  ASSERT_TRUE(event->valid());
}

TEST_F(CollectTimelineEventsTest, OpenEventForProcessTreeThread) {
  {
    protos::gen::TracePacket packet;
    packet.set_timestamp(kTimeA);

    auto* process_tree = packet.mutable_process_tree();

    auto* process = process_tree->add_threads();
    process->set_tid(kPid);
    process->set_tgid(1);

    auto buffer = packet.SerializeAsString();

    protos::pbzero::TracePacket::Decoder decoder(buffer);

    ASSERT_OK(collector_.Collect(decoder, &context_));
  }

  ASSERT_OK(collector_.End(&context_));

  const auto* event = context_.timeline->GetOpeningEvent(kTimeA, kPid);
  ASSERT_TRUE(event);
  ASSERT_TRUE(event->valid());
}

TEST_F(CollectTimelineEventsTest, OpenEventForNewTask) {
  {
    protos::gen::TracePacket packet;
    auto* event = packet.mutable_ftrace_events()->add_event();
    event->set_timestamp(kTimeA);

    auto* new_task = event->mutable_task_newtask();
    new_task->set_clone_flags(0);
    new_task->set_comm("");
    new_task->set_oom_score_adj(0);
    new_task->set_pid(kPid);

    auto buffer = packet.SerializeAsString();

    protos::pbzero::TracePacket::Decoder decoder(buffer);

    ASSERT_OK(collector_.Collect(decoder, &context_));
  }

  ASSERT_OK(collector_.End(&context_));

  const auto* open_event = context_.timeline->GetOpeningEvent(kTimeA, kPid);
  ASSERT_TRUE(open_event);
  ASSERT_TRUE(open_event->valid());
}

TEST_F(CollectTimelineEventsTest, ProcFreeEndsThread) {
  {
    protos::gen::TracePacket packet;

    auto* event = packet.mutable_ftrace_events()->add_event();
    event->set_timestamp(kTimeA);

    auto* new_task = event->mutable_task_newtask();
    new_task->set_clone_flags(0);
    new_task->set_comm("");
    new_task->set_oom_score_adj(0);
    new_task->set_pid(kPid);

    auto buffer = packet.SerializeAsString();

    protos::pbzero::TracePacket::Decoder decoder(buffer);
    ASSERT_OK(collector_.Collect(decoder, &context_));
  }

  {
    protos::gen::TracePacket packet;

    auto* event = packet.mutable_ftrace_events()->add_event();
    event->set_timestamp(kTimeB);

    auto* process_free = event->mutable_sched_process_free();
    process_free->set_comm("");
    process_free->set_pid(kPid);
    process_free->set_prio(0);

    auto buffer = packet.SerializeAsString();

    protos::pbzero::TracePacket::Decoder decoder(buffer);
    ASSERT_OK(collector_.Collect(decoder, &context_));
  }

  ASSERT_OK(collector_.End(&context_));

  const auto* start = context_.timeline->GetOpeningEvent(kTimeA, kPid);
  ASSERT_TRUE(start);
  ASSERT_TRUE(start->valid());

  // The end event was correctly set so that the free event is inclusive.
  const auto* end = context_.timeline->GetOpeningEvent(kTimeB, kPid);
  ASSERT_TRUE(end);
  ASSERT_TRUE(end->valid());

  const auto* after = context_.timeline->GetOpeningEvent(kTimeC, kPid);
  ASSERT_FALSE(after);
}

}  // namespace perfetto::trace_redaction
