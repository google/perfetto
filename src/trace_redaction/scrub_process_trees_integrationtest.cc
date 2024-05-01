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

#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/collect_timeline_events.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/optimize_timeline.h"
#include "src/trace_redaction/scrub_process_trees.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redaction_integration_fixture.h"
#include "src/trace_redaction/trace_redactor.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"

namespace perfetto::trace_redaction {

namespace {

constexpr std::string_view kProcessName =
    "com.Unity.com.unity.multiplayer.samples.coop";

}  // namespace

class ScrubProcessTreesIntegrationTest
    : public testing::Test,
      protected TraceRedactionIntegrationFixure {
 protected:
  void SetUp() override {
    // ScrubProcessTrees depends on:
    //    - FindPackageUid    (creates: uid)
    //    - OptimizeTimeline  (creates: optimized timeline)
    //
    // OptimizeTimeline depends on:
    //    - FindPackageUid (uses: uid)
    //    - CollectTimelineEvents  (uses: timeline)
    //
    // CollectTimelineEvents depends on.... nothing
    // FindPackageUid depends on... nothing

    trace_redactor()->emplace_collect<FindPackageUid>();
    trace_redactor()->emplace_collect<CollectTimelineEvents>();
    trace_redactor()->emplace_build<OptimizeTimeline>();
    trace_redactor()->emplace_transform<ScrubProcessTrees>();

    // In this case, the process and package have the same name.
    context()->package_name = kProcessName;
  }

  std::vector<std::string> CollectProcessNames(
      protos::pbzero::Trace::Decoder trace) const {
    std::vector<std::string> names;

    for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
      protos::pbzero::TracePacket::Decoder packet(*packet_it);

      if (!packet.has_process_tree()) {
        continue;
      }

      protos::pbzero::ProcessTree::Decoder process_tree(packet.process_tree());

      for (auto process_it = process_tree.processes(); process_it;
           ++process_it) {
        protos::pbzero::ProcessTree::Process::Decoder process(*process_it);

        if (process.has_cmdline()) {
          names.push_back(process.cmdline()->as_std_string());
        }
      }
    }

    return names;
  }
};

TEST_F(ScrubProcessTreesIntegrationTest, RemovesProcessNamesFromProcessTrees) {
  ASSERT_OK(Redact());

  auto original_trace_str = LoadOriginal();
  ASSERT_OK(original_trace_str);

  auto redacted_trace_str = LoadRedacted();
  ASSERT_OK(redacted_trace_str);

  protos::pbzero::Trace::Decoder original_trace(original_trace_str.value());
  auto original_processes = CollectProcessNames(std::move(original_trace));

  ASSERT_GT(original_processes.size(), 1u);

  protos::pbzero::Trace::Decoder redacted_trace(redacted_trace_str.value());
  auto redacted_processes = CollectProcessNames(std::move(redacted_trace));

  ASSERT_EQ(redacted_processes.size(), 1u);
  ASSERT_EQ(redacted_processes.at(0), kProcessName);
}

}  // namespace perfetto::trace_redaction
