/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_redaction/filter_untrusted_app_packets.h"

#include <cstdint>
#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;

constexpr int32_t kTargetUid = 10146;
constexpr int32_t kOtherUid = 10200;
constexpr int32_t kTracedProbesUid = 9999;

class FilterUntrustedAppPacketsTest : public testing::Test {
 protected:
  void SetUp() override {
    context_.package_name = "com.example.target";
    context_.package_uid = kTargetUid;
    context_.normalize_uid = true;
  }

  Context context_;
};

TEST_F(FilterUntrustedAppPacketsTest, DropsOtherApps) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(kOtherUid);
  packet->set_process_tree();
  std::string bytes = packet.SerializeAsString();

  ASSERT_OK(FilterUntrustedAppPackets().Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
  EXPECT_EQ(context_.stats.untrusted_app_packets_dropped, 1u);
}

TEST_F(FilterUntrustedAppPacketsTest, KeepsOthers) {
  for (int32_t uid : {kTargetUid, kTracedProbesUid, 1000}) {
    protozero::HeapBuffered<TracePacket> packet;
    packet->set_trusted_uid(uid);
    packet->set_process_tree();
    std::string bytes = packet.SerializeAsString();
    std::string original = bytes;

    ASSERT_OK(FilterUntrustedAppPackets().Transform(context_, &bytes));
    EXPECT_EQ(bytes, original);
  }
  EXPECT_EQ(context_.stats.untrusted_app_packets_dropped, 0u);
}

}  // namespace
}  // namespace perfetto::trace_redaction
