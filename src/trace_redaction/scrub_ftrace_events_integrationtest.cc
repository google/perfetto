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
#include <string_view>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "src/base/test/status_matchers.h"
#include "src/base/test/utils.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace//ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/android/packages_list.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

namespace {
using FtraceEvent = protos::pbzero::FtraceEvent;
using PackagesList = protos::pbzero::PackagesList;
using PackageInfo = protos::pbzero::PackagesList::PackageInfo;
using Trace = protos::pbzero::Trace;
using TracePacket = protos::pbzero::TracePacket;

constexpr std::string_view kTracePath =
    "test/data/trace-redaction-general.pftrace";

// Runs ScrubFtraceEvents over an actual trace, verifying packet integrity when
// fields are removed.
class ScrubFtraceEventsIntegrationTest : public testing::Test {
 public:
  ScrubFtraceEventsIntegrationTest() = default;
  ~ScrubFtraceEventsIntegrationTest() override = default;

 protected:
  void SetUp() override {
    src_trace_ = base::GetTestDataPath(std::string(kTracePath));
    context_.ftrace_packet_allow_list.insert(
        protos::pbzero::FtraceEvent::kSchedSwitchFieldNumber);
  }

  std::string src_trace_;

  Context context_;  // Used for allowlist.
  ScrubFtraceEvents transform_;

  static base::StatusOr<std::string> ReadRawTrace(const std::string& path) {
    std::string redacted_buffer;

    if (base::ReadFile(path, &redacted_buffer)) {
      return redacted_buffer;
    }

    return base::ErrStatus("Failed to read %s", path.c_str());
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
  const auto& src_file = src_trace_;

  auto raw_src_trace = ReadRawTrace(src_file);
  ASSERT_OK(raw_src_trace);

  protos::pbzero::Trace::Decoder source_trace(raw_src_trace.value());

  for (auto packet_it = source_trace.packet(); packet_it; ++packet_it) {
    auto packet = packet_it->as_std_string();
    ASSERT_OK(transform_.Transform(context_, &packet));

    protos::pbzero::TracePacket::Decoder left_packet(*packet_it);
    protos::pbzero::TracePacket::Decoder right_packet(packet);

    ComparePackets(std::move(left_packet), std::move(right_packet));
  }
}

}  // namespace
}  // namespace perfetto::trace_redaction
