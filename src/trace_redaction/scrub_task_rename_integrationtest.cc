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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "src/base/test/status_matchers.h"
#include "src/base/test/tmp_dir_tree.h"
#include "src/base/test/utils.h"
#include "src/trace_redaction/build_timeline.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/optimize_timeline.h"
#include "src/trace_redaction/scrub_task_rename.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

namespace {
using FtraceEvent = protos::pbzero::FtraceEvent;

constexpr std::string_view kTracePath =
    "test/data/trace-redaction-general.pftrace";

// Set the package name to "just some package name". If a specific package name
// is needed, the test it should overwrite this value.
constexpr std::string_view kPackageName =
    "com.Unity.com.unity.multiplayer.samples.coop";

class RenameEventsTraceRedactorIntegrationTest : public testing::Test {
 protected:
  void SetUp() override {
    // In order for ScrubTaskRename to work, it needs the timeline. All
    // registered primitives are there to generate the timeline.
    redactor_.collectors()->emplace_back(new FindPackageUid());
    redactor_.collectors()->emplace_back(new BuildTimeline());
    redactor_.builders()->emplace_back(new OptimizeTimeline());
    redactor_.transformers()->emplace_back(new ScrubTaskRename());

    context_.package_name = kPackageName;

    src_trace_ = base::GetTestDataPath(std::string(kTracePath));

    dest_trace_ = tmp_dir_.AbsolutePath("dst.pftrace");
    tmp_dir_.TrackFile("dst.pftrace");
  }

  base::Status Redact() {
    return redactor_.Redact(src_trace_, dest_trace_, &context_);
  }

  base::StatusOr<std::string> LoadOriginal() const {
    return ReadRawTrace(src_trace_);
  }

  base::StatusOr<std::string> LoadRedacted() const {
    return ReadRawTrace(dest_trace_);
  }

  std::vector<uint32_t> GetAllRenamedPids(
      protos::pbzero::Trace::Decoder trace) const {
    std::vector<uint32_t> renamed_pids;

    for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
      protos::pbzero::TracePacket::Decoder packet_decoder(*packet_it);

      if (!packet_decoder.has_ftrace_events()) {
        continue;
      }

      protos::pbzero::FtraceEventBundle::Decoder bundle_decoder(
          packet_decoder.ftrace_events());

      for (auto event_it = bundle_decoder.event(); event_it; ++event_it) {
        protos::pbzero::FtraceEvent::Decoder event(*event_it);

        if (event.has_task_rename()) {
          renamed_pids.push_back(event.pid());
        }
      }
    }

    return renamed_pids;
  }

 private:
  base::StatusOr<std::string> ReadRawTrace(const std::string& path) const {
    std::string redacted_buffer;

    if (base::ReadFile(path, &redacted_buffer)) {
      return redacted_buffer;
    }

    return base::ErrStatus("Failed to read %s", path.c_str());
  }

  Context context_;
  TraceRedactor redactor_;

  base::TmpDirTree tmp_dir_;

  std::string src_trace_;
  std::string dest_trace_;
};

TEST_F(RenameEventsTraceRedactorIntegrationTest, RemovesUnwantedRenameTasks) {
  auto result = Redact();
  ASSERT_OK(result) << result.c_message();

  auto original = LoadOriginal();
  ASSERT_OK(original) << original.status().c_message();

  auto redacted = LoadRedacted();
  ASSERT_OK(redacted) << redacted.status().c_message();

  auto original_rename_pids =
      GetAllRenamedPids(protos::pbzero::Trace::Decoder(*original));
  std::sort(original_rename_pids.begin(), original_rename_pids.end());

  // The test trace has found rename events. This assert is just to document
  // theme.
  ASSERT_EQ(original_rename_pids.size(), 4u);
  ASSERT_EQ(original_rename_pids[0], 7971u);
  ASSERT_EQ(original_rename_pids[1], 7972u);
  ASSERT_EQ(original_rename_pids[2], 7973u);
  ASSERT_EQ(original_rename_pids[3], 7974u);

  auto redacted_rename_pids =
      GetAllRenamedPids(protos::pbzero::Trace::Decoder(*redacted));
  ASSERT_TRUE(redacted_rename_pids.empty());
}

}  // namespace
}  // namespace perfetto::trace_redaction
