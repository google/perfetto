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

#include "src/trace_redaction/prune_heap_dumps.h"

#include <cstdint>
#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/art/heap_graph.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;
using ArtHeapGraphTracePacket =
    com::android::art::tracing::pbzero::ArtHeapGraphTracePacket;

constexpr char kTargetPackage[] = "com.example.target";
constexpr int32_t kTargetUid = 10146;

class PruneHeapDumpsTest : public testing::Test {
 protected:
  void SetUp() override {
    context_.package_name = kTargetPackage;
    context_.package_uid = kTargetUid;
    context_.normalize_uid = true;
  }

  Context context_;
};

TEST_F(PruneHeapDumpsTest, DropsNonTargetSystemHeapPacket) {
  protozero::HeapBuffered<ArtHeapGraphTracePacket> packet;
  packet->set_trusted_uid(1000);
  packet->set_trusted_packet_sequence_id(4);
  packet->set_heap_graph()->set_pid(1);
  std::string bytes = packet.SerializeAsString();

  HeapDumpKey key;
  key.writer_uid = 1000;
  key.sequence_id = 4;
  HeapDumpSequence sequence;
  sequence.heap_graph_packets = 1;
  sequence.is_complete = true;
  context_.heap_dump_sequences[key] = sequence;

  ASSERT_OK(PruneHeapDumps().Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(PruneHeapDumpsTest, DropsIncompleteTargetDump) {
  protozero::HeapBuffered<ArtHeapGraphTracePacket> packet;
  packet->set_trusted_uid(kTargetUid);
  packet->set_trusted_packet_sequence_id(4);
  packet->set_heap_graph()->set_pid(1);
  std::string bytes = packet.SerializeAsString();

  HeapDumpKey key;
  key.writer_uid = kTargetUid;
  key.sequence_id = 4;
  HeapDumpSequence sequence;
  sequence.heap_graph_packets = 1;
  sequence.is_complete = false;
  context_.heap_dump_sequences[key] = sequence;

  ASSERT_OK(PruneHeapDumps().Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
}

TEST_F(PruneHeapDumpsTest, KeepsNonHeapPacketsUntouched) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(1000);
  packet->set_process_tree()->add_processes()->set_pid(1);
  std::string bytes = packet.SerializeAsString();
  std::string original = bytes;

  ASSERT_OK(PruneHeapDumps().Transform(context_, &bytes));
  EXPECT_EQ(bytes, original);
}

TEST_F(PruneHeapDumpsTest, KeepsMappingWrittenByTargetUid) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(kTargetUid);
  packet->set_deobfuscation_mapping()->set_package_name(kTargetPackage);
  std::string bytes = packet.SerializeAsString();
  std::string original = bytes;

  ASSERT_OK(PruneHeapDumps().Transform(context_, &bytes));
  EXPECT_EQ(bytes, original);
  EXPECT_EQ(context_.stats.deobfuscation_mappings_kept, 1u);
}

TEST_F(PruneHeapDumpsTest, DropsMappingWrittenByNonTargetUid) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(1000);
  packet->set_deobfuscation_mapping()->set_package_name(kTargetPackage);
  std::string bytes = packet.SerializeAsString();

  ASSERT_OK(PruneHeapDumps().Transform(context_, &bytes));
  EXPECT_TRUE(bytes.empty());
  EXPECT_EQ(context_.stats.deobfuscation_mappings_kept, 0u);
}

TEST_F(PruneHeapDumpsTest, KeepsCompleteTargetDump) {
  protozero::HeapBuffered<ArtHeapGraphTracePacket> packet;
  packet->set_trusted_uid(kTargetUid);
  packet->set_trusted_packet_sequence_id(4);
  packet->set_heap_graph()->set_pid(1);
  std::string bytes = packet.SerializeAsString();
  std::string original = bytes;

  HeapDumpKey key;
  key.writer_uid = kTargetUid;
  key.sequence_id = 4;
  HeapDumpSequence sequence;
  sequence.heap_graph_packets = 1;
  sequence.is_complete = true;
  context_.heap_dump_sequences[key] = sequence;

  ASSERT_OK(PruneHeapDumps().Transform(context_, &bytes));
  EXPECT_EQ(bytes, original);
}

TEST_F(PruneHeapDumpsTest, KeepsCompleteSystemUidTargetDump) {
  context_.package_uid = 1000;

  protozero::HeapBuffered<ArtHeapGraphTracePacket> packet;
  packet->set_trusted_uid(1000);
  packet->set_trusted_packet_sequence_id(4);
  packet->set_heap_graph()->set_pid(1);
  std::string bytes = packet.SerializeAsString();
  std::string original = bytes;

  HeapDumpKey key;
  key.writer_uid = 1000;
  key.sequence_id = 4;
  HeapDumpSequence sequence;
  sequence.heap_graph_packets = 1;
  sequence.is_complete = true;
  context_.heap_dump_sequences[key] = sequence;

  ASSERT_OK(PruneHeapDumps().Transform(context_, &bytes));
  EXPECT_EQ(bytes, original);
}

}  // namespace
}  // namespace perfetto::trace_redaction
