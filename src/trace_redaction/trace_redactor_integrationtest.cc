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
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/populate_allow_lists.h"
#include "src/trace_redaction/prune_package_list.h"
#include "src/trace_redaction/scrub_ftrace_events.h"
#include "src/trace_redaction/scrub_trace_packet.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"
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

constexpr uint64_t kPackageUid = 10252;

class TraceRedactorIntegrationTest : public testing::Test {
 public:
  TraceRedactorIntegrationTest() = default;
  ~TraceRedactorIntegrationTest() override = default;

 protected:
  void SetUp() override {
    src_trace_ = base::GetTestDataPath(std::string(kTracePath));

    // Add every primitive to the redactor. This should mirror the production
    // configuration. This configuration may differ to help with verifying the
    // results.
    redactor_.collectors()->emplace_back(new FindPackageUid());
    redactor_.builders()->emplace_back(new PopulateAllowlists());
    redactor_.transformers()->emplace_back(new PrunePackageList());
    redactor_.transformers()->emplace_back(new ScrubTracePacket());
    redactor_.transformers()->emplace_back(new ScrubFtraceEvents());

    // Set the package name to "just some package name". If a specific package
    // name is needed, it should overwrite this value.
    context_.package_name = "com.google.omadm.trigger";
  }

  const std::string& src_trace() const { return src_trace_; }

  std::vector<protozero::ConstBytes> GetPackageInfos(
      const Trace::Decoder& trace) const {
    std::vector<protozero::ConstBytes> infos;

    for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
      TracePacket::Decoder packet_decoder(*packet_it);
      if (packet_decoder.has_packages_list()) {
        PackagesList::Decoder list_it(packet_decoder.packages_list());
        for (auto info_it = list_it.packages(); info_it; ++info_it) {
          PackageInfo::Decoder info(*info_it);
          infos.push_back(*info_it);
        }
      }
    }

    return infos;
  }

  static base::StatusOr<std::string> ReadRawTrace(const std::string& path) {
    std::string redacted_buffer;

    if (base::ReadFile(path, &redacted_buffer)) {
      return redacted_buffer;
    }

    return base::ErrStatus("Failed to read %s", path.c_str());
  }

  // NOTE - this will include fields like "timestamp" and "pid".
  static void GetEventFields(const Trace::Decoder& trace,
                             base::FlatSet<uint32_t>* set) {
    for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
      TracePacket::Decoder packet(*packet_it);

      if (!packet.has_ftrace_events()) {
        continue;
      }

      protos::pbzero::FtraceEventBundle::Decoder bundle(packet.ftrace_events());

      if (!bundle.has_event()) {
        continue;
      }

      for (auto events_it = bundle.event(); events_it; ++events_it) {
        protozero::ProtoDecoder event(*events_it);

        for (auto event_it = event.ReadField(); event_it.valid();
             event_it = event.ReadField()) {
          set->insert(event_it.id());
        }
      }
    }
  }

  static base::StatusOr<protozero::ConstBytes> FindFirstFtraceEvents(
      const Trace::Decoder& trace) {
    for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
      TracePacket::Decoder packet(*packet_it);

      if (packet.has_ftrace_events()) {
        return packet.ftrace_events();
      }
    }

    return base::ErrStatus("Failed to find ftrace events");
  }

  std::string src_trace_;
  base::TmpDirTree tmp_dir_;

  Context context_;
  TraceRedactor redactor_;
};

TEST_F(TraceRedactorIntegrationTest, FindsPackageAndFiltersPackageList) {
  context_.package_name = "com.Unity.com.unity.multiplayer.samples.coop";

  auto result = redactor_.Redact(
      src_trace(), tmp_dir_.AbsolutePath("dst.pftrace"), &context_);
  tmp_dir_.TrackFile("dst.pftrace");

  ASSERT_OK(result);

  ASSERT_OK_AND_ASSIGN(auto redacted_buffer,
                       ReadRawTrace(tmp_dir_.AbsolutePath("dst.pftrace")));

  Trace::Decoder redacted_trace(redacted_buffer);
  std::vector<protozero::ConstBytes> infos = GetPackageInfos(redacted_trace);

  ASSERT_TRUE(context_.package_uid.has_value());
  ASSERT_EQ(NormalizeUid(context_.package_uid.value()),
            NormalizeUid(kPackageUid));

  // It is possible for two packages_list to appear in the trace. The
  // find_package_uid will stop after the first one is found. Package uids are
  // appear as n * 1,000,000 where n is some integer. It is also possible for
  // two packages_list to contain copies of each other - for example
  // "com.Unity.com.unity.multiplayer.samples.coop" appears in both
  // packages_list.
  ASSERT_EQ(infos.size(), 2u);

  std::array<PackageInfo::Decoder, 2> decoders = {
      PackageInfo::Decoder(infos[0]), PackageInfo::Decoder(infos[1])};

  for (auto& decoder : decoders) {
    ASSERT_TRUE(decoder.has_name());
    ASSERT_EQ(decoder.name().ToStdString(),
              "com.Unity.com.unity.multiplayer.samples.coop");

    ASSERT_TRUE(decoder.has_uid());
    ASSERT_EQ(NormalizeUid(decoder.uid()), NormalizeUid(kPackageUid));
  }
}

// It is possible for multiple packages to share a uid. The names will appears
// across multiple package lists. The only time the package name appears is in
// the package list, so there is no way to differentiate these packages (only
// the uid is used later), so each entry should remain.
TEST_F(TraceRedactorIntegrationTest, RetainsAllInstancesOfUid) {
  context_.package_name = "com.google.android.networkstack.tethering";

  auto result = redactor_.Redact(
      src_trace(), tmp_dir_.AbsolutePath("dst.pftrace"), &context_);
  tmp_dir_.TrackFile("dst.pftrace");
  ASSERT_OK(result);

  ASSERT_OK_AND_ASSIGN(auto redacted_buffer,
                       ReadRawTrace(tmp_dir_.AbsolutePath("dst.pftrace")));

  Trace::Decoder redacted_trace(redacted_buffer);
  std::vector<protozero::ConstBytes> infos = GetPackageInfos(redacted_trace);

  ASSERT_EQ(infos.size(), 8u);

  std::array<std::string, 8> package_names;

  for (size_t i = 0; i < infos.size(); ++i) {
    PackageInfo::Decoder info(infos[i]);
    ASSERT_TRUE(info.has_name());
    package_names[i] = info.name().ToStdString();
  }

  std::sort(package_names.begin(), package_names.end());
  ASSERT_EQ(package_names[0], "com.google.android.cellbroadcastservice");
  ASSERT_EQ(package_names[1], "com.google.android.cellbroadcastservice");
  ASSERT_EQ(package_names[2], "com.google.android.networkstack");
  ASSERT_EQ(package_names[3], "com.google.android.networkstack");
  ASSERT_EQ(package_names[4],
            "com.google.android.networkstack.permissionconfig");
  ASSERT_EQ(package_names[5],
            "com.google.android.networkstack.permissionconfig");
  ASSERT_EQ(package_names[6], "com.google.android.networkstack.tethering");
  ASSERT_EQ(package_names[7], "com.google.android.networkstack.tethering");
}

// Makes sure all not-allowed ftrace event is removed from a trace.
TEST_F(TraceRedactorIntegrationTest, RemovesFtraceEvents) {
  auto pre_redaction_file = src_trace();
  auto post_redaction_file = tmp_dir_.AbsolutePath("dst.pftrace");

  // We know that there are two oom score updates in the test trace. These
  // events are not in the allowlist and should be dropped.
  auto pre_redaction_buffer = ReadRawTrace(pre_redaction_file);
  ASSERT_OK(pre_redaction_buffer) << pre_redaction_buffer.status().message();
  Trace::Decoder pre_redaction_trace(*pre_redaction_buffer);

  base::FlatSet<uint32_t> pre_redaction_event_types;
  GetEventFields(pre_redaction_trace, &pre_redaction_event_types);
  ASSERT_GT(pre_redaction_event_types.count(
                FtraceEvent::kOomScoreAdjUpdateFieldNumber),
            0u);

  auto result =
      redactor_.Redact(pre_redaction_file, post_redaction_file, &context_);
  tmp_dir_.TrackFile("dst.pftrace");
  ASSERT_OK(result) << result.message();

  auto post_redaction_buffer = ReadRawTrace(post_redaction_file);
  ASSERT_OK(post_redaction_buffer) << post_redaction_buffer.status().message();
  Trace::Decoder post_redaction_trace(*post_redaction_buffer);

  base::FlatSet<uint32_t> post_redaction_event_types;
  GetEventFields(post_redaction_trace, &post_redaction_event_types);
  ASSERT_EQ(post_redaction_event_types.count(
                FtraceEvent::kOomScoreAdjUpdateFieldNumber),
            0u);
}

// When a event is dropped from ftrace_events, only that event should be droped,
// the other events in the ftrace_events should be retained.
TEST_F(TraceRedactorIntegrationTest,
       RetainsFtraceEventsWhenRemovingFtraceEvent) {
  auto pre_redaction_file = src_trace();
  auto post_redaction_file = tmp_dir_.AbsolutePath("dst.pftrace");

  auto pre_redaction_buffer = ReadRawTrace(pre_redaction_file);
  ASSERT_OK(pre_redaction_buffer) << pre_redaction_buffer.status().message();

  Trace::Decoder pre_redaction_trace(*pre_redaction_buffer);

  auto pre_redaction_first_events = FindFirstFtraceEvents(pre_redaction_trace);
  ASSERT_OK(pre_redaction_first_events)
      << pre_redaction_first_events.status().message();

  auto result =
      redactor_.Redact(pre_redaction_file, post_redaction_file, &context_);
  tmp_dir_.TrackFile("dst.pftrace");
  ASSERT_OK(result) << result.message();

  auto post_redaction_buffer = ReadRawTrace(post_redaction_file);
  ASSERT_OK(post_redaction_buffer) << post_redaction_buffer.status().message();

  Trace::Decoder post_redaction_trace(*post_redaction_buffer);

  auto post_redaction_ftrace_events =
      FindFirstFtraceEvents(post_redaction_trace);
  ASSERT_OK(post_redaction_ftrace_events)
      << post_redaction_ftrace_events.status().message();

  base::FlatSet<uint32_t> events_before;
  GetEventFields(pre_redaction_trace, &events_before);
  ASSERT_EQ(events_before.size(), 14u);
  ASSERT_TRUE(events_before.count(FtraceEvent::kTimestampFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kPidFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kPrintFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kSchedSwitchFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kCpuFrequencyFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kCpuIdleFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kSchedWakeupFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kSchedWakingFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kSchedWakeupNewFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kTaskNewtaskFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kTaskRenameFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kSchedProcessExitFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kSchedProcessFreeFieldNumber));
  ASSERT_TRUE(events_before.count(FtraceEvent::kOomScoreAdjUpdateFieldNumber));

  base::FlatSet<uint32_t> events_after;
  GetEventFields(post_redaction_trace, &events_after);
  ASSERT_EQ(events_after.size(), 9u);

  // Retained.
  ASSERT_TRUE(events_after.count(FtraceEvent::kTimestampFieldNumber));
  ASSERT_TRUE(events_after.count(FtraceEvent::kPidFieldNumber));
  ASSERT_TRUE(events_after.count(FtraceEvent::kSchedSwitchFieldNumber));
  ASSERT_TRUE(events_after.count(FtraceEvent::kCpuFrequencyFieldNumber));
  ASSERT_TRUE(events_after.count(FtraceEvent::kCpuIdleFieldNumber));
  ASSERT_TRUE(events_after.count(FtraceEvent::kSchedWakingFieldNumber));
  ASSERT_TRUE(events_after.count(FtraceEvent::kTaskNewtaskFieldNumber));
  ASSERT_TRUE(events_after.count(FtraceEvent::kTaskRenameFieldNumber));
  ASSERT_TRUE(events_after.count(FtraceEvent::kSchedProcessFreeFieldNumber));

  // Dropped.
  ASSERT_FALSE(events_after.count(FtraceEvent::kPrintFieldNumber));
  ASSERT_FALSE(events_after.count(FtraceEvent::kSchedWakeupFieldNumber));
  ASSERT_FALSE(events_after.count(FtraceEvent::kSchedWakeupNewFieldNumber));
  ASSERT_FALSE(events_after.count(FtraceEvent::kSchedProcessExitFieldNumber));
  ASSERT_FALSE(events_after.count(FtraceEvent::kOomScoreAdjUpdateFieldNumber));
}

}  // namespace
}  // namespace perfetto::trace_redaction
