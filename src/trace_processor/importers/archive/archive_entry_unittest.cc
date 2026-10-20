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

#include "src/trace_processor/importers/archive/archive_entry.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/util/trace_type.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor {
namespace {

// What a packet of a test trace carries, besides a timestamp.
enum class Packet { kPlain, kClockSnapshot, kCompressed, kZstdCompressed };

TraceBlobView MakeTrace(const std::vector<Packet>& packets) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;
  for (Packet p : packets) {
    auto* packet = trace->add_packet();
    packet->set_timestamp(1000);
    packet->set_trusted_packet_sequence_id(1);
    switch (p) {
      case Packet::kPlain:
        break;
      case Packet::kClockSnapshot: {
        auto* clock = packet->set_clock_snapshot()->add_clocks();
        clock->set_clock_id(6);
        clock->set_timestamp(1000);
        break;
      }
      case Packet::kCompressed:
        packet->set_compressed_packets(std::string("\x01\x02"));
        break;
      case Packet::kZstdCompressed:
        packet->set_zstd_compressed_packets(std::string("\x01\x02"));
        break;
    }
  }
  std::vector<uint8_t> bytes = trace.SerializeAsArray();
  return TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size()));
}

bool MayHaveClockSnapshot(const TraceBlobView& trace) {
  return ProtoTraceMayHaveClockSnapshot(&trace, &trace + 1);
}

TEST(ProtoTraceMayHaveClockSnapshotTest, NoSnapshot) {
  EXPECT_FALSE(MayHaveClockSnapshot(MakeTrace({Packet::kPlain})));
  EXPECT_FALSE(
      MayHaveClockSnapshot(MakeTrace({Packet::kPlain, Packet::kPlain})));
}

TEST(ProtoTraceMayHaveClockSnapshotTest, SnapshotInAnyPacket) {
  EXPECT_TRUE(MayHaveClockSnapshot(MakeTrace({Packet::kClockSnapshot})));
  EXPECT_TRUE(MayHaveClockSnapshot(
      MakeTrace({Packet::kPlain, Packet::kPlain, Packet::kClockSnapshot})));
}

// Compressed packets are not inflated, so they count as hiding a snapshot.
TEST(ProtoTraceMayHaveClockSnapshotTest, CompressedPackets) {
  EXPECT_TRUE(
      MayHaveClockSnapshot(MakeTrace({Packet::kPlain, Packet::kCompressed})));
  EXPECT_TRUE(MayHaveClockSnapshot(
      MakeTrace({Packet::kPlain, Packet::kZstdCompressed})));
}

TEST(ProtoTraceMayHaveClockSnapshotTest, EmptyOrMalformed) {
  EXPECT_FALSE(ProtoTraceMayHaveClockSnapshot(nullptr, nullptr));
  std::vector<uint8_t> garbage(32, 0xff);
  EXPECT_FALSE(MayHaveClockSnapshot(
      TraceBlobView(TraceBlob::CopyFrom(garbage.data(), garbage.size()))));
}

// A member read from a tar is a list of chunks with arbitrary boundaries:
// splitting the trace anywhere must not change the result.
TEST(ProtoTraceMayHaveClockSnapshotTest, SplitAcrossChunks) {
  for (bool with_snapshot : {false, true}) {
    TraceBlobView trace =
        MakeTrace({Packet::kPlain, Packet::kPlain,
                   with_snapshot ? Packet::kClockSnapshot : Packet::kPlain});
    for (size_t split = 0; split <= trace.size(); ++split) {
      TraceBlobView chunks[] = {trace.slice_off(0, split),
                                trace.slice_off(split, trace.size() - split)};
      EXPECT_EQ(ProtoTraceMayHaveClockSnapshot(chunks, chunks + 2),
                with_snapshot)
          << "split at " << split;
    }
  }
}

TEST(ArchiveEntryTest, ClockSnapshotFirstWithinPriority) {
  ArchiveEntry sdk{"a_sdk.pb", 0, TraceImporterId(), 0, true};
  ArchiveEntry system{"b_system.pb", 1, TraceImporterId(), 0, false};
  EXPECT_LT(system, sdk);
  EXPECT_FALSE(sdk < system);

  // The priority still comes first.
  ArchiveEntry other{"0_other", 2, TraceImporterId(), 1, false};
  EXPECT_LT(sdk, other);
}

}  // namespace
}  // namespace perfetto::trace_processor
