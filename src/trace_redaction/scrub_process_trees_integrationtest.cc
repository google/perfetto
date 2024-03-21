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
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "src/base/test/status_matchers.h"
#include "src/base/test/tmp_dir_tree.h"
#include "src/base/test/utils.h"
#include "src/trace_redaction/build_timeline.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/optimize_timeline.h"
#include "src/trace_redaction/scrub_process_trees.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_redaction {

namespace {

constexpr std::string_view kTracePath =
    "test/data/trace-redaction-general.pftrace";
constexpr std::string_view kProcessName =
    "com.Unity.com.unity.multiplayer.samples.coop";

class ScrubProcessTreesIntegrationTest : public testing::Test {
 protected:
  void SetUp() override {
    src_trace_ = base::GetTestDataPath(std::string(kTracePath));

    // ScrubProcessTrees depends on:
    //    - FindPackageUid    (uid)
    //    - OptimizeTimeline  (sealed + optimized timeline)
    //
    // OptimizeTimeline depends on:
    //    - FindPackageUid (uid)
    //    - BuildTimeline  (timeline)
    //
    // BuildTimeline depends on.... nothing
    // FindPackageUid depends on... nothing

    redactor_.collectors()->emplace_back(new FindPackageUid());
    redactor_.collectors()->emplace_back(new BuildTimeline());
    redactor_.builders()->emplace_back(new OptimizeTimeline());
    redactor_.transformers()->emplace_back(new ScrubProcessTrees());

    // In this case, the process and package have the same name.
    context_.package_name = kProcessName;

    dest_trace_ = tmp_dir_.AbsolutePath("dst.pftrace");
    tmp_dir_.TrackFile("dst.pftrace");
  }

  static base::StatusOr<std::string> ReadRawTrace(const std::string& path) {
    std::string redacted_buffer;

    if (base::ReadFile(path, &redacted_buffer)) {
      return redacted_buffer;
    }

    return base::ErrStatus("Failed to read %s", path.c_str());
  }

  std::string src_trace_;
  std::string dest_trace_;

  base::TmpDirTree tmp_dir_;

  Context context_;
  TraceRedactor redactor_;
};

TEST_F(ScrubProcessTreesIntegrationTest, RemovesProcessNamesFromProcessTrees) {
  ASSERT_OK(redactor_.Redact(src_trace_, dest_trace_, &context_));
  ASSERT_OK_AND_ASSIGN(auto redacted_buffer, ReadRawTrace(dest_trace_));

  protos::pbzero::Trace::Decoder trace(redacted_buffer);

  for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
    protos::pbzero::TracePacket::Decoder packet(*packet_it);

    if (!packet.has_process_tree()) {
      continue;
    }

    protos::pbzero::ProcessTree::Decoder process_tree(packet.process_tree());

    for (auto process_it = process_tree.processes(); process_it; ++process_it) {
      protos::pbzero::ProcessTree::Process::Decoder process(*process_it);

      std::vector<std::string> cmdline;
      for (auto cmd_it = process.cmdline(); cmd_it; ++cmd_it) {
        cmdline.push_back(cmd_it->as_std_string());
      }

      // It's okay to be empty.
      if (cmdline.empty()) {
        continue;
      }

      if (cmdline.size() == 1) {
        ASSERT_EQ(cmdline[0], kProcessName);
        continue;
      }

      // If there are more than
      for (const auto& token : cmdline) {
        ASSERT_TRUE(token.empty());
      }
    }
  }
}

}  // namespace
}  // namespace perfetto::trace_redaction
