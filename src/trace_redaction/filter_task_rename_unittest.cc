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

#include "src/trace_redaction/filter_task_rename.h"
#include "src/trace_redaction/process_thread_timeline.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/ftrace/task.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace {

// Used when a single pid is needed.
constexpr uint32_t kPid = 7971;

constexpr uint64_t kUid = 27;

constexpr uint64_t kJustSomeTime = 6702094131629195;

}  // namespace

class ScrubRenameTaskTest : public testing::Test {
 protected:
  //  event {
  //    timestamp: 6702094131629195
  //    pid: 7971
  //    task_rename {
  //      pid: 7971
  //      oldcomm: "adbd"
  //      newcomm: "sh"
  //      oom_score_adj: -950
  //    }
  //  }
  protos::gen::FtraceEvent CreateRenameEvent(uint64_t ts, uint32_t pid) {
    protos::gen::FtraceEvent event;
    event.set_timestamp(ts);
    event.set_pid(pid);

    auto* rename = event.mutable_task_rename();
    rename->set_pid(static_cast<int32_t>(pid));
    rename->set_oldcomm("adbd");
    rename->set_newcomm("sh");
    rename->set_oom_score_adj(-950);

    return event;
  }

  //  event {
  //    timestamp: 6702094034179654
  //    pid: 7145
  //    sched_switch {
  //      prev_comm: "Job.worker 3"
  //      prev_pid: 7145
  //      prev_prio: 120
  //      prev_state: 1
  //      next_comm: "swapper/1"
  //      next_pid: 0
  //      next_prio: 120
  //    }
  //  }
  protos::gen::FtraceEvent CreateSomeEvent(uint64_t ts, uint32_t pid) {
    protos::gen::FtraceEvent event;
    event.set_timestamp(ts);
    event.set_pid(pid);

    auto* sched = event.mutable_sched_switch();
    sched->set_prev_comm("Job.worker 3");
    sched->set_prev_pid(static_cast<int32_t>(pid));
    sched->set_prev_prio(120);
    sched->set_prev_state(1);
    sched->set_next_comm("swapper/1");
    sched->set_next_pid(0);
    sched->set_next_prio(120);

    return event;
  }

  const FilterTaskRename& filter() const { return filter_; }

  Context* context() { return &context_; }

 private:
  Context context_;
  FilterTaskRename filter_;
};

TEST_F(ScrubRenameTaskTest, ReturnErrorForNoPackage) {
  context()->timeline.reset(new ProcessThreadTimeline());

  ASSERT_FALSE(filter().VerifyContext(*context()).ok());
}

TEST_F(ScrubRenameTaskTest, ReturnErrorForNoTimeline) {
  context()->package_name = "package name";
  context()->package_uid = kUid;

  ASSERT_FALSE(filter().VerifyContext(*context()).ok());
}

TEST_F(ScrubRenameTaskTest, KeepsNonRenameEvent) {
  context()->package_name = "package name";
  context()->package_uid = kUid;

  context()->timeline.reset(new ProcessThreadTimeline());

  auto event = CreateSomeEvent(kJustSomeTime, kPid).SerializeAsArray();
  ASSERT_TRUE(filter().KeepEvent(*context(), {event.data(), event.size()}));
}

TEST_F(ScrubRenameTaskTest, RejectsRenameEventOutsidePackage) {
  context()->package_name = "package name";
  context()->package_uid = kUid;

  // There's no connection between kPid and kUid. This means the rename packet
  // should be dropped.
  context()->timeline.reset(new ProcessThreadTimeline());

  auto event = CreateRenameEvent(kJustSomeTime, kPid).SerializeAsArray();
  ASSERT_FALSE(filter().KeepEvent(*context(), {event.data(), event.size()}));
}

TEST_F(ScrubRenameTaskTest, AcceptsRenameEventInPackage) {
  context()->package_name = "package name";
  context()->package_uid = kUid;

  context()->timeline.reset(new ProcessThreadTimeline());
  context()->timeline->Append(
      ProcessThreadTimeline::Event::Open(0, kPid, 0, kUid));
  context()->timeline->Sort();

  auto bytes = CreateRenameEvent(kJustSomeTime, kPid).SerializeAsArray();
  ASSERT_TRUE(filter().KeepEvent(*context(), {bytes.data(), bytes.size()}));
}

}  // namespace perfetto::trace_redaction
