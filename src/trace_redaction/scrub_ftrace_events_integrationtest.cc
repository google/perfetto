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

#include <vector>

#include "perfetto/base/status.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redaction_integration_fixture.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace//ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

// Runs ScrubFtraceEvents over an actual trace, verifying packet integrity when
// fields are removed.
class ScrubFtraceEventsIntegrationTest
    : public testing::Test,
      protected TraceRedactionIntegrationFixure {
 public:
  ScrubFtraceEventsIntegrationTest() = default;
  ~ScrubFtraceEventsIntegrationTest() override = default;

 protected:
  void SetUp() override {
    context()->ftrace_packet_allow_list.insert(
        protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber);

    trace_redactor()->emplace_transform<ScrubFtraceEvents>();
  }

  // Gets spans for `event` messages that contain `sched_switch` messages.
  static std::vector<protozero::ConstBytes> GetEventsWithSchedSwitch(
      protos::pbzero::TracePacket::Decoder packet) {
    std::vector<protozero::ConstBytes> ranges;

    if (!packet.has_ftrace_events()) {
      return ranges;
    }

    protos::pbzero::FtraceEventBundle::Decoder bundle(packet.ftrace_events());

    if (!bundle.has_event()) {
      return ranges;
    }

    for (auto event_it = bundle.event(); event_it; ++event_it) {
      protos::pbzero::FtraceEvent::Decoder event(*event_it);

      if (event.has_sched_switch()) {
        ranges.push_back(*event_it);
      }
    }

    return ranges;
  }

  // Instead of using the allow-list created by PopulateAllowlist, use a simpler
  // allowlist; an allowlist that contains most value types.
  //
  // uint64....FtraceEvent...............timestamp
  // uint32....FtraceEvent...............pid
  //
  // int32.....SchedSwitchFtraceEvent....prev_pid
  // int64.....SchedSwitchFtraceEvent....prev_state
  // string....SchedSwitchFtraceEvent....next_comm
  //
  // Compare all switch events in each trace. The comparison is only on the
  // switch packets, not on the data leading up to or around them.
  static void ComparePackets(protos::pbzero::TracePacket::Decoder left,
                             protos::pbzero::TracePacket::Decoder right) {
    auto left_switches = GetEventsWithSchedSwitch(std::move(left));
    auto right_switches = GetEventsWithSchedSwitch(std::move(right));

    ASSERT_EQ(left_switches.size(), right_switches.size());

    auto left_switch_it = left_switches.begin();
    auto right_switch_it = right_switches.begin();

    while (left_switch_it != left_switches.end() &&
           right_switch_it != right_switches.end()) {
      auto left_switch_str = left_switch_it->ToStdString();
      auto right_switch_str = right_switch_it->ToStdString();

      ASSERT_EQ(left_switch_str, right_switch_str);

      ++left_switch_it;
      ++right_switch_it;
    }

    ASSERT_EQ(left_switches.size(), right_switches.size());
  }
};

TEST_F(ScrubFtraceEventsIntegrationTest, FindsPackageAndFiltersPackageList) {
  auto redacted = Redact();
  ASSERT_OK(redacted) << redacted.message();

  // Load source.
  auto before_raw_trace = LoadOriginal();
  ASSERT_OK(before_raw_trace) << before_raw_trace.status().message();
  protos::pbzero::Trace::Decoder before_trace(before_raw_trace.value());
  auto before_it = before_trace.packet();

  // Load redacted.
  auto after_raw_trace = LoadRedacted();
  ASSERT_OK(after_raw_trace) << after_raw_trace.status().message();
  protos::pbzero::Trace::Decoder after_trace(after_raw_trace.value());
  auto after_it = after_trace.packet();

  while (before_it && after_it) {
    protos::pbzero::TracePacket::Decoder before_packet(*before_it);
    protos::pbzero::TracePacket::Decoder after_packet(*after_it);

    ComparePackets(std::move(before_packet), std::move(after_packet));

    ++before_it;
    ++after_it;
  }

  // Both should be at the end.
  ASSERT_FALSE(before_it);
  ASSERT_FALSE(after_it);
}

}  // namespace perfetto::trace_redaction
