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

#include "src/trace_redaction/prune_shell_packets.h"

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;

constexpr int32_t kTargetUid = 10146;
constexpr int32_t kTracedProbesUid = 9999;

class PruneShellPacketsTest : public testing::Test {
 protected:
  void SetUp() override {
    context_.package_name = "com.example.target";
    context_.package_uid = kTargetUid;
    context_.normalize_uid = true;
  }

  Context context_;
};

TEST_F(PruneShellPacketsTest, RemovesShells) {
  // Real remnant of a traced extension_descriptor packet.
  protozero::HeapBuffered<TracePacket> shell;
  shell->set_trusted_uid(kTracedProbesUid);
  shell->set_trusted_packet_sequence_id(1);
  shell->set_timestamp_clock_id(6);
  shell->set_timestamp(4804032990072);
  shell->set_trusted_pid(761);
  std::string bytes = shell.SerializeAsString();

  ASSERT_OK(PruneShellPackets().Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(PruneShellPacketsTest, KeepsContent) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(1000);
  packet->set_process_tree();
  std::string bytes = packet.SerializeAsString();
  std::string original = bytes;

  ASSERT_OK(PruneShellPackets().Transform(context_, &bytes));
  EXPECT_EQ(bytes, original);
}

// Metadata that affects trace processing is content for this primitive.
TEST_F(PruneShellPacketsTest, KeepsMeaningfulMetadata) {
  std::vector<std::string> packets;
  {
    protozero::HeapBuffered<TracePacket> p;
    p->set_trusted_uid(9999);
    p->set_trusted_packet_sequence_id(2);
    p->set_previous_packet_dropped(true);
    packets.push_back(p.SerializeAsString());
  }
  {
    protozero::HeapBuffered<TracePacket> p;
    p->set_trusted_uid(9999);
    p->set_trusted_packet_sequence_id(2);
    p->set_first_packet_on_sequence(true);
    packets.push_back(p.SerializeAsString());
  }
  {
    protozero::HeapBuffered<TracePacket> p;
    p->set_trusted_uid(9999);
    p->set_trusted_packet_sequence_id(2);
    p->set_sequence_flags(TracePacket::SEQ_INCREMENTAL_STATE_CLEARED);
    packets.push_back(p.SerializeAsString());
  }
  {
    protozero::HeapBuffered<TracePacket> p;
    p->set_trusted_uid(9999);
    p->set_trusted_packet_sequence_id(2);
    p->set_interned_data();
    packets.push_back(p.SerializeAsString());
  }
  {
    protozero::HeapBuffered<TracePacket> p;
    p->set_trusted_uid(9999);
    p->set_machine_id(3);
    packets.push_back(p.SerializeAsString());
  }

  for (auto& bytes : packets) {
    std::string original = bytes;
    ASSERT_OK(PruneShellPackets().Transform(context_, &bytes));
    EXPECT_EQ(bytes, original);
  }
}

}  // namespace
}  // namespace perfetto::trace_redaction
