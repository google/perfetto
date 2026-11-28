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

#include "src/trace_redaction/prune_missing_uid_packets.h"

#include <cstdint>
#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/heap_dump_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;

constexpr char kTargetPackage[] = "com.example.target";
constexpr int32_t kTargetUid = 10146;
constexpr int32_t kTracedProbesUid = 9999;

class AllowSynchronizationMarker final : public PacketFilter {
 public:
  bool Includes(const Context&, const std::string& packet) const override {
    return TracePacket::Decoder(packet).has_synchronization_marker();
  }
};

class PruneMissingUidPacketsTest : public testing::Test {
 protected:
  void SetUp() override {
    context_.package_name = kTargetPackage;
    context_.package_uid = kTargetUid;
    context_.normalize_uid = true;
    transform_.emplace_allow<AllowTargetDeobfuscationMapping>();
  }

  Context context_;
  PruneMissingUidPackets transform_;
};

TEST_F(PruneMissingUidPacketsTest, DropsAllUidLessPacketsWhenAllowlistIsEmpty) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_deobfuscation_mapping()->set_package_name(kTargetPackage);
  std::string bytes = packet.SerializeAsString();

  ASSERT_OK(PruneMissingUidPackets().Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(PruneMissingUidPacketsTest, KeepsPacketAcceptedByAnyAllowlistFilter) {
  transform_.emplace_allow<AllowSynchronizationMarker>();

  constexpr uint8_t kSync[] = {1, 2, 3, 4};
  protozero::HeapBuffered<TracePacket> marker;
  marker->set_synchronization_marker(kSync, sizeof(kSync));
  std::string marker_bytes = marker.SerializeAsString();
  std::string marker_original = marker_bytes;
  ASSERT_OK(transform_.Transform(context_, &marker_bytes));
  EXPECT_EQ(marker_bytes, marker_original);
  EXPECT_EQ(context_.stats.missing_uid_packets_dropped, 0u);

  protozero::HeapBuffered<TracePacket> tree;
  tree->set_process_tree();
  std::string tree_bytes = tree.SerializeAsString();
  ASSERT_OK(transform_.Transform(context_, &tree_bytes));
  EXPECT_TRUE(tree_bytes.empty());
  EXPECT_EQ(context_.stats.missing_uid_packets_dropped, 1u);
}

TEST_F(PruneMissingUidPacketsTest, KeepsTargetMapping) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_deobfuscation_mapping()->set_package_name(kTargetPackage);
  std::string bytes = packet.SerializeAsString();
  std::string original = bytes;

  ASSERT_OK(transform_.Transform(context_, &bytes));
  EXPECT_EQ(bytes, original);
}

TEST_F(PruneMissingUidPacketsTest, DropsOtherMapping) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_deobfuscation_mapping()->set_package_name("com.example.other");
  std::string bytes = packet.SerializeAsString();

  ASSERT_OK(transform_.Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(PruneMissingUidPacketsTest, DropsMixedPacket) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_deobfuscation_mapping()->set_package_name(kTargetPackage);
  packet->set_process_tree();
  std::string bytes = packet.SerializeAsString();

  ASSERT_OK(transform_.Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
}

// A second mapping in the same packet must not ride along with the target's.
TEST_F(PruneMissingUidPacketsTest, DropsTwoMappings) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_deobfuscation_mapping()->set_package_name(kTargetPackage);
  packet->set_deobfuscation_mapping()->set_package_name("com.example.other");
  std::string bytes = packet.SerializeAsString();

  ASSERT_OK(transform_.Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(PruneMissingUidPacketsTest, DropsTwoNames) {
  protozero::HeapBuffered<TracePacket> packet;
  auto* mapping = packet->set_deobfuscation_mapping();
  mapping->set_package_name(kTargetPackage);
  mapping->set_package_name("com.example.other");
  std::string bytes = packet.SerializeAsString();

  ASSERT_OK(transform_.Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(PruneMissingUidPacketsTest, IgnoresUidPackets) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(kTracedProbesUid);
  packet->set_process_tree();
  std::string bytes = packet.SerializeAsString();
  std::string original = bytes;

  ASSERT_OK(transform_.Transform(context_, &bytes));
  EXPECT_EQ(bytes, original);
}

}  // namespace
}  // namespace perfetto::trace_redaction
