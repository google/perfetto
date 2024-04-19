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

#include "src/trace_redaction/filter_sched_waking_events.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/sched.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {
namespace {
constexpr int32_t kPackageUid = 1;
}  // namespace

class FilterSchedWakingEventsTest : public testing::Test {
 protected:
  void SetUp() override { transform_.emplace_back<FilterSchedWakingEvents>(); }

  void BeginBundle() { ftrace_bundle_ = trace_packet_.mutable_ftrace_events(); }

  void AddWaking(uint64_t ts, int32_t pid, std::string_view comm) {
    ASSERT_NE(ftrace_bundle_, nullptr);

    auto* event = ftrace_bundle_->add_event();
    event->set_timestamp(ts);

    auto* sched_waking = event->mutable_sched_waking();
    sched_waking->set_pid(pid);
    sched_waking->set_comm(std::string(comm));
  }

  const ScrubFtraceEvents& transform() const { return transform_; }

  // event {
  //   timestamp: 6702093757720043
  //   pid: 0
  //   sched_switch {
  //     prev_comm: "swapper/0"
  //     prev_pid: 0
  //     prev_prio: 120
  //     prev_state: 0
  //     next_comm: "Job.worker 5"
  //     next_pid: 7147
  //     next_prio: 120
  //   }
  // }
  protos::gen::FtraceEvent* CreateSchedSwitchEvent(
      protos::gen::FtraceEvent* event) {
    event->set_timestamp(6702093757720043);
    event->set_pid(0);

    auto* sched_switch = event->mutable_sched_switch();
    sched_switch->set_prev_comm("swapper/0");
    sched_switch->set_prev_pid(0);
    sched_switch->set_prev_prio(120);
    sched_switch->set_prev_state(0);
    sched_switch->set_next_comm("Job.worker 6");
    sched_switch->set_next_pid(7147);
    sched_switch->set_next_prio(120);

    return event;
  }

  // event {
  //   timestamp: 6702093757727075
  //   pid: 7147                    <- This pid woke up...
  //   sched_waking {
  //     comm: "Job.worker 6"
  //     pid: 7148                  <- ... this pid
  //     prio: 120
  //     success: 1
  //     target_cpu: 6
  //   }
  // }
  protos::gen::FtraceEvent* CreateSchedWakingEvent(
      protos::gen::FtraceEvent* event) {
    event->set_timestamp(6702093757727075);
    event->set_pid(7147);

    auto* sched_waking = event->mutable_sched_waking();
    sched_waking->set_comm("Job.worker 6");
    sched_waking->set_pid(7148);
    sched_waking->set_prio(120);
    sched_waking->set_success(1);
    sched_waking->set_target_cpu(6);

    return event;
  }

 private:
  protos::gen::TracePacket trace_packet_;
  protos::gen::FtraceEventBundle* ftrace_bundle_;

  ScrubFtraceEvents transform_;
};

TEST_F(FilterSchedWakingEventsTest, ReturnsErrorForNullPacket) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.package_uid = kPackageUid;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  ASSERT_FALSE(transform().Transform(context, nullptr).ok());
}

TEST_F(FilterSchedWakingEventsTest, ReturnsErrorForEmptyPacket) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.package_uid = kPackageUid;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  std::string packet_str = "";

  ASSERT_FALSE(transform().Transform(context, &packet_str).ok());
}

TEST_F(FilterSchedWakingEventsTest, ReturnsErrorForNoTimeline) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.package_uid = kPackageUid;

  protos::gen::TracePacket packet;
  std::string packet_str = packet.SerializeAsString();

  ASSERT_FALSE(transform().Transform(context, &packet_str).ok());
}

TEST_F(FilterSchedWakingEventsTest, ReturnsErrorForMissingPackage) {
  // Don't use context_. These tests will use invalid contexts.
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();

  protos::gen::TracePacket packet;
  std::string packet_str = packet.SerializeAsString();

  ASSERT_FALSE(transform().Transform(context, &packet_str).ok());
}

// Assume that the traces has a series of events like the events below. All
// constants will come from these packets:
//
// event {
//   timestamp: 6702093757720043
//   pid: 0
//   sched_switch {
//     prev_comm: "swapper/0"
//     prev_pid: 0
//     prev_prio: 120
//     prev_state: 0
//     next_comm: "Job.worker 5"
//     next_pid: 7147
//     next_prio: 120
//   }
// }
// event {
//   timestamp: 6702093757727075
//   pid: 7147                    <- This pid woke up...
//   sched_waking {
//     comm: "Job.worker 6"
//     pid: 7148                  <- ... this pid
//     prio: 120
//     success: 1
//     target_cpu: 6
//   }
// }
//
// The waking event is configured to be retained (see
// KeepsWakingWhenBothPidsConnectToPackage for more information on how). Because
// this transform only affects waking events, the sched switch event should be
// retain.
TEST_F(FilterSchedWakingEventsTest, RetainsNonWakingEvents) {
  std::string packet_str;

  {
    protos::gen::TracePacket packet;
    auto* events = packet.mutable_ftrace_events();
    events->set_cpu(0);

    CreateSchedSwitchEvent(events->add_event());
    CreateSchedWakingEvent(events->add_event());

    packet_str = packet.SerializeAsString();
  }

  // Create a timeline where the wake-target (7147 & 7148) is connected to the
  // target package.
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();
  context.package_uid = kPackageUid;
  context.timeline->Append(ProcessThreadTimeline::Event::Open(
      6702093757720043, 7147, 0, kPackageUid));
  context.timeline->Append(ProcessThreadTimeline::Event::Open(
      6702093757720043, 7148, 0, kPackageUid));
  context.timeline->Sort();

  ASSERT_TRUE(transform().Transform(context, &packet_str).ok());

  {
    protos::gen::TracePacket packet;
    packet.ParseFromString(packet_str);

    ASSERT_TRUE(packet.has_ftrace_events());

    const protos::gen::FtraceEvent* switch_it = nullptr;
    const protos::gen::FtraceEvent* waking_it = nullptr;

    for (const auto& event : packet.ftrace_events().event()) {
      if (event.has_sched_switch()) {
        switch_it = &event;
      }

      if (event.has_sched_waking()) {
        waking_it = &event;
      }
    }

    // The sched switch event should be here because this primitive should not
    // affect it.
    //
    // The sched waking event should be here because the waker and target
    // connect to the target package.
    ASSERT_TRUE(switch_it);
    ASSERT_TRUE(waking_it);
  }
}

// Assume that the traces has a series of events like the events below. All
// constants will come from these packets:
//
// event {
//   timestamp: 6702093757727075
//   pid: 7147                    <- This pid woke up...
//   sched_waking {
//     comm: "Job.worker 6"
//     pid: 7148                  <- ... this pid
//     prio: 120
//     success: 1
//     target_cpu: 6
//   }
// }
//
// Because the sched waking event pid's appears in the timeline and is connected
// to the target package (kPackageUid), the waking even should remain.
TEST_F(FilterSchedWakingEventsTest, KeepsWhenBothPidsConnectToPackage) {
  std::string packet_str;

  {
    protos::gen::TracePacket packet;
    auto* events = packet.mutable_ftrace_events();
    events->set_cpu(0);

    CreateSchedWakingEvent(events->add_event());

    packet_str = packet.SerializeAsString();
  }

  // Create a timeline where the wake-target (7147 & 7148) is connected to the
  // target package.
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();
  context.package_uid = kPackageUid;
  context.timeline->Append(ProcessThreadTimeline::Event::Open(
      6702093757720043, 7147, 0, kPackageUid));
  context.timeline->Append(ProcessThreadTimeline::Event::Open(
      6702093757720043, 7148, 0, kPackageUid));
  context.timeline->Sort();

  ASSERT_TRUE(transform().Transform(context, &packet_str).ok());

  {
    protos::gen::TracePacket packet;
    packet.ParseFromString(packet_str);

    ASSERT_TRUE(packet.has_ftrace_events());

    const protos::gen::FtraceEvent* waking_it = nullptr;

    for (const auto& event : packet.ftrace_events().event()) {
      if (event.has_sched_waking()) {
        waking_it = &event;
      }
    }

    ASSERT_TRUE(waking_it);

    const auto& waking = waking_it->sched_waking();

    ASSERT_EQ(waking.comm(), "Job.worker 6");
    ASSERT_EQ(waking.pid(), 7148);
    ASSERT_EQ(waking.prio(), 120);
    ASSERT_EQ(waking.success(), 1);
    ASSERT_EQ(waking.target_cpu(), 6);
  }
}

// Assume that the traces has a series of events like the events below. All
// constants will come from these packets:
//
// event {
//   timestamp: 6702093757727075
//   pid: 7147                    <- This pid woke up...
//   sched_waking {
//     comm: "Job.worker 6"
//     pid: 7148                  <- ... this pid
//     prio: 120
//     success: 1
//     target_cpu: 6
//   }
// }
//
// Because only one of the sched waking events pid's appears in the
// timeline and is connected to the target package (kPackageUid), the waking
// even should be removed.
TEST_F(FilterSchedWakingEventsTest, DropWhenOnlyWakerConnectsToPackage) {
  std::string packet_str;

  {
    protos::gen::TracePacket packet;
    auto* events = packet.mutable_ftrace_events();
    events->set_cpu(0);

    CreateSchedWakingEvent(events->add_event());

    packet_str = packet.SerializeAsString();
  }

  // Because 7147 is not added to the timeline, the waking event should not be
  // retained.
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();
  context.package_uid = kPackageUid;
  context.timeline->Append(ProcessThreadTimeline::Event::Open(
      6702093757720043, 7148, 0, kPackageUid));
  context.timeline->Sort();

  ASSERT_TRUE(transform().Transform(context, &packet_str).ok());

  {
    protos::gen::TracePacket packet;
    packet.ParseFromString(packet_str);

    ASSERT_TRUE(packet.has_ftrace_events());

    const protos::gen::FtraceEvent* waking_it = nullptr;

    for (const auto& event : packet.ftrace_events().event()) {
      if (event.has_sched_waking()) {
        waking_it = &event;
      }
    }

    ASSERT_FALSE(waking_it);
  }
}

// Assume that the traces has a series of events like the events below. All
// constants will come from these packets:
//
// event {
//   timestamp: 6702093757727075
//   pid: 7147                    <- This pid woke up...
//   sched_waking {
//     comm: "Job.worker 6"
//     pid: 7148                  <- ... this pid
//     prio: 120
//     success: 1
//     target_cpu: 6
//   }
// }
//
// Because the only one of the sched waking events pid's appears in the
// timeline and is connected to the target package (kPackageUid), the waking
// even should remain.
TEST_F(FilterSchedWakingEventsTest, DropWhenOnlyTargetConnectsToPackage) {
  std::string packet_str;

  {
    protos::gen::TracePacket packet;
    auto* events = packet.mutable_ftrace_events();
    events->set_cpu(0);

    CreateSchedWakingEvent(events->add_event());

    packet_str = packet.SerializeAsString();
  }

  // Because 7147 is not added to the timeline, the waking event should not be
  // retained.
  Context context;
  context.timeline = std::make_unique<ProcessThreadTimeline>();
  context.package_uid = kPackageUid;
  context.timeline->Append(ProcessThreadTimeline::Event::Open(
      6702093757720043, 7147, 0, kPackageUid));
  context.timeline->Sort();

  ASSERT_TRUE(transform().Transform(context, &packet_str).ok());

  {
    protos::gen::TracePacket packet;
    packet.ParseFromString(packet_str);

    ASSERT_TRUE(packet.has_ftrace_events());

    const protos::gen::FtraceEvent* waking_it = nullptr;

    for (const auto& event : packet.ftrace_events().event()) {
      if (event.has_sched_waking()) {
        waking_it = &event;
      }
    }

    ASSERT_FALSE(waking_it);
  }
}
}  // namespace perfetto::trace_redaction
