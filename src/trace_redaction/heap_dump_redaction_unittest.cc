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

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_redaction/filter_untrusted_app_packets.h"
#include "src/trace_redaction/heap_dump_util.h"
#include "src/trace_redaction/prune_heap_dumps.h"
#include "src/trace_redaction/prune_missing_uid_packets.h"
#include "src/trace_redaction/prune_shell_packets.h"
#include "src/trace_redaction/scrub_service_event.h"
#include "src/trace_redaction/scrub_trigger.h"
#include "src/trace_redaction/trace_redactor.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/android/packages_list.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.pbzero.h"
#include "protos/perfetto/trace/profiling/art_process_metadata.pbzero.h"
#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/ps/process_stats.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trigger.pbzero.h"
#include "protos/third_party/android/art/heap_graph.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;
using ArtHeapGraphTracePacket =
    com::android::art::tracing::pbzero::ArtHeapGraphTracePacket;

constexpr char kTargetPackage[] = "com.example.target";
constexpr char kOtherPackage[] = "com.example.other";
constexpr int32_t kTargetUid = 10146;
constexpr int32_t kOtherUid = 10200;
constexpr int32_t kPhoneUid = 1001;
constexpr int32_t kTracedProbesUid = 9999;

constexpr int32_t kTargetPid = 100;
constexpr int32_t kOtherPid = 200;

// Mirrors the real APC sample: heap dumps first, then process stats, then the
// only process snapshot.
constexpr uint64_t kProcessStartTs = 50;
constexpr uint64_t kHeapDumpTs = 100;
constexpr uint64_t kProcessStatsTs = 200;
constexpr uint64_t kProcessTreeTs = 300;

std::string HeapGraphPacket(int32_t trusted_uid,
                            uint32_t seq,
                            int32_t pid,
                            bool continued) {
  protozero::HeapBuffered<ArtHeapGraphTracePacket> packet;
  packet->set_trusted_uid(trusted_uid);
  packet->set_trusted_packet_sequence_id(seq);
  packet->set_timestamp(kHeapDumpTs);
  auto* graph = packet->set_heap_graph();
  graph->set_pid(pid);
  graph->add_objects()->set_id(1);
  if (continued) {
    graph->set_continued(true);
  }
  return packet.SerializeAsString();
}

std::string ArtMetadataPacket(int32_t trusted_uid,
                              uint32_t seq,
                              int32_t pid,
                              const std::string& package) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(trusted_uid);
  packet->set_trusted_packet_sequence_id(seq);
  packet->set_timestamp(kHeapDumpTs);
  auto* metadata = packet->set_art_process_metadata();
  metadata->set_pid(pid);
  metadata->set_uid(trusted_uid);
  metadata->set_package_name(package);
  return packet.SerializeAsString();
}

std::string MappingPacket(const std::string& package) {
  protozero::HeapBuffered<TracePacket> packet;
  auto* mapping = packet->set_deobfuscation_mapping();
  mapping->set_package_name(package);
  auto* cls = mapping->add_obfuscated_classes();
  cls->set_obfuscated_name("a");
  cls->set_deobfuscated_name("com.example.Real");
  return packet.SerializeAsString();
}

std::string PackagesListPacket(int32_t trusted_uid) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(trusted_uid);
  auto* list = packet->set_packages_list();
  auto* target = list->add_packages();
  target->set_name(kTargetPackage);
  target->set_uid(kTargetUid);
  auto* other = list->add_packages();
  other->set_name(kOtherPackage);
  other->set_uid(kOtherUid);
  return packet.SerializeAsString();
}

// A packages list written by an app that maps the target to another app.
std::string SpoofedPackagesListPacket(int32_t trusted_uid) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(trusted_uid);
  auto* target = packet->set_packages_list()->add_packages();
  target->set_name(kTargetPackage);
  target->set_uid(kOtherUid);
  return packet.SerializeAsString();
}

std::string ProcessTreePacket(int32_t trusted_uid,
                              std::vector<std::pair<int32_t, int32_t>> procs) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(trusted_uid);
  packet->set_timestamp(kProcessTreeTs);
  auto* tree = packet->set_process_tree();
  for (auto [pid, uid] : procs) {
    auto* process = tree->add_processes();
    process->set_pid(pid);
    process->set_ppid(1);
    process->set_uid(uid);
    process->set_process_start_from_boot(kProcessStartTs);
  }
  return packet.SerializeAsString();
}

std::string ProcessStatsPacket() {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(kTracedProbesUid);
  packet->set_timestamp(kProcessStatsTs);
  auto* stats = packet->set_process_stats();
  stats->add_processes()->set_pid(kTargetPid);
  stats->add_processes()->set_pid(kOtherPid);
  return packet.SerializeAsString();
}

std::string TriggerPacket(int32_t producer_uid, const std::string& producer) {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_trusted_uid(kTracedProbesUid);
  auto* trigger = packet->set_trigger();
  trigger->set_trigger_name("com.android.telemetry.art-outofmemory");
  trigger->set_producer_name(producer);
  trigger->set_trusted_producer_uid(producer_uid);
  return packet.SerializeAsString();
}

std::string BuildTrace(const std::vector<std::string>& packets) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;
  for (const auto& p : packets) {
    trace->add_packet()->AppendRawProtoBytes(p.data(), p.size());
  }
  return trace.SerializeAsString();
}

std::vector<std::string> SplitTrace(const std::string& trace) {
  std::vector<std::string> packets;
  protos::pbzero::Trace::Decoder decoder(trace);
  for (auto it = decoder.packet(); it; ++it) {
    packets.push_back(it->as_std_string());
  }
  return packets;
}

// Returns the ids of fields inside "oneof data" present in a packet.
std::vector<uint32_t> Payloads(const std::string& packet) {
  std::vector<uint32_t> ids;
  protozero::ProtoDecoder decoder(packet);
  for (auto f = decoder.ReadField(); f.valid(); f = decoder.ReadField()) {
    if (!IsTracePacketNonDataField(f.id())) {
      ids.push_back(f.id());
    }
  }
  return ids;
}

std::vector<int32_t> HeapGraphWriters(const std::vector<std::string>& packets) {
  std::vector<int32_t> uids;
  for (const auto& p : packets) {
    protozero::ProtoDecoder decoder(p);
    if (decoder.FindField(kHeapGraphFieldNumber).valid()) {
      uids.push_back(
          decoder.FindField(TracePacket::kTrustedUidFieldNumber).as_int32());
    }
  }
  return uids;
}

std::string UidLessProcessTreePacket() {
  protozero::HeapBuffered<TracePacket> packet;
  packet->set_timestamp(kProcessTreeTs);
  auto* process = packet->set_process_tree()->add_processes();
  process->set_pid(kOtherPid);
  process->set_uid(kOtherUid);
  return packet.SerializeAsString();
}

// The multi-app trace used by the end-to-end tests. Heap dumps come before
// the package list in file order on purpose.
std::vector<std::string> MultiAppTrace() {
  return {
      // Target dump: two packets on sequence 4, complete.
      HeapGraphPacket(kTargetUid, 4, kTargetPid, /*continued=*/true),
      ArtMetadataPacket(kTargetUid, 4, kTargetPid, kTargetPackage),
      HeapGraphPacket(kTargetUid, 4, kTargetPid, /*continued=*/false),
      // Another app's dump that claims the target's pid.
      HeapGraphPacket(kOtherUid, 5, kTargetPid, /*continued=*/false),
      ArtMetadataPacket(kOtherUid, 5, kOtherPid, kOtherPackage),
      // A system-uid Java process (com.android.phone) also OOMed.
      HeapGraphPacket(kPhoneUid, 6, 300, /*continued=*/false),
      // An app trying to poison attribution.
      SpoofedPackagesListPacket(kOtherUid),
      ProcessTreePacket(kOtherUid, {{kOtherPid, kTargetUid}}),
      // The target app writing a fake process tree into its own trace.
      ProcessTreePacket(kTargetUid, {{kOtherPid, kTargetUid}}),
      // System data.
      PackagesListPacket(kTracedProbesUid),
      TriggerPacket(kOtherUid, kOtherPackage),
      ProcessStatsPacket(),
      ProcessTreePacket(kTracedProbesUid,
                        {{kTargetPid, kTargetUid}, {kOtherPid, kOtherUid}}),
      // Appended after collection (no trusted_uid).
      MappingPacket(kTargetPackage),
      MappingPacket(kOtherPackage),
      UidLessProcessTreePacket(),
  };
}

class HeapDumpRedactionTest : public testing::Test {
 protected:
  base::Status Redact(const std::vector<std::string>& packets,
                      std::string* output,
                      const char* package = kTargetPackage) {
    auto redactor = TraceRedactor::CreateInstance(TraceRedactor::Config());
    context_.package_name = package;
    return redactor->RedactBuffer(BuildTrace(packets), output, &context_);
  }

  static std::vector<std::string> Trace() { return MultiAppTrace(); }

  Context context_;
};

TEST_F(HeapDumpRedactionTest, KeepsOnlyTargetHeapDump) {
  std::string output;
  ASSERT_OK(Redact(Trace(), &output));

  // Pure heap dump trace (0 ftrace events, 1 system ProcessTree): dynamic Pass
  // 2 is skipped.
  EXPECT_EQ(context_.synthetic_process, nullptr);
  EXPECT_FALSE(context_.merged_process_tree.has_value());

  auto packets = SplitTrace(output);
  EXPECT_THAT(HeapGraphWriters(packets),
              testing::ElementsAre(kTargetUid, kTargetUid));

  // The target's heap graph packets are kept byte-identical.
  auto input = Trace();
  EXPECT_EQ(packets[0], input[0]);
  EXPECT_EQ(packets[2], input[2]);
}

TEST_F(HeapDumpRedactionTest, KeepsTargetMetadataAndMappingOnly) {
  std::string output;
  ASSERT_OK(Redact(Trace(), &output));

  int art_metadata = 0;
  int mappings = 0;
  for (const auto& p : SplitTrace(output)) {
    protozero::ProtoDecoder decoder(p);
    if (decoder.FindField(TracePacket::kArtProcessMetadataFieldNumber)
            .valid()) {
      art_metadata++;
      EXPECT_EQ(
          decoder.FindField(TracePacket::kTrustedUidFieldNumber).as_int32(),
          kTargetUid);
    }
    if (auto name = heap_dump::DeobfuscationMappingPackage(&decoder); name) {
      mappings++;
      EXPECT_EQ(name->ToStdString(), kTargetPackage);
    }
  }
  EXPECT_EQ(art_metadata, 1);
  EXPECT_EQ(mappings, 1);
}

TEST_F(HeapDumpRedactionTest, PrunesSystemTablesToTarget) {
  std::string output;
  ASSERT_OK(Redact(Trace(), &output));

  std::vector<int32_t> stats_pids;
  std::vector<int32_t> tree_pids;
  std::vector<std::string> package_names;
  for (const auto& p : SplitTrace(output)) {
    TracePacket::Decoder packet(p);
    if (packet.has_process_stats()) {
      protos::pbzero::ProcessStats::Decoder stats(packet.process_stats());
      for (auto it = stats.processes(); it; ++it) {
        stats_pids.push_back(
            protos::pbzero::ProcessStats::Process::Decoder(*it).pid());
      }
    }
    if (packet.has_process_tree()) {
      protos::pbzero::ProcessTree::Decoder tree(packet.process_tree());
      for (auto it = tree.processes(); it; ++it) {
        tree_pids.push_back(
            protos::pbzero::ProcessTree::Process::Decoder(*it).pid());
      }
    }
    if (packet.has_packages_list()) {
      protos::pbzero::PackagesList::Decoder list(packet.packages_list());
      for (auto it = list.packages(); it; ++it) {
        package_names.push_back(
            protos::pbzero::PackagesList::PackageInfo::Decoder(*it)
                .name()
                .ToStdString());
      }
    }
  }

  // Stats were recorded before the process snapshot; back-dating keeps the
  // target's row.
  EXPECT_THAT(stats_pids, testing::ElementsAre(kTargetPid));
  // The target app's fake process tree entry for kOtherPid is pruned.
  EXPECT_THAT(tree_pids, testing::ElementsAre(kTargetPid));
  EXPECT_THAT(package_names, testing::ElementsAre(kTargetPackage));
}

TEST_F(HeapDumpRedactionTest, AppPacketsDoNotAffectAttribution) {
  std::string output;
  ASSERT_OK(Redact(Trace(), &output));
  // The spoofed packages list (written by another app) is ignored.
  ASSERT_TRUE(context_.package_uid.has_value());
  EXPECT_EQ(*context_.package_uid, static_cast<uint64_t>(kTargetUid));
}

TEST_F(HeapDumpRedactionTest, ScrubsTriggerFromOtherApp) {
  std::string output;
  ASSERT_OK(Redact(Trace(), &output));

  int triggers = 0;
  for (const auto& p : SplitTrace(output)) {
    TracePacket::Decoder packet(p);
    if (!packet.has_trigger()) {
      continue;
    }
    triggers++;
    protos::pbzero::Trigger::Decoder trigger(packet.trigger());
    EXPECT_TRUE(trigger.has_trigger_name());
    EXPECT_FALSE(trigger.has_producer_name());
    EXPECT_FALSE(trigger.has_trusted_producer_uid());
  }
  EXPECT_EQ(triggers, 1);
}

TEST_F(HeapDumpRedactionTest, KeepsTriggerFromTarget) {
  std::vector<std::string> packets = Trace();
  for (auto& p : packets) {
    if (TracePacket::Decoder(p).has_trigger()) {
      p = TriggerPacket(kTargetUid, kTargetPackage);
    }
  }

  std::string output;
  ASSERT_OK(Redact(packets, &output));
  for (const auto& p : SplitTrace(output)) {
    TracePacket::Decoder packet(p);
    if (packet.has_trigger()) {
      protos::pbzero::Trigger::Decoder trigger(packet.trigger());
      EXPECT_EQ(trigger.producer_name().ToStdString(), kTargetPackage);
    }
  }
}

TEST_F(HeapDumpRedactionTest, DropsUidLessNonMappingPackets) {
  std::string output;
  ASSERT_OK(Redact(Trace(), &output));
  for (const auto& p : SplitTrace(output)) {
    if (GetWriterType(TracePacket::Decoder(p)) == WriterType::kMissingUid) {
      EXPECT_THAT(
          Payloads(p),
          testing::ElementsAre(TracePacket::kDeobfuscationMappingFieldNumber));
    }
  }
}

TEST_F(HeapDumpRedactionTest, Summary) {
  std::string output;
  ASSERT_OK(Redact(Trace(), &output));
  auto summary = SummarizeHeapDumpRedaction(context_);
  EXPECT_EQ(summary.heap_graph_packets_kept, 2u);
  // Other app (1) + com.android.phone (1).
  EXPECT_EQ(summary.heap_graph_packets_dropped, 2u);
  EXPECT_EQ(summary.complete_dumps_kept, 1u);
  EXPECT_EQ(summary.incomplete_dumps_dropped, 0u);
  // Other app: heap graph, art metadata, packages list, process tree.
  EXPECT_EQ(summary.untrusted_app_packets_dropped, 4u);
  // Other package mapping + uid-less process tree.
  EXPECT_EQ(summary.missing_uid_packets_dropped, 2u);
  EXPECT_EQ(summary.deobfuscation_mappings_kept, 1u);
}

TEST_F(HeapDumpRedactionTest, IncompleteOnlyTargetDumpIsDroppedNotFailed) {
  std::vector<std::string> packets = {
      PackagesListPacket(kTracedProbesUid),
      HeapGraphPacket(kTargetUid, 4, kTargetPid, /*continued=*/true),
  };
  std::string output;
  ASSERT_OK(Redact(packets, &output));
  EXPECT_THAT(HeapGraphWriters(SplitTrace(output)), testing::IsEmpty());

  auto summary = SummarizeHeapDumpRedaction(context_);
  EXPECT_EQ(summary.complete_dumps_kept, 0u);
  EXPECT_EQ(summary.incomplete_dumps_dropped, 1u);
}

TEST_F(HeapDumpRedactionTest, DropsIncompleteSecondTargetDump) {
  std::vector<std::string> packets = {
      PackagesListPacket(kTracedProbesUid),
      HeapGraphPacket(kTargetUid, 4, kTargetPid, /*continued=*/false),
      HeapGraphPacket(kTargetUid, 7, kTargetPid, /*continued=*/true),
      ArtMetadataPacket(kTargetUid, 8, kTargetPid, kTargetPackage),
  };
  std::string output;
  ASSERT_OK(Redact(packets, &output));

  auto out = SplitTrace(output);
  EXPECT_THAT(HeapGraphWriters(out), testing::ElementsAre(kTargetUid));
  for (const auto& p : out) {
    // Metadata on a sequence without a heap graph is dropped.
    EXPECT_FALSE(TracePacket::Decoder(p).has_art_process_metadata());
  }
  EXPECT_EQ(SummarizeHeapDumpRedaction(context_).incomplete_dumps_dropped, 2u);
}

TEST_F(HeapDumpRedactionTest, NoTargetDumpProducesNoHeapData) {
  std::vector<std::string> packets = {
      PackagesListPacket(kTracedProbesUid),
      HeapGraphPacket(kOtherUid, 5, kOtherPid, /*continued=*/false),
  };
  std::string output;
  ASSERT_OK(Redact(packets, &output));
  EXPECT_THAT(HeapGraphWriters(SplitTrace(output)), testing::IsEmpty());

  auto summary = SummarizeHeapDumpRedaction(context_);
  EXPECT_EQ(summary.complete_dumps_kept, 0u);
  EXPECT_EQ(summary.incomplete_dumps_dropped, 0u);
  EXPECT_EQ(summary.heap_graph_packets_dropped, 1u);
}

TEST_F(HeapDumpRedactionTest, RedactsForSystemUidTarget) {
  protozero::HeapBuffered<TracePacket> list;
  list->set_trusted_uid(kTracedProbesUid);
  auto* packages = list->set_packages_list();
  auto* phone = packages->add_packages();
  phone->set_name("com.android.phone");
  phone->set_uid(kPhoneUid);
  auto* app = packages->add_packages();
  app->set_name(kTargetPackage);
  app->set_uid(kTargetUid);

  std::vector<std::string> packets = {
      list.SerializeAsString(),
      HeapGraphPacket(kPhoneUid, 6, 300, /*continued=*/false),
      HeapGraphPacket(1000, 7, 150, /*continued=*/false),
      HeapGraphPacket(kTargetUid, 8, kTargetPid, /*continued=*/false),
  };
  std::string output;
  ASSERT_OK(Redact(packets, &output, "com.android.phone"));
  EXPECT_THAT(HeapGraphWriters(SplitTrace(output)),
              testing::ElementsAre(kPhoneUid));

  auto summary = SummarizeHeapDumpRedaction(context_);
  EXPECT_EQ(summary.complete_dumps_kept, 1u);
  EXPECT_EQ(summary.heap_graph_packets_kept, 1u);
  EXPECT_EQ(summary.heap_graph_packets_dropped, 2u);
  EXPECT_EQ(summary.untrusted_app_packets_dropped, 1u);
}

TEST_F(HeapDumpRedactionTest, MalformedAppPacketDoesNotFailTrace) {
  // Another app writes a process tree without a timestamp. Verification would
  // reject this from a system producer, but app packets are only dropped.
  protozero::HeapBuffered<TracePacket> bad;
  bad->set_trusted_uid(kOtherUid);
  bad->set_process_tree()->add_processes()->set_pid(kOtherPid);

  auto packets = Trace();
  packets.push_back(bad.SerializeAsString());
  std::string output;
  ASSERT_OK(Redact(packets, &output));
}

TEST_F(HeapDumpRedactionTest, NoProcessTreeIsNotAnError) {
  std::vector<std::string> packets = {
      PackagesListPacket(kTracedProbesUid),
      HeapGraphPacket(kTargetUid, 4, kTargetPid, /*continued=*/false),
  };
  std::string output;
  ASSERT_OK(Redact(packets, &output));
  EXPECT_THAT(HeapGraphWriters(SplitTrace(output)),
              testing::ElementsAre(kTargetUid));
}

TEST_F(HeapDumpRedactionTest, SecondaryUserTargetDumpKept) {
  constexpr int32_t kSecondaryUserTargetUid = 10 * 100000 + kTargetUid;
  std::vector<std::string> packets = {
      PackagesListPacket(kTracedProbesUid),
      HeapGraphPacket(kSecondaryUserTargetUid, 4, kTargetPid,
                      /*continued=*/false),
  };
  std::string output;
  ASSERT_OK(Redact(packets, &output));
  EXPECT_THAT(HeapGraphWriters(SplitTrace(output)),
              testing::ElementsAre(kSecondaryUserTargetUid));
}

TEST_F(HeapDumpRedactionTest, ExplicitUidIsolatesUsers) {
  constexpr int32_t kSecondaryUserTargetUid = 10 * 100000 + kTargetUid;
  std::vector<std::string> packets = {
      PackagesListPacket(kTracedProbesUid),
      HeapGraphPacket(kTargetUid, 4, kTargetPid, /*continued=*/false),
      HeapGraphPacket(kSecondaryUserTargetUid, 5, kTargetPid + 1,
                      /*continued=*/false),
  };
  context_.package_uid = kSecondaryUserTargetUid;
  std::string output;
  ASSERT_OK(Redact(packets, &output));
  EXPECT_THAT(HeapGraphWriters(SplitTrace(output)),
              testing::ElementsAre(kSecondaryUserTargetUid));
}

TEST_F(HeapDumpRedactionTest, ScrubsSlowDataSourcesFromServiceEvents) {
  protozero::HeapBuffered<TracePacket> event;
  event->set_trusted_uid(kTracedProbesUid);
  event->set_service_event()
      ->set_last_flush_slow_data_sources()
      ->add_data_source()
      ->set_producer_name(kOtherPackage);

  auto packets = Trace();
  packets.push_back(event.SerializeAsString());
  std::string output;
  ASSERT_OK(Redact(packets, &output));
  EXPECT_EQ(output.find(kOtherPackage), std::string::npos);
}

// Remnants of dropped system packets that are left with only shell fields
// (uid/pid, sequence id, timestamp) are removed.
TEST_F(HeapDumpRedactionTest, DropsShellRemnants) {
  protozero::HeapBuffered<TracePacket> remnant;
  remnant->set_trusted_uid(1000);
  remnant->set_trusted_pid(1500);
  remnant->set_trusted_packet_sequence_id(77);
  remnant->set_timestamp(kHeapDumpTs);
  remnant->AppendBytes(TracePacket::kTrackEventFieldNumber, "x", 1);

  auto packets = Trace();
  packets.push_back(remnant.SerializeAsString());
  std::string output;
  ASSERT_OK(Redact(packets, &output));

  for (const auto& p : SplitTrace(output)) {
    protozero::ProtoDecoder decoder(p);
    auto seq =
        decoder.FindField(TracePacket::kTrustedPacketSequenceIdFieldNumber);
    EXPECT_FALSE(seq.valid() && seq.as_uint32() == 77u);
  }
}

// Target-app packets (`WriterType::kApp`) may only carry heap dump payloads:
// spoofed system payloads (even malformed ones) are stripped by
// `app_packet_mask` without failing or poisoning the trace.
TEST_F(HeapDumpRedactionTest, StripsSpoofedSystemPayloadsFromTargetApp) {
  protozero::HeapBuffered<TracePacket> spoofed;
  spoofed->set_trusted_uid(kTargetUid);
  spoofed->set_ftrace_events();  // Missing cpu; would fail if treated as system
  spoofed->set_process_stats()->add_processes()->set_pid(kOtherPid);

  auto packets = Trace();
  packets.push_back(spoofed.SerializeAsString());
  std::string output;
  ASSERT_OK(Redact(packets, &output));

  for (const auto& p : SplitTrace(output)) {
    TracePacket::Decoder decoder(p);
    EXPECT_FALSE(decoder.has_ftrace_events());
  }
}

// Verifies the unified pipeline handles a trace containing both ftrace
// scheduling events, multiple ProcessTree snapshots, and Java heap dumps,
// activating dynamic Pass 2 while preserving heap dump packets intact.
TEST_F(HeapDumpRedactionTest, RedactsCombinedSystemAndHeapDumpTrace) {
  protozero::HeapBuffered<TracePacket> ftrace_pkt;
  ftrace_pkt->set_trusted_uid(kTracedProbesUid);
  ftrace_pkt->set_timestamp(kHeapDumpTs);
  auto* bundle = ftrace_pkt->set_ftrace_events();
  bundle->set_cpu(0);
  {
    auto* e = bundle->add_event();
    e->set_timestamp(kHeapDumpTs);
    e->set_pid(kTargetPid);
    auto* rename = e->set_task_rename();
    rename->set_pid(kTargetPid);
    rename->set_oldcomm("TargetMain");
    rename->set_newcomm("TargetRenamed");
    rename->set_oom_score_adj(0);
  }
  {
    auto* e = bundle->add_event();
    e->set_timestamp(kHeapDumpTs + 1);
    e->set_pid(kOtherPid);
    auto* rename = e->set_task_rename();
    rename->set_pid(kOtherPid);
    rename->set_oldcomm("OtherMain");
    rename->set_newcomm("OtherSecret");
    rename->set_oom_score_adj(0);
  }

  auto packets = Trace();
  // Add a second system ProcessTree snapshot and an ftrace bundle so dynamic
  // Pass 2 runs both process-tree merging and synthetic sched merging.
  packets.push_back(
      ProcessTreePacket(kTracedProbesUid, {{kTargetPid, kTargetUid}}));
  packets.push_back(ftrace_pkt.SerializeAsString());

  std::string output;
  ASSERT_OK(Redact(packets, &output));

  EXPECT_NE(context_.synthetic_process, nullptr);
  EXPECT_TRUE(context_.merged_process_tree.has_value());

  auto out_packets = SplitTrace(output);
  EXPECT_THAT(HeapGraphWriters(out_packets),
              testing::ElementsAre(kTargetUid, kTargetUid));
  EXPECT_NE(output.find("TargetRenamed"), std::string::npos);
  EXPECT_EQ(output.find("OtherSecret"), std::string::npos);

  int process_tree_count = 0;
  for (const auto& p : out_packets) {
    TracePacket::Decoder decoder(p);
    if (decoder.has_process_tree()) {
      process_tree_count++;
      protos::pbzero::ProcessTree::Decoder tree(decoder.process_tree());
      bool found_target_start = false;
      for (auto it = tree.processes(); it; ++it) {
        protos::pbzero::ProcessTree::Process::Decoder proc(*it);
        if (proc.pid() == kTargetPid && proc.has_process_start_from_boot() &&
            proc.process_start_from_boot() == kProcessStartTs) {
          found_target_start = true;
        }
      }
      EXPECT_TRUE(found_target_start);
    }
  }
  EXPECT_EQ(process_tree_count, 1);
}

}  // namespace
}  // namespace perfetto::trace_redaction
