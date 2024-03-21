
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
#include "src/trace_redaction/build_timeline.h"
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

constexpr uint64_t kNoPackage = 0;
constexpr uint64_t kUnityPackage = 10252;

constexpr uint64_t kZygotePid = 1093;
constexpr uint64_t kUnityPid = 7105;
constexpr uint64_t kUnityTid = 7127;

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

class BuildTimelineTest : public testing::Test,
                          public testing::WithParamInterface<TestParams> {
 protected:
  base::StatusOr<CollectPrimitive::ContinueCollection> PushProcessTreePacket(
      uint64_t timestamp) {
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
    zygote->set_uid(0);

    auto* unity = process_tree->add_processes();
    unity->set_pid(kUnityPid);
    unity->set_ppid(1093);
    unity->add_cmdline("com.Unity.com.unity.multiplayer.samples.coop");
    unity->set_uid(kUnityPackage);

    auto* thread = process_tree->add_threads();
    thread->set_tid(kUnityTid);
    thread->set_tgid(kUnityPid);

    process_tree->set_collection_end_timestamp(timestamp);

    std::string packet_str = packet.SerializeAsString();
    return build_.Collect(protos::pbzero::TracePacket::Decoder(packet_str),
                          &context_);
  }

  base::StatusOr<CollectPrimitive::ContinueCollection>
  PushSchedProcessFreePacket(uint64_t timestamp) {
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

    std::string packet_str = packet.SerializeAsString();
    return build_.Collect(protos::pbzero::TracePacket::Decoder(packet_str),
                          &context_);
  }

  BuildTimeline build_;
  Context context_;
};

class BuildTimelineWithProcessTree : public BuildTimelineTest {};

TEST_P(BuildTimelineWithProcessTree, FindsOpenSpans) {
  auto params = GetParam();

  auto result = PushProcessTreePacket(kProcessTreeTimestamp);
  ASSERT_OK(result) << result.status().message();

  context_.timeline->Sort();

  auto slice = context_.timeline->Search(params.ts(), params.pid());
  ASSERT_EQ(slice.pid, params.pid());
  ASSERT_EQ(slice.uid, params.uid());
}

INSTANTIATE_TEST_SUITE_P(
    AcrossWholeTimeline,
    BuildTimelineWithProcessTree,
    testing::Values(
        // Before the processes/threads existed.
        TestParams(0, kZygotePid, kNoPackage),
        TestParams(0, kUnityPid, kNoPackage),
        TestParams(0, kUnityTid, kNoPackage),

        // When the process tree started.
        TestParams(kProcessTreeTimestamp, kZygotePid, kNoPackage),
        TestParams(kProcessTreeTimestamp, kUnityPid, kUnityPackage),
        TestParams(kProcessTreeTimestamp, kUnityTid, kUnityPackage),

        // After the process tree started.
        TestParams(kProcessTreeTimestamp + 1, kZygotePid, kNoPackage),
        TestParams(kProcessTreeTimestamp + 1, kUnityPid, kUnityPackage),
        TestParams(kProcessTreeTimestamp + 1, kUnityTid, kUnityPackage)));

// Assumes all BuildTimelineWithProcessTree tests pass.
class BuildTimelineWithFreeProcess : public BuildTimelineTest {};

TEST_P(BuildTimelineWithFreeProcess, FindsClosedSpans) {
  auto params = GetParam();

  auto result = PushProcessTreePacket(kProcessTreeTimestamp);
  ASSERT_OK(result) << result.status().message();

  result = PushSchedProcessFreePacket(kThreadFreeTimestamp);
  ASSERT_OK(result) << result.status().message();

  context_.timeline->Sort();

  auto slice = context_.timeline->Search(params.ts(), params.pid());
  ASSERT_EQ(slice.pid, params.pid());
  ASSERT_EQ(slice.uid, params.uid());
}

INSTANTIATE_TEST_SUITE_P(
    AcrossWholeTimeline,
    BuildTimelineWithFreeProcess,
    testing::Values(
        TestParams(kThreadFreeTimestamp - 1, kZygotePid, kNoPackage),
        TestParams(kThreadFreeTimestamp - 1, kUnityPid, kUnityPackage),
        TestParams(kThreadFreeTimestamp - 1, kUnityTid, kUnityPackage),

        TestParams(kThreadFreeTimestamp, kZygotePid, kNoPackage),
        TestParams(kThreadFreeTimestamp, kUnityPid, kUnityPackage),
        TestParams(kThreadFreeTimestamp, kUnityTid, kNoPackage),

        TestParams(kThreadFreeTimestamp + 1, kZygotePid, kNoPackage),
        TestParams(kThreadFreeTimestamp + 1, kUnityPid, kUnityPackage),
        TestParams(kThreadFreeTimestamp + 1, kUnityTid, kNoPackage)));

}  // namespace perfetto::trace_redaction
