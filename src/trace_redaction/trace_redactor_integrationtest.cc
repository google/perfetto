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

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/base/test/utils.h"
#include "src/trace_redaction/find_package_uid.h"
#include "src/trace_redaction/prune_package_list.h"
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
    "test/data/trace_redaction_jank_high_cpu.pftrace";

// "com.google.android.settings.intelligence" will have one package, but two
// processes will reference it. When doing so, they will use two different
// uids (multiples of 1,000,000).
constexpr std::string_view kPackageName =
    "com.google.android.settings.intelligence";
constexpr uint64_t kPackageUid = 10118;

class TraceRedactorIntegrationTest : public testing::Test {
 public:
  TraceRedactorIntegrationTest() = default;
  ~TraceRedactorIntegrationTest() override = default;

 protected:
  void SetUp() override {
    src_trace_ = base::GetTestDataPath(std::string(kTracePath));
    dest_trace_ = std::make_unique<base::TempFile>(base::TempFile::Create());
  }

  const std::string& src_trace() const { return src_trace_; }

  const std::string& dest_trace() const { return dest_trace_->path(); }

 private:
  std::string src_trace_;
  std::unique_ptr<base::TempFile> dest_trace_;
};

TEST_F(TraceRedactorIntegrationTest, FindsPackageAndFiltersPackageList) {
  TraceRedactor redaction;
  redaction.collectors()->emplace_back(new FindPackageUid());
  redaction.transformers()->emplace_back(new PrunePackageList());

  Context context;
  context.package_name = kPackageName;

  auto result = redaction.Redact(src_trace(), dest_trace(), &context);

  ASSERT_TRUE(result.ok()) << result.message();

  std::string redacted_buffer;
  ASSERT_TRUE(base::ReadFile(dest_trace(), &redacted_buffer));

  // Collect package info from the trace.
  std::vector<protozero::ConstBytes> infos;

  Trace::Decoder trace_decoder(redacted_buffer);

  for (auto packet_it = trace_decoder.packet(); packet_it; ++packet_it) {
    TracePacket::Decoder packet_decoder(*packet_it);

    if (packet_decoder.has_packages_list()) {
      PackagesList::Decoder list_it(packet_decoder.packages_list());

      for (auto info_it = list_it.packages(); info_it; ++info_it) {
        infos.push_back(*info_it);
      }
    }
  }

  ASSERT_EQ(infos.size(), 1u);

  PackageInfo::Decoder info(infos[0]);

  ASSERT_TRUE(info.has_name());
  ASSERT_EQ(info.name().ToStdString(), kPackageName);

  ASSERT_TRUE(info.has_uid());
  ASSERT_EQ(NormalizeUid(info.uid()), NormalizeUid(kPackageUid));

  ASSERT_TRUE(context.package_uid.has_value());
  ASSERT_EQ(NormalizeUid(context.package_uid.value()),
            NormalizeUid(kPackageUid));
}

}  // namespace
}  // namespace perfetto::trace_redaction
