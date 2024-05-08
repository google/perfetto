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
#include "src/trace_redaction/collect_system_info.h"
#include "src/trace_redaction/collect_timeline_events.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/redact_ftrace_event.h"
#include "src/trace_redaction/remap_scheduling_events.h"
#include "src/trace_redaction/trace_redaction_integration_fixture.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"

namespace perfetto::trace_redaction {

// Runs ThreadMergeRemapFtraceEventPid, ThreadMergeRemapSchedSwitchPid,
// ThreadMergeRemapSchedWakingPid, and ThreadMergeDropField to replace pids with
// synthetic pids (for all threads outside of the target package);
class RemapSchedulingEventsIntegrationTest
    : public testing::Test,
      protected TraceRedactionIntegrationFixure {
 public:
  static constexpr auto kPackageName =
      "com.Unity.com.unity.multiplayer.samples.coop";
  static constexpr uint64_t kPackageId = 10252;
  static constexpr int32_t kPid = 7105;

  // Threads belonging to pid 7105. Collected using trace processors.
  static constexpr auto kTids = {
      0,  // pid 0 will always be included because CPU idle uses it.
      7105, 7111, 7112, 7113, 7114, 7115, 7116, 7117, 7118, 7119, 7120,
      7124, 7125, 7127, 7129, 7130, 7131, 7132, 7133, 7134, 7135, 7136,
      7137, 7139, 7141, 7142, 7143, 7144, 7145, 7146, 7147, 7148, 7149,
      7150, 7151, 7152, 7153, 7154, 7155, 7156, 7157, 7158, 7159, 7160,
      7161, 7162, 7163, 7164, 7165, 7166, 7167, 7171, 7172, 7174, 7178,
      7180, 7184, 7200, 7945, 7946, 7947, 7948, 7950, 7969,
  };

 protected:
  void SetUp() override {
    trace_redactor()->emplace_collect<FindPackageUid>();

    // In order to remap threads, we need to have synth threads.
    trace_redactor()->emplace_collect<CollectSystemInfo>();
    trace_redactor()->emplace_build<BuildSyntheticThreads>();

    // Timeline information is needed to know if a pid belongs to a package.
    trace_redactor()->emplace_collect<CollectTimelineEvents>();

    auto* redactions = trace_redactor()->emplace_transform<RedactFtraceEvent>();
    redactions->emplace_back<ThreadMergeRemapFtraceEventPid::kFieldId,
                             ThreadMergeRemapFtraceEventPid>();
    redactions->emplace_back<ThreadMergeRemapSchedSwitchPid::kFieldId,
                             ThreadMergeRemapSchedSwitchPid>();
    redactions->emplace_back<ThreadMergeRemapSchedWakingPid::kFieldId,
                             ThreadMergeRemapSchedWakingPid>();
    redactions->emplace_back<ThreadMergeDropField::kSchedProcessFreeFieldNumber,
                             ThreadMergeDropField>();
    redactions->emplace_back<ThreadMergeDropField::kTaskNewtaskFieldNumber,
                             ThreadMergeDropField>();

    context()->package_name = kPackageName;
  }

  struct Index {
    // List of FtraceEvent
    std::vector<protozero::ConstBytes> events;

    // List of SchedSwitchFtraceEvent
    std::vector<protozero::ConstBytes> events_sched_switch;

    // List of SchedWakingFtraceEvent
    std::vector<protozero::ConstBytes> events_sched_waking;

    // List of SchedProcessFreeFtraceEvent
    std::vector<protozero::ConstBytes> events_sched_process_free;

    // List of TaskNewtaskFtraceEvent
    std::vector<protozero::ConstBytes> events_task_newtask;
  };

  void UpdateFtraceIndex(protozero::ConstBytes bytes, Index* index) {
    protos::pbzero::FtraceEventBundle::Decoder bundle(bytes);

    for (auto event = bundle.event(); event; ++event) {
      index->events.push_back(event->as_bytes());

      // protos::pbzero::FtraceEvent
      protozero::ProtoDecoder ftrace_event(event->as_bytes());

      auto sched_switch = ftrace_event.FindField(
          protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber);
      if (sched_switch.valid()) {
        index->events_sched_switch.push_back(sched_switch.as_bytes());
      }

      auto sched_waking = ftrace_event.FindField(
          protos::pbzero::FtraceEvent::kSchedWakingFieldNumber);
      if (sched_waking.valid()) {
        index->events_sched_waking.push_back(sched_waking.as_bytes());
      }

      auto sched_process_free = ftrace_event.FindField(
          protos::pbzero::FtraceEvent::kSchedProcessFreeFieldNumber);
      if (sched_process_free.valid()) {
        index->events_sched_process_free.push_back(
            sched_process_free.as_bytes());
      }

      auto task_newtask = ftrace_event.FindField(
          protos::pbzero::FtraceEvent::kTaskNewtaskFieldNumber);
      if (task_newtask.valid()) {
        index->events_task_newtask.push_back(task_newtask.as_bytes());
      }
    }
  }

  // Bytes should be TracePacket
  Index CreateFtraceIndex(const std::string& bytes) {
    Index index;

    protozero::ProtoDecoder packet_decoder(bytes);

    for (auto packet = packet_decoder.ReadField(); packet.valid();
         packet = packet_decoder.ReadField()) {
      auto events = packet_decoder.FindField(
          protos::pbzero::TracePacket::kFtraceEventsFieldNumber);

      if (events.valid()) {
        UpdateFtraceIndex(events.as_bytes(), &index);
      }
    }

    return index;
  }

  base::StatusOr<std::string> LoadAndRedactTrace() {
    auto source = LoadOriginal();

    if (!source.ok()) {
      return source.status();
    }

    auto redact = Redact();

    if (!redact.ok()) {
      return redact;
    }

    // Double-check the package id with the one from trace processor. If this
    // was wrong and this check was missing, finding the problem would be much
    // harder.
    if (!context()->package_uid.has_value()) {
      return base::ErrStatus("Missing package uid.");
    }

    if (context()->package_uid.value() != kPackageId) {
      return base::ErrStatus("Unexpected package uid found.");
    }

    auto redacted = LoadRedacted();

    if (redacted.ok()) {
      return redacted;
    }

    // System info is used to initialize the synth threads. If these are wrong,
    // then the synth threads will be wrong.
    if (!context()->system_info.has_value()) {
      return base::ErrStatus("Missing system info.");
    }

    if (context()->system_info->last_cpu() != 7u) {
      return base::ErrStatus("Unexpected cpu count.");
    }

    // The synth threads should have been initialized. They will be used here to
    // verify which threads exist in the redacted trace.
    if (!context()->synthetic_threads.has_value()) {
      return base::ErrStatus("Missing synthetic threads.");
    }

    if (context()->synthetic_threads->tids.size() != 8u) {
      return base::ErrStatus("Unexpected synthentic thread count.");
    }

    return redacted;
  }

  // Should be called after redaction since it requires data from the context.
  std::unordered_set<int32_t> CopyAllowedTids(const Context& context) const {
    std::unordered_set<int32_t> tids(kTids.begin(), kTids.end());

    tids.insert(context.synthetic_threads->tgid);
    tids.insert(context.synthetic_threads->tids.begin(),
                context.synthetic_threads->tids.end());

    return tids;
  }

 private:
  std::unordered_set<int32_t> allowed_tids_;
};

TEST_F(RemapSchedulingEventsIntegrationTest, FilterFtraceEventPid) {
  auto redacted = LoadAndRedactTrace();
  ASSERT_OK(redacted);

  auto allowlist = CopyAllowedTids(*context());

  auto index = CreateFtraceIndex(*redacted);

  for (const auto& event : index.events) {
    protos::pbzero::FtraceEvent::Decoder decoder(event);
    auto pid = static_cast<int32_t>(decoder.pid());
    ASSERT_TRUE(allowlist.count(pid));
  }
}

TEST_F(RemapSchedulingEventsIntegrationTest, FiltersSchedSwitch) {
  auto redacted = LoadAndRedactTrace();
  ASSERT_OK(redacted);

  auto allowlist = CopyAllowedTids(*context());

  auto index = CreateFtraceIndex(*redacted);

  for (const auto& event : index.events_sched_switch) {
    protos::pbzero::SchedSwitchFtraceEvent::Decoder decoder(event);
    ASSERT_TRUE(allowlist.count(decoder.prev_pid()));
    ASSERT_TRUE(allowlist.count(decoder.next_pid()));
  }
}

TEST_F(RemapSchedulingEventsIntegrationTest, FiltersSchedWaking) {
  auto redacted = LoadAndRedactTrace();
  ASSERT_OK(redacted);

  auto allowlist = CopyAllowedTids(*context());

  auto index = CreateFtraceIndex(*redacted);

  for (const auto& event : index.events_sched_waking) {
    protos::pbzero::SchedWakingFtraceEvent::Decoder decoder(event);
    ASSERT_TRUE(allowlist.count(decoder.pid()));
  }
}

TEST_F(RemapSchedulingEventsIntegrationTest, FiltersProcessFree) {
  auto redacted = LoadAndRedactTrace();
  ASSERT_OK(redacted);

  auto allowlist = CopyAllowedTids(*context());

  auto index = CreateFtraceIndex(*redacted);

  for (const auto& event : index.events_sched_process_free) {
    protos::pbzero::SchedProcessFreeFtraceEvent::Decoder decoder(event);
    ASSERT_TRUE(allowlist.count(decoder.pid()));
  }
}

TEST_F(RemapSchedulingEventsIntegrationTest, FiltersNewTask) {
  auto redacted = LoadAndRedactTrace();
  ASSERT_OK(redacted);

  auto allowlist = CopyAllowedTids(*context());

  auto index = CreateFtraceIndex(*redacted);

  for (const auto& event : index.events_task_newtask) {
    protos::pbzero::TaskNewtaskFtraceEvent::Decoder decoder(event);
    ASSERT_TRUE(allowlist.count(decoder.pid()));
  }
}

}  // namespace perfetto::trace_redaction
