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

#include "src/trace_redaction/scrub_trigger.h"

#include <cstdint>
#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trigger.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;

constexpr int32_t kTargetUid = 10146;
constexpr int32_t kOtherUid = 10200;
constexpr int32_t kTracedProbesUid = 9999;

class ScrubTriggerTest : public testing::Test {
 protected:
  void SetUp() override {
    context_.package_name = "com.example.target";
    context_.package_uid = kTargetUid;
    context_.normalize_uid = true;
  }

  Context context_;
};

TEST_F(ScrubTriggerTest, RemovesOtherProducer) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(kTracedProbesUid);
  auto* trigger = packet->set_trigger();
  trigger->set_trigger_name("oom");
  trigger->set_producer_name("com.example.other");
  trigger->set_trusted_producer_uid(kOtherUid);
  std::string bytes = packet.SerializeAsString();

  ASSERT_OK(ScrubTrigger().Transform(context_, &bytes));
  TracePacket::Decoder decoder(bytes);
  protos::pbzero::Trigger::Decoder scrubbed(decoder.trigger());
  EXPECT_TRUE(scrubbed.has_trigger_name());
  EXPECT_FALSE(scrubbed.has_producer_name());
  EXPECT_FALSE(scrubbed.has_trusted_producer_uid());
}

}  // namespace
}  // namespace perfetto::trace_redaction
