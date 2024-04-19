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

#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/filter_packet_using_allowlist.h"
#include "src/trace_redaction/scrub_trace_packet.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ps/process_tree.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

TEST(FilterPacketUsingAllowlistParamErrorTest, ReturnErrorForNullPacket) {
  ScrubTracePacket transform_;
  transform_.emplace_back<FilterPacketUsingAllowlist>();

  // Have something in the allow-list to avoid that error.
  Context context;
  context.trace_packet_allow_list.insert(
      protos::pbzero::TracePacket::kProcessTreeFieldNumber);

  ASSERT_FALSE(transform_.Transform(context, nullptr).ok());
}

TEST(FilterPacketUsingAllowlistParamErrorTest, ReturnErrorForEmptyPacket) {
  ScrubTracePacket transform_;
  transform_.emplace_back<FilterPacketUsingAllowlist>();

  // Have something in the allow-list to avoid that error.
  Context context;
  context.trace_packet_allow_list.insert(
      protos::pbzero::TracePacket::kProcessTreeFieldNumber);

  std::string packet_str = "";

  ASSERT_FALSE(transform_.Transform(context, &packet_str).ok());
}

class FilterPacketUsingAllowlistTest : public testing::Test {
 protected:
  void SetUp() override {
    transform_.emplace_back<FilterPacketUsingAllowlist>();
  }

  base::StatusOr<std::string> Redact(const protos::gen::TracePacket& packet) {
    auto str = packet.SerializeAsString();
    auto status = transform_.Transform(context_, &str);

    if (status.ok()) {
      return str;
    }

    return status;
  }

  Context context_;

 private:
  ScrubTracePacket transform_;
};

TEST_F(FilterPacketUsingAllowlistTest, ReturnErrorForEmptyAllowList) {
  // The context will have no allow-list entries. ScrubTracePacket should fail.

  protos::gen::TracePacket packet;

  auto status = Redact(packet);
  ASSERT_FALSE(status.ok()) << status.status().c_message();
}

// The whole packet should be dropped (cleared) when it has a data type not
// included in the allow-list.
TEST_F(FilterPacketUsingAllowlistTest, DropsOutsiderPacketType) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(1234);
  packet.mutable_android_camera_frame_event();  // Creates and sets data.

  // Populate the allow-list with something that doesn't match the data in the
  // packet.
  context_.trace_packet_allow_list.insert(
      protos::pbzero::TracePacket::kProcessTreeFieldNumber);

  auto status = Redact(packet);
  ASSERT_OK(status) << status.status().c_message();

  ASSERT_TRUE(status->empty());
}

// Typically a trace packet should always have a data type (e.g. ProcessTree),
// but it is possible that another transformation has cleared that data. If
// that's the case, this primitive should treat it as an outsider.
TEST_F(FilterPacketUsingAllowlistTest, DropsPacketsWithNoType) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(1234);

  std::string packet_str = packet.SerializeAsString();
  ASSERT_GT(packet_str.size(), 0u);

  context_.trace_packet_allow_list.insert(
      protos::pbzero::TracePacket::kProcessTreeFieldNumber);

  auto status = Redact(packet);
  ASSERT_OK(status) << status.status().c_message();

  ASSERT_TRUE(status->empty());
}

// A packet should not change (at all) if it's in the allow-list.
TEST_F(FilterPacketUsingAllowlistTest, SkipsAllowedPacket) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(1234);

  // Add a process tree to the packet. Process trees are in the allow-list.
  auto* process = packet.mutable_process_tree()->add_processes();
  process->set_uid(0);
  process->set_ppid(3);
  process->set_pid(7);

  context_.trace_packet_allow_list.insert(
      protos::pbzero::TracePacket::kProcessTreeFieldNumber);

  auto status = Redact(packet);
  ASSERT_OK(status) << status.status().c_message();

  // The transform shouldn't have changed the string, so the string before and
  // after should match.
  ASSERT_EQ(*status, packet.SerializeAsString());
}

}  // namespace perfetto::trace_redaction
