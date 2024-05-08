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

#include "src/trace_redaction/remap_scheduling_events.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"

namespace perfetto::trace_redaction {

template <class T>
class ThreadMergeTest {
 protected:
  struct Process {
    uint64_t uid;
    int32_t ppid;
    int32_t pid;
  };

  base::Status Redact(protos::pbzero::FtraceEvent* event_message) {
    T redact;

    auto bundle_str = bundle_.SerializeAsString();
    protos::pbzero::FtraceEventBundle::Decoder bundle_decoder(bundle_str);

    auto event_str = bundle_.event().back().SerializeAsString();
    protos::pbzero::FtraceEvent::Decoder event_decoder(event_str);

    return redact.Redact(context_, bundle_decoder, event_decoder,
                         event_message);
  }

  Context context_;
  protos::gen::FtraceEventBundle bundle_;
};

// All ftrace events have a timestamp and a pid. This test focuses on the
// event's pid value. When that pid doesn't belong to the target package, it
// should be replaced with a synthetic thread id.
//
//  event {
//    timestamp: 6702093743539938
//    pid: 0
//    sched_switch { ... }
//  }
class ThreadMergeRemapFtraceEventPidTest
    : public testing::Test,
      protected ThreadMergeTest<ThreadMergeRemapFtraceEventPid> {
 protected:
  static constexpr uint32_t kCpu = 3;

  static constexpr auto kTimestamp = 123456789;

  // This process will be connected to the target package.
  static constexpr Process kProcess = {12, 5, 7};

  // This process will not be connected to the target package.
  static constexpr Process kOtherProcess = {120, 50, 70};

  void SetUp() override {
    bundle_.add_event();

    context_.package_uid = kProcess.uid;

    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        0, kProcess.pid, kProcess.ppid, kProcess.uid));
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        0, kOtherProcess.pid, kOtherProcess.ppid, kOtherProcess.uid));
    context_.timeline->Sort();

    // Because kCpu is 3, it means that there are four CPUs (id 0, id 1, ...).
    context_.synthetic_threads.emplace();
    context_.synthetic_threads->tids.assign({100, 101, 102, 103});
  }
};

// This should never happen, a bundle should always have a cpu. If it doesn't
// have a CPU, the event field should be dropped (safest option).
//
// TODO(vaage): This will create an invalid trace. It can also leak information
// if other primitives don't strip the remaining information. To be safe, these
// cases should be replaced with errors.
TEST_F(ThreadMergeRemapFtraceEventPidTest, MissingCpuReturnsError) {
  // Do not call set_cpu(uint32_t value). There should be no cpu for this case.
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_FALSE(Redact(event_message.get()).ok());
}

// This should never happen, an event should always have a timestamp. If it
// doesn't have a timestamp, the event field should be dropped (safest option).
//
// TODO(vaage): This will create an invalid trace. It can also leak information
// if other primitives don't strip the remaining information. To be safe, these
// cases should be replaced with errors.
TEST_F(ThreadMergeRemapFtraceEventPidTest, MissingTimestampReturnsError) {
  bundle_.set_cpu(kCpu);
  // Do not call set_timestamp(uint64_t value). There should be no timestamp for
  // this case.
  bundle_.mutable_event()->back().set_pid(kProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_FALSE(Redact(event_message.get()).ok());
}

TEST_F(ThreadMergeRemapFtraceEventPidTest, NoopWhenPidIsInPackage) {
  bundle_.set_cpu(kCpu);
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_OK(Redact(event_message.get()));

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_pid());
  ASSERT_EQ(static_cast<int32_t>(event.pid()), kProcess.pid);
}

TEST_F(ThreadMergeRemapFtraceEventPidTest, ChangesPidWhenPidIsOutsidePackage) {
  bundle_.set_cpu(kCpu);  // The CPU is used to select the pid.
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kOtherProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_OK(Redact(event_message.get()));

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_pid());
  ASSERT_EQ(static_cast<int32_t>(event.pid()),
            context_.synthetic_threads->tids[kCpu]);
}

// When creating a sched_switch event, the event pid and the previous pid should
// be the same pid.
//
//  event {
//    timestamp: 6702093743539938
//    pid: 0
//    sched_switch {
//      prev_comm: "swapper/7"
//      prev_pid: 0
//      prev_prio: 120
//      prev_state: 0
//      next_comm: "FMOD stream thr"
//      next_pid: 7174
//      next_prio: 104
//    }
//  }
class ThreadMergeRemapSchedSwitchPidTest
    : public testing::Test,
      protected ThreadMergeTest<ThreadMergeRemapSchedSwitchPid> {
 protected:
  static constexpr uint32_t kCpu = 3;

  static constexpr auto kTimestamp = 123456789;

  // This process will be connected to the target package.
  static constexpr Process kPrevProcess = {12, 5, 7};
  static constexpr Process kNextProcess = {12, 5, 8};

  // This process will not be connected to the target package.
  static constexpr Process kOtherProcess = {120, 50, 70};

  void SetUp() override {
    bundle_.add_event();

    context_.package_uid = kPrevProcess.uid;

    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        0, kPrevProcess.pid, kPrevProcess.ppid, kPrevProcess.uid));
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        0, kNextProcess.pid, kNextProcess.ppid, kNextProcess.uid));
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        0, kOtherProcess.pid, kOtherProcess.ppid, kOtherProcess.uid));

    context_.timeline->Sort();

    // Because kCpu is 3, it means that there are four CPUs (id 0, id 1, ...).
    context_.synthetic_threads.emplace();
    context_.synthetic_threads->tids.assign({100, 101, 102, 103});
  }
};

// This should never happen, a bundle should always have a cpu. If it doesn't
// have a CPU, the event field should be dropped (safest option).
//
// TODO(vaage): This will create an invalid trace. It can also leak information
// if other primitives don't strip the remaining information. To be safe, these
// cases should be replaced with errors.
TEST_F(ThreadMergeRemapSchedSwitchPidTest, MissingCpuReturnsError) {
  // Do not call set_cpu(uint32_t value). There should be no cpu for this case.
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kPrevProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_FALSE(Redact(event_message.get()).ok());
}

// This should never happen, an event should always have a timestamp. If it
// doesn't have a timestamp, the event field should be dropped (safest option).
//
// TODO(vaage): This will create an invalid trace. It can also leak information
// if other primitives don't strip the remaining information. To be safe, these
// cases should be replaced with errors.
TEST_F(ThreadMergeRemapSchedSwitchPidTest, MissingTimestampReturnsError) {
  bundle_.set_cpu(kCpu);
  // Do not call set_timestamp(uint64_t value). There should be no timestamp for
  // this case.
  bundle_.mutable_event()->back().set_pid(kPrevProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_FALSE(Redact(event_message.get()).ok());
}

TEST_F(ThreadMergeRemapSchedSwitchPidTest, NoopWhenPidIsInPackage) {
  bundle_.set_cpu(kCpu);
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kPrevProcess.pid);

  auto* sched_switch = bundle_.mutable_event()->back().mutable_sched_switch();
  sched_switch->set_prev_pid(kPrevProcess.pid);
  sched_switch->set_next_pid(kNextProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_OK(Redact(event_message.get()));

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  ASSERT_TRUE(event.sched_switch().has_prev_pid());
  ASSERT_EQ(static_cast<int32_t>(event.sched_switch().prev_pid()),
            kPrevProcess.pid);

  ASSERT_TRUE(event.sched_switch().has_next_pid());
  ASSERT_EQ(static_cast<int32_t>(event.sched_switch().next_pid()),
            kNextProcess.pid);
}

TEST_F(ThreadMergeRemapSchedSwitchPidTest,
       ChangesPrevPidWhenPidIsOutsidePackage) {
  bundle_.set_cpu(kCpu);
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kPrevProcess.pid);

  auto* sched_switch = bundle_.mutable_event()->back().mutable_sched_switch();
  sched_switch->set_prev_pid(kOtherProcess.pid);
  sched_switch->set_next_pid(kNextProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_OK(Redact(event_message.get()));

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  ASSERT_TRUE(event.sched_switch().has_prev_pid());
  ASSERT_EQ(static_cast<int32_t>(event.sched_switch().prev_pid()),
            context_.synthetic_threads->tids[kCpu]);

  ASSERT_TRUE(event.sched_switch().has_next_pid());
  ASSERT_EQ(static_cast<int32_t>(event.sched_switch().next_pid()),
            kNextProcess.pid);
}

TEST_F(ThreadMergeRemapSchedSwitchPidTest,
       ChangesNextPidWhenPidIsOutsidePackage) {
  bundle_.set_cpu(kCpu);
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kPrevProcess.pid);

  auto* sched_switch = bundle_.mutable_event()->back().mutable_sched_switch();
  sched_switch->set_prev_pid(kPrevProcess.pid);
  sched_switch->set_next_pid(kOtherProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_OK(Redact(event_message.get()));

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_switch());

  ASSERT_TRUE(event.sched_switch().has_prev_pid());
  ASSERT_EQ(static_cast<int32_t>(event.sched_switch().prev_pid()),
            kPrevProcess.pid);

  ASSERT_TRUE(event.sched_switch().has_next_pid());
  ASSERT_EQ(static_cast<int32_t>(event.sched_switch().next_pid()),
            context_.synthetic_threads->tids[kCpu]);
}

//  event {
//    timestamp: 6702093743527386
//    pid: 0
//    sched_waking {
//      comm: "FMOD stream thr"
//      pid: 7174
//      prio: 104
//      success: 1
//      target_cpu: 7
//    }
//  }
class ThreadMergeRemapSchedWakingPidTest
    : public testing::Test,
      protected ThreadMergeTest<ThreadMergeRemapSchedWakingPid> {
 protected:
  static constexpr uint32_t kCpu = 3;

  static constexpr auto kTimestamp = 123456789;

  // This process will be connected to the target package.
  static constexpr Process kWakerProcess = {12, 5, 7};
  static constexpr Process kWakeTarget = {12, 5, 8};

  // This process will not be connected to the target package.
  static constexpr Process kOtherProcess = {120, 50, 70};

  void SetUp() override {
    bundle_.add_event();

    context_.package_uid = kWakerProcess.uid;

    context_.timeline = std::make_unique<ProcessThreadTimeline>();
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        0, kWakerProcess.pid, kWakerProcess.ppid, kWakerProcess.uid));
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        0, kWakeTarget.pid, kWakeTarget.ppid, kWakeTarget.uid));
    context_.timeline->Append(ProcessThreadTimeline::Event::Open(
        0, kOtherProcess.pid, kOtherProcess.ppid, kOtherProcess.uid));

    context_.timeline->Sort();

    // Because kCpu is 3, it means that there are four CPUs (id 0, id 1, ...).
    context_.synthetic_threads.emplace();
    context_.synthetic_threads->tids.assign({100, 101, 102, 103});
  }
};

// This should never happen, a bundle should always have a cpu. If it doesn't
// have a CPU, the event field should be dropped (safest option).
//
// TODO(vaage): This will create an invalid trace. It can also leak information
// if other primitives don't strip the remaining information. To be safe, these
// cases should be replaced with errors.
TEST_F(ThreadMergeRemapSchedWakingPidTest, MissingCpuReturnsError) {
  // Do not call set_cpu(uint32_t value). There should be no cpu for this case.
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kWakerProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_FALSE(Redact(event_message.get()).ok());
}

// This should never happen, an event should always have a timestamp. If it
// doesn't have a timestamp, the event field should be dropped (safest option).
//
// TODO(vaage): This will create an invalid trace. It can also leak information
// if other primitives don't strip the remaining information. To be safe, these
// cases should be replaced with errors.
TEST_F(ThreadMergeRemapSchedWakingPidTest, MissingTimestampReturnsError) {
  bundle_.set_cpu(kCpu);
  // Do not call set_timestamp(uint64_t value). There should be no timestamp for
  // this case.
  bundle_.mutable_event()->back().set_pid(kWakerProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_FALSE(Redact(event_message.get()).ok());
}

TEST_F(ThreadMergeRemapSchedWakingPidTest, NoopWhenPidIsInPackage) {
  bundle_.set_cpu(kCpu);
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kWakerProcess.pid);

  auto* sched_waking = bundle_.mutable_event()->back().mutable_sched_waking();
  sched_waking->set_pid(kWakeTarget.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_OK(Redact(event_message.get()));

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_waking());

  ASSERT_TRUE(event.sched_waking().has_pid());
  ASSERT_EQ(static_cast<int32_t>(event.sched_waking().pid()), kWakeTarget.pid);
}

TEST_F(ThreadMergeRemapSchedWakingPidTest, ChangesPidWhenPidIsOutsidePackage) {
  bundle_.set_cpu(kCpu);
  bundle_.mutable_event()->back().set_timestamp(kTimestamp);
  bundle_.mutable_event()->back().set_pid(kWakerProcess.pid);

  auto* sched_switch = bundle_.mutable_event()->back().mutable_sched_waking();
  sched_switch->set_pid(kOtherProcess.pid);

  protozero::HeapBuffered<protos::pbzero::FtraceEvent> event_message;
  ASSERT_OK(Redact(event_message.get()));

  protos::gen::FtraceEvent event;
  event.ParseFromString(event_message.SerializeAsString());

  ASSERT_TRUE(event.has_sched_waking());

  ASSERT_TRUE(event.sched_waking().has_pid());
  ASSERT_EQ(static_cast<int32_t>(event.sched_waking().pid()),
            context_.synthetic_threads->tids[kCpu]);
}

}  // namespace perfetto::trace_redaction
