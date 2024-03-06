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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/base/test/tmp_dir_tree.h"
#include "src/base/test/utils.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/prune_package_list.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/android/packages_list.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"

namespace perfetto::trace_redaction {

namespace {
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

  std::string src_trace_;
  base::TmpDirTree tmp_dir_;
};

TEST_F(TraceRedactorIntegrationTest, FindsPackageAndFiltersPackageList) {
  TraceRedactor redaction;
  redaction.collectors()->emplace_back(new FindPackageUid());
  redaction.transformers()->emplace_back(new PrunePackageList());

  Context context;
  context.package_name = "com.Unity.com.unity.multiplayer.samples.coop";

  auto result = redaction.Redact(
      src_trace(), tmp_dir_.AbsolutePath("dst.pftrace"), &context);
  tmp_dir_.TrackFile("dst.pftrace");

  ASSERT_TRUE(result.ok()) << result.message();

  std::string redacted_buffer;
  ASSERT_TRUE(
      base::ReadFile(tmp_dir_.AbsolutePath("dst.pftrace"), &redacted_buffer));

  Trace::Decoder redacted_trace(redacted_buffer);
  std::vector<protozero::ConstBytes> infos = GetPackageInfos(redacted_trace);

  ASSERT_TRUE(context.package_uid.has_value());
  ASSERT_EQ(NormalizeUid(context.package_uid.value()),
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
  TraceRedactor redaction;
  redaction.collectors()->emplace_back(new FindPackageUid());
  redaction.transformers()->emplace_back(new PrunePackageList());

  Context context;
  context.package_name = "com.google.android.networkstack.tethering";

  auto result = redaction.Redact(
      src_trace(), tmp_dir_.AbsolutePath("dst.pftrace"), &context);
  tmp_dir_.TrackFile("dst.pftrace");
  ASSERT_TRUE(result.ok()) << result.message();

  std::string redacted_buffer;
  ASSERT_TRUE(
      base::ReadFile(tmp_dir_.AbsolutePath("dst.pftrace"), &redacted_buffer));

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

}  // namespace
}  // namespace perfetto::trace_redaction
