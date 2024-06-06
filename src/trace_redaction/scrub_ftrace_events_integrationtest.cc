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

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/redact_ftrace_events.h"
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

    auto* redact = trace_redactor()->emplace_transform<RedactFtraceEvents>();
    redact->emplace_ftrace_filter<FilterFtracesUsingAllowlist>();
    redact->emplace_post_filter_modifier<DoNothing>();
  }

  // Gets spans for `event` messages that contain `sched_switch` messages.
  void FindAllEvents(const protos::pbzero::TracePacket::Decoder& packet,
                     std::vector<protozero::ConstBytes>* events) const {
    if (!packet.has_ftrace_events()) {
      return;
    }

    protos::pbzero::FtraceEventBundle::Decoder ftrace_events(
        packet.ftrace_events());

    for (auto it = ftrace_events.event(); it; ++it) {
      events->push_back(*it);
    }
  }

  std::vector<protozero::ConstBytes> FindAllEvents(
      const std::string& data) const {
    protos::pbzero::Trace::Decoder decoder(data);
    std::vector<protozero::ConstBytes> events;

    for (auto it = decoder.packet(); it; ++it) {
      protos::pbzero::TracePacket::Decoder packet(*it);
      FindAllEvents(packet, &events);
    }

    return events;
  }

  static bool IsNotSwitchEvent(protozero::ConstBytes field) {
    protos::pbzero::FtraceEvent::Decoder event(field);
    return event.has_sched_switch();
  }
};

TEST_F(ScrubFtraceEventsIntegrationTest, FindsPackageAndFiltersPackageList) {
  ASSERT_OK(Redact());

  // Load unredacted trace - make sure there are non-allow-listed events.
  {
    auto raw = LoadOriginal();
    ASSERT_OK(raw);

    auto fields = FindAllEvents(*raw);

    // More than switch events should be found.
    auto it = std::find_if(fields.begin(), fields.end(), IsNotSwitchEvent);
    ASSERT_NE(it, fields.end());
  }

  // Load redacted trace - make sure there are only allow-listed events.
  {
    auto raw = LoadRedacted();
    ASSERT_OK(raw);

    auto field = FindAllEvents(*raw);

    // Only switch events should be found.
    auto it = std::find_if_not(field.begin(), field.end(), IsNotSwitchEvent);
    ASSERT_EQ(it, field.end());
  }
}

}  // namespace perfetto::trace_redaction
