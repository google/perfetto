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

#include "src/trace_redaction/scrub_trace_packet.h"

#include <string>

#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/ps/process_tree.gen.h"

namespace perfetto::trace_redaction {

// The whole packet should be dropped (cleared) when it has a data type not
// included in the allow-list.
TEST(ScrubTracePacketTest, DropsOutsiderPacketType) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(1234);
  packet.mutable_android_camera_frame_event();  // Creates and sets data.

  std::string packet_str = packet.SerializeAsString();
  ASSERT_GT(packet_str.size(), 0u);

  Context ignore;  // The context is not important for this primitive.

  ScrubTracePacket scrub;
  ASSERT_OK(scrub.Transform(ignore, &packet_str));

  ASSERT_TRUE(packet_str.empty());
}

// Typically a trace packet should always have a data type (e.g. ProcessTree),
// but it is possible that another transformation has cleared that data. If
// that's the case, this primitive should treat it as an outsider.
TEST(ScrubTracePacketTest, DropsPacketsWithNoType) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(1234);

  std::string packet_str = packet.SerializeAsString();
  ASSERT_GT(packet_str.size(), 0u);

  Context ignore;  // The context is not important for this primitive.

  ScrubTracePacket scrub;
  ASSERT_OK(scrub.Transform(ignore, &packet_str));

  ASSERT_TRUE(packet_str.empty());
}

// A packet should not change (at all) if it's in the allow-list.
TEST(ScrubTracePacketTest, SkipsAllowedPacket) {
  protos::gen::TracePacket packet;
  packet.set_timestamp(1234);

  // Add a process tree to the packet. Process trees are in the allow-list.
  auto* process = packet.mutable_process_tree()->add_processes();
  process->set_uid(0);
  process->set_ppid(3);
  process->set_pid(7);

  std::string original_packet_str = packet.SerializeAsString();
  ASSERT_GT(original_packet_str.size(), 0u);

  // Make a copy that can be modified by the primitive (even though it shouldn't
  // be).
  std::string mutable_packet_str(original_packet_str);

  Context ignore;  // The context is not important for this primitive.

  ScrubTracePacket scrub;
  ASSERT_OK(scrub.Transform(ignore, &mutable_packet_str));

  // The transform shouldn't have changed the string, so the string before and
  // after should match.
  ASSERT_EQ(original_packet_str, mutable_packet_str);
}

}  // namespace perfetto::trace_redaction
