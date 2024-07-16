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

#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include <memory>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/remote_clock_sync.pbzero.h"

namespace perfetto::trace_processor {
namespace {

constexpr auto REALTIME = protos::pbzero::BUILTIN_CLOCK_REALTIME;
constexpr auto BOOTTIME = protos::pbzero::BUILTIN_CLOCK_BOOTTIME;

class ProtoTraceReaderTest : public ::testing::Test {
 public:
  ProtoTraceReaderTest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, 0x1001);
    context_.clock_tracker = std::make_unique<ClockTracker>(&context_);
    proto_trace_reader_ = std::make_unique<ProtoTraceReader>(&context_);
  }

  util::Status Tokenize() {
    trace_->Finalize();
    std::vector<uint8_t> trace_bytes = trace_.SerializeAsArray();
    std::unique_ptr<uint8_t[]> raw_trace(new uint8_t[trace_bytes.size()]);
    memcpy(raw_trace.get(), trace_bytes.data(), trace_bytes.size());
    auto status = proto_trace_reader_->Parse(TraceBlobView(
        TraceBlob::TakeOwnership(std::move(raw_trace), trace_bytes.size())));

    trace_.Reset();
    return status;
  }

 protected:
  protozero::HeapBuffered<protos::pbzero::Trace> trace_;
  TraceProcessorContext context_;
  std::unique_ptr<ProtoTraceReader> proto_trace_reader_;
};

TEST_F(ProtoTraceReaderTest, RemoteClockSync_Valid) {
  context_.machine_tracker =
      std::make_unique<MachineTracker>(&context_, 0x1001);

  auto* packet = trace_->add_packet();
  packet->set_machine_id(0x1001);
  auto* remote_clock_sync = packet->set_remote_clock_sync();
  auto* synced_clocks = remote_clock_sync->add_synced_clocks();
  auto* client_clocks = synced_clocks->set_client_clocks();

  // First synced clock snapshots on both sides.
  auto* clock = client_clocks->add_clocks();
  clock->set_clock_id(BOOTTIME);
  clock->set_timestamp(10000);

  auto* host_clocks = synced_clocks->set_host_clocks();
  clock = host_clocks->add_clocks();
  clock->set_clock_id(BOOTTIME);
  clock->set_timestamp(120000);

  // Second synced clock snapshots on both sides.
  synced_clocks = remote_clock_sync->add_synced_clocks();

  client_clocks = synced_clocks->set_client_clocks();
  clock = client_clocks->add_clocks();
  clock->set_clock_id(BOOTTIME);
  clock->set_timestamp(25000);

  host_clocks = synced_clocks->set_host_clocks();
  clock = host_clocks->add_clocks();
  clock->set_clock_id(BOOTTIME);
  clock->set_timestamp(135000);

  ASSERT_TRUE(Tokenize().ok());
  ASSERT_EQ(1u, context_.clock_tracker->clock_offsets_for_testing().size());
}

TEST_F(ProtoTraceReaderTest, RemoteClockSync_Incomplete) {
  context_.machine_tracker =
      std::make_unique<MachineTracker>(&context_, 0x1001);

  auto* packet = trace_->add_packet();
  packet->set_machine_id(0x1001);
  auto* remote_clock_sync = packet->set_remote_clock_sync();
  auto* synced_clocks = remote_clock_sync->add_synced_clocks();
  auto* client_clocks = synced_clocks->set_client_clocks();

  // First synced clock snapshots on both sides.
  auto* clock = client_clocks->add_clocks();
  clock->set_clock_id(BOOTTIME);
  clock->set_timestamp(10000);

  auto* host_clocks = synced_clocks->set_host_clocks();
  clock = host_clocks->add_clocks();
  clock->set_clock_id(BOOTTIME);
  clock->set_timestamp(120000);

  // Second synced clock snapshots on both sides.
  synced_clocks = remote_clock_sync->add_synced_clocks();

  client_clocks = synced_clocks->set_client_clocks();
  clock = client_clocks->add_clocks();
  clock->set_clock_id(BOOTTIME);
  clock->set_timestamp(25000);

  // Missing the second host CLOCK_BOOTTIME making it below the minimum
  // requirement for using the remote_clock_sync for calculating clock offset.

  ASSERT_TRUE(Tokenize().ok());
  // No valid clock offset.
  ASSERT_EQ(0u, context_.clock_tracker->clock_offsets_for_testing().size());
}

TEST_F(ProtoTraceReaderTest, CalculateClockOffset) {
  std::vector<ProtoTraceReader::SyncClockSnapshots> sync_clock_snapshots;
  ProtoTraceReader::SyncClockSnapshots snapshots;
  snapshots[BOOTTIME] = {120000, 10000};
  snapshots[REALTIME] = {135000, 25000};
  sync_clock_snapshots.push_back(std::move(snapshots));

  snapshots[BOOTTIME] = {140000, 20000};
  snapshots[REALTIME] = {150000, 35000};
  sync_clock_snapshots.push_back(std::move(snapshots));

  auto clock_offsets = proto_trace_reader_->CalculateClockOffsetsForTesting(
      sync_clock_snapshots);
  ASSERT_EQ(2u, clock_offsets.size());
  // Client 10000      20000
  // Host     120000     140000
  // Estimated offsets: (10000 + 20000)/2 - 120000 = -105000,
  //                    20000 - (120000 + 140000) / 2 = -110000.
  // Average = -107500.
  ASSERT_EQ(-107500, clock_offsets[BOOTTIME]);
  // Client 25000      35000
  // Host     135000     150000
  // Estimated offsets: (25000 + 35000)/2 - 135000 = -105000,
  //                    35000 - (135000 + 150000) / 2 = -107500.
  // Average = -106250.
  ASSERT_EQ(-106250, clock_offsets[REALTIME]);
}

TEST_F(ProtoTraceReaderTest, CalculateClockOffset_AboveThreshold) {
  std::vector<ProtoTraceReader::SyncClockSnapshots> sync_clock_snapshots;
  ProtoTraceReader::SyncClockSnapshots snapshots;
  snapshots[BOOTTIME] = {120000, 10000};
  snapshots[REALTIME] = {135000, 25000};
  sync_clock_snapshots.push_back(std::move(snapshots));

  // 30 sec interval: the 2 clock snapshots will be considered 2 different
  // rounds of clock synchronization IPC exchange and won't be used.
  auto interval = 30ull * 1000 * 1000 * 1000;
  snapshots[BOOTTIME] = {120000 + interval, 10000 + interval};
  snapshots[REALTIME] = {135000 + interval, 25000 + interval};
  sync_clock_snapshots.push_back(std::move(snapshots));

  auto clock_offsets = proto_trace_reader_->CalculateClockOffsetsForTesting(
      sync_clock_snapshots);
  ASSERT_EQ(0u, clock_offsets.size());
}

TEST_F(ProtoTraceReaderTest, CalculateClockOffset_MultiRounds) {
  std::vector<ProtoTraceReader::SyncClockSnapshots> sync_clock_snapshots;
  ProtoTraceReader::SyncClockSnapshots snapshots;
  // This emits clock offsets -105000, -110000.
  snapshots[BOOTTIME] = {120000, 10000};
  sync_clock_snapshots.push_back(std::move(snapshots));
  snapshots[BOOTTIME] = {140000, 20000};
  sync_clock_snapshots.push_back(std::move(snapshots));

  // The interval works as a delimeter of IPC exchange.
  auto interval = 30ull * 1000 * 1000 * 1000;

  // This emits clock offsets: (30000 + 45000) / 2 - 160000 = -122500,
  //                           45000 - (160000 + 170000) / 2 = -120000.
  snapshots[BOOTTIME] = {160000 + interval, 30000 + interval};
  sync_clock_snapshots.push_back(std::move(snapshots));
  snapshots[BOOTTIME] = {170000 + interval, 45000 + interval};
  sync_clock_snapshots.push_back(std::move(snapshots));

  auto clock_offsets = proto_trace_reader_->CalculateClockOffsetsForTesting(
      sync_clock_snapshots);
  ASSERT_EQ(1u, clock_offsets.size());
  // Average(-105000, -110000, -122500, -120000) = -114375.
  ASSERT_EQ(-114375, clock_offsets[BOOTTIME]);
}

}  // namespace
}  // namespace perfetto::trace_processor
