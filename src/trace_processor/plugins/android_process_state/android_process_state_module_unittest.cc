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

#include "src/trace_processor/plugins/android_process_state/android_process_state_module.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/blob_packet_writer.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::android_process_state {
namespace {

using ::com::android::internal::pbzero::AndroidFreezerStateSnapshot;
using ::com::android::internal::pbzero::AndroidProcessStateChangedEvent;
using ::com::android::internal::pbzero::AndroidProcessStateSnapshot;
using ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using ::perfetto::protos::pbzero::TracePacket;

class AndroidProcessStateModuleTest : public testing::Test {
 public:
  AndroidProcessStateModuleTest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.descriptor_pool_ = std::make_unique<DescriptorPool>();
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.import_logs_tracker =
        std::make_unique<ImportLogsTracker>(&context_, TraceId(1));
    context_.process_tracker = std::make_unique<ProcessTracker>(&context_);
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());

    process_state_table_ = std::make_unique<tables::AndroidProcessStateTable>(
        context_.storage->mutable_string_pool());
    freezer_state_table_ = std::make_unique<tables::AndroidFreezerStateTable>(
        context_.storage->mutable_string_pool());

    tracker_ = std::make_unique<AndroidProcessStateTracker>(
        &context_, process_state_table_.get());
    module_ = std::make_unique<AndroidProcessStateModule>(
        &module_context_, tracker_.get(), freezer_state_table_.get());
  }

  void PushProcessStateDump(const std::vector<uint8_t>& dump_payload,
                            int64_t ts) {
    protozero::HeapBuffered<TracePacket> packet;
    packet->AppendBytes(
        FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber,
        dump_payload.data(), dump_payload.size());
    std::vector<uint8_t> bytes = packet.SerializeAsArray();

    TraceBlobView tbv(TraceBlob::CopyFrom(bytes.data(), bytes.size()));
    TracePacketData tpd{tbv.copy(), {}};
    SelectiveTracePacketDecoder decoder(tbv.data(), tbv.length());
    TracePacketField field = decoder.FindUnknownField(
        FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber);
    ASSERT_TRUE(field.valid());
    module_->ParseField({decoder, ts, tpd, field});
  }

  void PushFreezerDump(const std::vector<uint8_t>& dump_payload, int64_t ts) {
    protozero::HeapBuffered<TracePacket> packet;
    packet->AppendBytes(
        FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber,
        dump_payload.data(), dump_payload.size());
    std::vector<uint8_t> bytes = packet.SerializeAsArray();

    TraceBlobView tbv(TraceBlob::CopyFrom(bytes.data(), bytes.size()));
    TracePacketData tpd{tbv.copy(), {}};
    SelectiveTracePacketDecoder decoder(tbv.data(), tbv.length());
    TracePacketField field = decoder.FindUnknownField(
        FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber);
    ASSERT_TRUE(field.valid());
    module_->ParseField({decoder, ts, tpd, field});
  }

 protected:
  TraceProcessorContext context_;
  ProtoImporterModuleContext module_context_;
  std::unique_ptr<tables::AndroidProcessStateTable> process_state_table_;
  std::unique_ptr<tables::AndroidFreezerStateTable> freezer_state_table_;
  std::unique_ptr<AndroidProcessStateTracker> tracker_;
  std::unique_ptr<AndroidProcessStateModule> module_;
};

TEST_F(AndroidProcessStateModuleTest, ParseDumpPopulatesProcessTable) {
  protozero::HeapBuffered<AndroidProcessStateSnapshot> snapshot;
  {
    auto* rec = snapshot->add_record();
    rec->set_pid(2001);
    rec->set_uid(10002);
    rec->set_proc_state(2);  // PROCESS_STATE_PERSISTENT
    rec->set_oom_score(-900);
    rec->set_capability_flags(1);
  }
  {
    auto* rec = snapshot->add_record();
    rec->set_pid(2002);
    rec->set_uid(10003);
    rec->set_proc_state(19);  // PROCESS_STATE_CACHED_EMPTY
    rec->set_oom_score(905);
    rec->set_capability_flags(0);
  }
  std::vector<uint8_t> snap_bytes = snapshot.SerializeAsArray();

  PushProcessStateDump(snap_bytes, 1000000000);
  module_->OnEventsFullyExtracted();

  // Verify process_state table
  EXPECT_EQ(process_state_table_->row_count(), 2u);

  // Verify main process table (row 0 is reserved swapper/idle process)
  const auto& pt = context_.storage->process_table();
  EXPECT_EQ(pt.row_count(), 3u);

  auto prr0 = pt[1];
  EXPECT_EQ(prr0.pid(), 2001);
  EXPECT_EQ(prr0.uid(), 10002u);

  auto prr1 = pt[2];
  EXPECT_EQ(prr1.pid(), 2002);
  EXPECT_EQ(prr1.uid(), 10003u);
}

TEST_F(AndroidProcessStateModuleTest, ParseChangeAndDumpRewind) {
  // Process 3001 undergoes a change at ts=5000
  protozero::HeapBuffered<AndroidProcessStateChangedEvent> change;
  change->set_pid(3001);
  change->set_uid(10004);
  change->set_prev_proc_state(
      ::com::android::internal::pbzero::PROCESS_STATE_CACHED_EMPTY);
  change->set_cur_proc_state(
      ::com::android::internal::pbzero::PROCESS_STATE_FOREGROUND_SERVICE);
  change->set_prev_oom_score(905);
  change->set_cur_oom_score(200);
  change->set_prev_capability_flags(0);
  change->set_cur_capability_flags(1);
  std::vector<uint8_t> change_bytes = change.SerializeAsArray();

  tracker_->ParseChange(
      5000, protozero::ConstBytes{change_bytes.data(), change_bytes.size()});

  // At trace stop (ts=10000), dump reports cur_proc_state=6
  protozero::HeapBuffered<AndroidProcessStateSnapshot> snapshot;
  auto* rec = snapshot->add_record();
  rec->set_pid(3001);
  rec->set_uid(10004);
  rec->set_proc_state(6);
  rec->set_oom_score(200);
  rec->set_capability_flags(1);
  std::vector<uint8_t> snap_bytes = snapshot.SerializeAsArray();

  PushProcessStateDump(snap_bytes, 10000);
  module_->OnEventsFullyExtracted();

  // Should have 1 change row (is_initial=0) and 1 initial baseline row
  // (is_initial=1)
  EXPECT_EQ(process_state_table_->row_count(), 2u);

  auto change_row = (*process_state_table_)[0];
  EXPECT_EQ(change_row.id().value, 0u);
  ASSERT_TRUE(change_row.ts().has_value());
  EXPECT_EQ(*change_row.ts(), 5000);
  EXPECT_EQ(change_row.is_initial(), 0u);

  auto initial_row = (*process_state_table_)[1];
  EXPECT_EQ(initial_row.id().value, 1u);
  EXPECT_FALSE(initial_row.ts().has_value());
  EXPECT_EQ(initial_row.is_initial(), 1u);
}

TEST_F(AndroidProcessStateModuleTest, ParseFreezerDump) {
  protozero::HeapBuffered<AndroidFreezerStateSnapshot> freezer_snapshot;
  auto* rec = freezer_snapshot->add_record();
  rec->set_pid(4001);
  rec->set_uid(10005);
  rec->set_frozen_dur_ms(1500);
  rec->set_unfrozen_dur_ms(0);
  std::vector<uint8_t> freezer_bytes = freezer_snapshot.SerializeAsArray();

  PushFreezerDump(freezer_bytes, 20000);

  EXPECT_EQ(freezer_state_table_->row_count(), 1u);
  auto rr = (*freezer_state_table_)[0];
  EXPECT_EQ(rr.id().value, 0u);
  EXPECT_FALSE(rr.ts().has_value());
  EXPECT_EQ(rr.is_initial(), 1u);
  EXPECT_FALSE(rr.unfrozen_dur_ms().has_value());
  EXPECT_FALSE(rr.frozen_dur_ms().has_value());
  EXPECT_FALSE(rr.unfreeze_reason().has_value());
}

}  // namespace
}  // namespace perfetto::trace_processor::android_process_state
