
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

#include <string>

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/collect_timeline_events.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

// Test packet (a small clip of a later trace):
//
// packet {
//  process_tree{
//    processes {
//      pid: 1093
//      ppid: 1
//      cmdline: "zygote"
//      uid: 0
//    }
//    processes {
//      pid: 7105
//      ppid: 1093
//      cmdline: "com.Unity.com.unity.multiplayer.samples.coop"
//      uid: 10252
//    }
//    threads {
//      tid: 7127
//      tgid: 7105
//    }
//    collection_end_timestamp: 6702093738547594
//  }
//  trusted_uid: 9999
//  timestamp: 6702093635419927
//  trusted_packet_sequence_id: 6
//  incremental_state_cleared: true
//  previous_packet_dropped: true
// }

namespace {

constexpr uint64_t kSystemPackage = 0;
constexpr uint64_t kUnityUid = 10252;

constexpr int32_t kZygotePid = 1093;
constexpr int32_t kUnityPid = 7105;
constexpr int32_t kUnityTid = 7127;

constexpr uint64_t kProcessTreeTimestamp = 6702093635419927;
constexpr uint64_t kThreadFreeTimestamp = 6702094703928940;

class TestParams {
 public:
  TestParams(uint64_t ts, int32_t pid, uint64_t uid)
      : ts_(ts), pid_(pid), uid_(uid) {}

  uint64_t ts() const { return ts_; }
  int32_t pid() const { return pid_; }
  uint64_t uid() const { return uid_; }

 private:
  uint64_t ts_;
  int32_t pid_;
  uint64_t uid_;
};

}  // namespace

class CollectTimelineEventsFixture {
 protected:
  std::string CreateProcessTreePacket(uint64_t timestamp) {
    protos::gen::TracePacket packet;
    packet.set_trusted_uid(9999);
    packet.set_timestamp(timestamp);
    packet.set_trusted_packet_sequence_id(6);
    packet.set_incremental_state_cleared(true);
    packet.set_previous_packet_dropped(true);

    auto* process_tree = packet.mutable_process_tree();

    auto* zygote = process_tree->add_processes();
    zygote->set_pid(kZygotePid);
    zygote->set_ppid(1);
    zygote->add_cmdline("zygote");
    zygote->set_uid(kSystemPackage);

    auto* unity = process_tree->add_processes();
    unity->set_pid(kUnityPid);
    unity->set_ppid(1093);
    unity->add_cmdline("com.Unity.com.unity.multiplayer.samples.coop");
    unity->set_uid(kUnityUid);

    auto* thread = process_tree->add_threads();
    thread->set_tid(kUnityTid);
    thread->set_tgid(kUnityPid);

    process_tree->set_collection_end_timestamp(timestamp);

    return packet.SerializeAsString();
  }

  std::string CreateSchedProcessFreePacket(uint64_t timestamp) {
    protos::gen::TracePacket packet;

    packet.set_trusted_uid(9999);
    packet.set_timestamp(timestamp);
    packet.set_trusted_packet_sequence_id(6);
    packet.set_incremental_state_cleared(true);
    packet.set_previous_packet_dropped(true);

    auto* ftrace_events = packet.mutable_ftrace_events();
    auto* ftrace_event = ftrace_events->add_event();
    ftrace_event->set_timestamp(timestamp);
    ftrace_event->set_pid(10);  // kernel thread - e.g. "rcuop/0"

    auto* process_free = ftrace_event->mutable_sched_process_free();
    process_free->set_comm("UnityMain");
    process_free->set_pid(kUnityTid);
    process_free->set_prio(120);

    return packet.SerializeAsString();
  }
};

class CollectTimelineEventsWithProcessTree
    : public testing::Test,
      public CollectTimelineEventsFixture,
      public testing::WithParamInterface<TestParams> {
 protected:
  Context context_;
  CollectTimelineEvents collector_;
};

TEST_P(CollectTimelineEventsWithProcessTree, FindsOpenSpans) {
  auto params = GetParam();

  auto packet_str = CreateProcessTreePacket(kProcessTreeTimestamp);

  protos::pbzero::TracePacket::Decoder packet(packet_str);

  auto begin_status = collector_.Begin(&context_);
  ASSERT_OK(begin_status) << begin_status.message();

  auto packet_status = collector_.Collect(packet, &context_);
  ASSERT_OK(packet_status) << packet_status.message();

  auto end_status = collector_.End(&context_);
  ASSERT_OK(end_status) << end_status.message();

  auto slice = context_.timeline->Search(params.ts(), params.pid());
  ASSERT_EQ(slice.pid, params.pid());
  ASSERT_EQ(slice.uid, params.uid());
}

INSTANTIATE_TEST_SUITE_P(
    AcrossWholeTimeline,
    CollectTimelineEventsWithProcessTree,
    testing::Values(
        // System-level process/thread
        TestParams(kProcessTreeTimestamp - 1,
                   kZygotePid,
                   ProcessThreadTimeline::Event::kUnknownUid),
        TestParams(kProcessTreeTimestamp, kZygotePid, kSystemPackage),
        TestParams(kProcessTreeTimestamp + 1, kZygotePid, kSystemPackage),

        // Process
        TestParams(kProcessTreeTimestamp - 1,
                   kUnityPid,
                   ProcessThreadTimeline::Event::kUnknownUid),
        TestParams(kProcessTreeTimestamp, kUnityPid, kUnityUid),
        TestParams(kProcessTreeTimestamp + 1, kUnityPid, kUnityUid),

        // Child thread. kUnityPid is the parent.
        TestParams(kProcessTreeTimestamp - 1,
                   kUnityTid,
                   ProcessThreadTimeline::Event::kUnknownUid),
        TestParams(kProcessTreeTimestamp, kUnityTid, kUnityUid),
        TestParams(kProcessTreeTimestamp + 1, kUnityTid, kUnityUid)));

class CollectTimelineEventsWithFreeProcess
    : public testing::Test,
      public CollectTimelineEventsFixture {
 protected:
  void SetUp() {
    std::array<std::string, 2> buffers = {
        CreateProcessTreePacket(kProcessTreeTimestamp),
        CreateSchedProcessFreePacket(kThreadFreeTimestamp)};

    std::array<protos::pbzero::TracePacket::Decoder, 2> decoders = {
        protos::pbzero::TracePacket::Decoder(buffers[0]),
        protos::pbzero::TracePacket::Decoder(buffers[1]),
    };

    ASSERT_OK(collector_.Begin(&context_));
    ASSERT_OK(collector_.Collect(decoders[0], &context_));
    ASSERT_OK(collector_.Collect(decoders[1], &context_));
    ASSERT_OK(collector_.End(&context_));
  }

  Context context_;
  CollectTimelineEvents collector_;
};

TEST_F(CollectTimelineEventsWithFreeProcess, FindsPackageBeforeFree) {
  auto slice = context_.timeline->Search(kThreadFreeTimestamp - 1, kUnityTid);

  ASSERT_EQ(slice.pid, kUnityTid);
  ASSERT_EQ(slice.uid, kUnityUid);
}

TEST_F(CollectTimelineEventsWithFreeProcess, NoPackageAtFree) {
  auto slice = context_.timeline->Search(kThreadFreeTimestamp, kUnityTid);

  ASSERT_EQ(slice.pid, kUnityTid);
  ASSERT_EQ(slice.uid, ProcessThreadTimeline::Event::kUnknownUid);
}

TEST_F(CollectTimelineEventsWithFreeProcess, NoPackageAfterFree) {
  auto slice = context_.timeline->Search(kThreadFreeTimestamp + 1, kUnityTid);

  ASSERT_EQ(slice.pid, kUnityTid);
  ASSERT_EQ(slice.uid, ProcessThreadTimeline::Event::kUnknownUid);
}

}  // namespace perfetto::trace_redaction
