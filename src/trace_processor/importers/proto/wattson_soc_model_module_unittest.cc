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

#include "src/trace_processor/importers/proto/wattson_soc_model_module.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/wattson_soc_model.pbzero.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/additional_modules.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/wattson_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/trace_processor_context_ptr.h"
#include "src/trace_processor/util/clock_synchronizer.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using ::com::android::internal::pbzero::WattsonCpu1DCurve;
using ::com::android::internal::pbzero::WattsonCpu2DCurve;
using ::com::android::internal::pbzero::WattsonCpuTopology;
using ::com::android::internal::pbzero::WattsonGpuCurve;
using ::com::android::internal::pbzero::WattsonL3Curve;
using ::com::android::internal::pbzero::WattsonSocModel;
using ::com::android::internal::pbzero::WattsonTpuCurve;
using ::testing::DoubleEq;
using ::testing::Eq;

class WattsonSocModelModuleTest : public testing::Test {
 public:
  WattsonSocModelModuleTest() {
    context_.register_additional_proto_modules = &RegisterAdditionalModules;
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    storage_ = context_.storage.get();
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    context_.global_metadata_tracker =
        std::make_unique<GlobalMetadataTracker>(context_.storage.get());
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.metadata_tracker = std::make_unique<MetadataTracker>(&context_);
    context_.import_logs_tracker =
        std::make_unique<ImportLogsTracker>(&context_, TraceId(1));
    context_.trace_time_state = std::make_unique<TraceTimeState>(
        ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_BOOTTIME));
    primary_sync_ = std::make_unique<ClockSynchronizer>(
        context_.trace_time_state.get(),
        std::make_unique<ClockSynchronizerListenerImpl>(&context_));
    context_.clock_tracker = std::make_unique<ClockTracker>(
        &context_, primary_sync_.get(), /*is_primary=*/true);
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.slice_tracker = std::make_unique<SliceTracker>(&context_);
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(storage_);
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(storage_);
    context_.process_track_translation_table =
        std::make_unique<ProcessTrackTranslationTable>(storage_);
    context_.args_translation_table =
        std::make_unique<ArgsTranslationTable>(storage_);
    context_.track_compressor = std::make_unique<TrackCompressor>(&context_);
    context_.descriptor_pool_ = std::make_unique<DescriptorPool>();

    context_.sorter = std::make_unique<TraceSorter>(
        &context_, TraceSorter::SortingMode::kFullSort);
    reader_ = std::make_unique<ProtoTraceReader>(&context_);
  }

  void Tokenize(const uint8_t* data, size_t size) {
    auto blob = TraceBlob::CopyFrom(data, size);
    auto status = reader_->Parse(TraceBlobView(std::move(blob)));
    ASSERT_TRUE(status.ok()) << status.message();
    context_.sorter->ExtractEventsForced();
  }

 protected:
  TraceProcessorContext context_;
  TraceStorage* storage_;
  std::unique_ptr<ClockSynchronizer> primary_sync_;
  std::unique_ptr<ProtoTraceReader> reader_;
};

TEST_F(WattsonSocModelModuleTest, ParseWattsonSocModel) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;

  // Build a WattsonSocModel
  protozero::HeapBuffered<WattsonSocModel> model;
  model->set_device_name("Tensor G6");
  model->set_gpu_id(0);

  auto* topology = model->set_cpu_topology();
  topology->set_use_devfreq(true);

  // CPU policies: CPU 0..3 -> policy 0, CPU 4..6 -> policy 4, CPU 7 -> policy 7
  {
    auto* cp = topology->add_cpu_map();
    cp->set_cpu(0);
    cp->set_policy(0);
    cp->set_deep_idle_offset_ns(0);
  }
  for (int cpu = 1; cpu < 4; ++cpu) {
    auto* cp = topology->add_cpu_map();
    cp->set_cpu(cpu);
    cp->set_policy(0);
  }
  for (int cpu = 4; cpu < 7; ++cpu) {
    auto* cp = topology->add_cpu_map();
    cp->set_cpu(cpu);
    cp->set_policy(4);
  }
  {
    auto* cp = topology->add_cpu_map();
    cp->set_cpu(7);
    cp->set_policy(7);
  }

  // Policy config for policy 7: 200000ns deep idle offset, vote by freq
  {
    auto* pc = topology->add_policy_configs();
    pc->set_policy(7);
    pc->set_deep_idle_offset_ns(200000);
    pc->set_vote_by_freq(true);
  }

  // Idle state map: nominal 2 -> override 1
  {
    auto* idle = topology->add_idle_state_map();
    idle->set_nominal_idle(2);
    idle->set_override_idle(1);
  }

  // 1D power curve: policy 0 @ 1000000 kHz
  {
    auto* c1d = model->add_cpu_1d_curves();
    c1d->set_policy(0);
    c1d->set_freq_khz(1000000);
    c1d->set_static_mw(5.5);
    c1d->set_active_mw(45.0);
    c1d->set_idle0_mw(1.2);
    c1d->set_idle1_mw(0.1);
  }

  // 2D power curve: policy 7 @ 3000000 kHz, dep_policy 4 @ 2000000 kHz
  {
    auto* c2d = model->add_cpu_2d_curves();
    c2d->set_policy(7);
    c2d->set_freq_khz(3000000);
    c2d->set_dep_policy(4);
    c2d->set_dep_freq_khz(2000000);
    c2d->set_static_mw(25.0);
    c2d->set_active_mw(350.0);
    c2d->set_idle0_mw(2.5);
    c2d->set_idle1_mw(0.2);
    c2d->set_interconnect_mw(18.0);
  }

  // L3 curve
  {
    auto* l3 = model->add_l3_curves();
    l3->set_freq_khz(1500000);
    l3->set_dep_policy(4);
    l3->set_dep_freq_khz(2000000);
    l3->set_l3_hit_mw(12.5);
    l3->set_l3_miss_mw(34.0);
  }

  // GPU curve
  {
    auto* gpu = model->add_gpu_curves();
    gpu->set_freq_khz(800000);
    gpu->set_active_mw(280.0);
    gpu->set_idle1_mw(6.0);
    gpu->set_idle2_mw(1.5);
  }

  // TPU curve
  {
    auto* tpu = model->add_tpu_curves();
    tpu->set_cluster(0);
    tpu->set_requests(1);
    tpu->set_freq_khz(1200000);
    tpu->set_active_mw(420.0);
  }

  auto model_bytes = model.SerializeAsArray();

  // Wrap in TracePacket
  auto* packet = trace->add_packet();
  packet->set_timestamp(1000);
  packet->AppendBytes(FrameworksBaseTracePacket::kWattsonSocModelFieldNumber,
                      reinterpret_cast<const char*>(model_bytes.data()),
                      model_bytes.size());

  auto trace_bytes = trace.SerializeAsArray();
  Tokenize(trace_bytes.data(), trace_bytes.size());

  // 1. Verify Device Info Table
  const auto& dev_table = storage_->wattson_custom_device_info_table();
  ASSERT_EQ(dev_table.row_count(), 1u);
  EXPECT_STREQ(storage_->GetString(dev_table[0].device_name()).c_str(),
               "Tensor G6");
  EXPECT_EQ(dev_table[0].use_devfreq(), 1);
  EXPECT_EQ(dev_table[0].gpu_id().value_or(-1), 0);

  // 2. Verify CPU Policies Table
  const auto& policy_table = storage_->wattson_custom_cpu_policy_table();
  ASSERT_EQ(policy_table.row_count(), 8u);
  EXPECT_EQ(policy_table[0].cpu(), 0);
  EXPECT_EQ(policy_table[0].policy(), 0);
  EXPECT_EQ(policy_table[4].cpu(), 4);
  EXPECT_EQ(policy_table[4].policy(), 4);
  EXPECT_EQ(policy_table[7].cpu(), 7);
  EXPECT_EQ(policy_table[7].policy(), 7);

  // 3. Verify Deep Idle Offsets
  const auto& dio_table = storage_->wattson_custom_deep_idle_offset_table();
  ASSERT_EQ(dio_table.row_count(), 2u);
  EXPECT_EQ(dio_table[0].cpu(), 0);
  EXPECT_EQ(dio_table[0].offset_ns(), 0);
  EXPECT_EQ(dio_table[1].cpu(), 7);
  EXPECT_EQ(dio_table[1].offset_ns(), 200000);

  // 4. Verify Idle State Map
  const auto& idle_table = storage_->wattson_custom_idle_state_map_table();
  ASSERT_EQ(idle_table.row_count(), 1u);
  EXPECT_EQ(idle_table[0].nominal_idle(), 2);
  EXPECT_EQ(idle_table[0].override_idle(), 1);

  // 5. Verify Vote By Freq
  const auto& vbf_table = storage_->wattson_custom_vote_by_freq_table();
  ASSERT_EQ(vbf_table.row_count(), 1u);
  EXPECT_EQ(vbf_table[0].cpu(), 7);

  // 6. Verify 1D Curves
  const auto& c1d_table = storage_->wattson_custom_curves_cpu_1d_table();
  ASSERT_EQ(c1d_table.row_count(), 1u);
  EXPECT_EQ(c1d_table[0].policy(), 0);
  EXPECT_EQ(c1d_table[0].freq_khz(), 1000000);
  EXPECT_THAT(c1d_table[0].static_mw(), DoubleEq(5.5));
  EXPECT_THAT(c1d_table[0].active_mw(), DoubleEq(45.0));

  // 7. Verify 2D Curves
  const auto& c2d_table = storage_->wattson_custom_curves_cpu_2d_table();
  ASSERT_EQ(c2d_table.row_count(), 1u);
  EXPECT_EQ(c2d_table[0].policy(), 7);
  EXPECT_EQ(c2d_table[0].freq_khz(), 3000000);
  EXPECT_EQ(c2d_table[0].dep_policy(), 4);
  EXPECT_EQ(c2d_table[0].dep_freq(), 2000000);
  EXPECT_THAT(c2d_table[0].static_mw(), DoubleEq(25.0));
  EXPECT_THAT(c2d_table[0].active_mw(), DoubleEq(350.0));
  EXPECT_THAT(c2d_table[0].interconnect_mw(), DoubleEq(18.0));

  // 8. Verify L3 Curves
  const auto& l3_table = storage_->wattson_custom_curves_l3_table();
  ASSERT_EQ(l3_table.row_count(), 1u);
  EXPECT_EQ(l3_table[0].freq_khz(), 1500000);
  EXPECT_THAT(l3_table[0].l3_hit_mw(), DoubleEq(12.5));
  EXPECT_THAT(l3_table[0].l3_miss_mw(), DoubleEq(34.0));

  // 9. Verify GPU Curves
  const auto& gpu_table = storage_->wattson_custom_curves_gpu_table();
  ASSERT_EQ(gpu_table.row_count(), 1u);
  EXPECT_EQ(gpu_table[0].freq_khz(), 800000);
  EXPECT_THAT(gpu_table[0].active_mw(), DoubleEq(280.0));

  // 10. Verify TPU Curves
  const auto& tpu_table = storage_->wattson_custom_curves_tpu_table();
  ASSERT_EQ(tpu_table.row_count(), 1u);
  EXPECT_EQ(tpu_table[0].cluster(), 0);
  EXPECT_EQ(tpu_table[0].requests(), 1);
  EXPECT_EQ(tpu_table[0].freq(), 1200000);
  EXPECT_THAT(tpu_table[0].active_mw(), DoubleEq(420.0));
}

}  // namespace
}  // namespace perfetto::trace_processor
