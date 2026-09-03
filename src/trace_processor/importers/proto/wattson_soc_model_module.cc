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

#include <array>
#include <cstddef>
#include <cstdint>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/wattson_soc_model.pbzero.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/wattson_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

using ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using ::com::android::internal::pbzero::WattsonCpuCurve;
using ::com::android::internal::pbzero::WattsonCpuPolicy;
using ::com::android::internal::pbzero::WattsonIdleStateMap;
using ::com::android::internal::pbzero::WattsonSocModel;
using ::com::android::internal::pbzero::WattsonSubsystemCurve;
using ::protozero::ConstBytes;

namespace {

// Number of idle levels the Wattson tables have a column for. Every curve set
// in the proto characterises at most two.
constexpr size_t kMaxIdleLevels = 2;

// Decodes the packed `idle_mw` array of a curve. The array is ordered from the
// shallowest idle level to the deepest; levels the curve set does not
// characterise are absent and stay at 0.
template <typename Decoder>
std::array<double, kMaxIdleLevels> ReadIdleMw(const Decoder& curve) {
  std::array<double, kMaxIdleLevels> idle{};
  bool parse_error = false;
  size_t i = 0;
  for (auto it = curve.idle_mw(&parse_error); it && i < idle.size(); ++it) {
    idle[i++] = static_cast<double>(*it);
  }
  return idle;
}

}  // namespace

WattsonSocModelModule::WattsonSocModelModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context)
    : ProtoImporterModule(module_context), context_(context) {
  RegisterForField(FrameworksBaseTracePacket::kWattsonSocModelFieldNumber);
}

WattsonSocModelModule::~WattsonSocModelModule() = default;

void WattsonSocModelModule::ParseField(const ParseFieldArgs& args) {
  if (args.field.id() ==
      FrameworksBaseTracePacket::kWattsonSocModelFieldNumber) {
    ParseWattsonSocModel(
        args.field.Cast<FrameworksBaseTracePacket::kWattsonSocModel>());
  }
}

void WattsonSocModelModule::ParseWattsonSocModel(ConstBytes blob) {
  // Ignore duplicate model packets within the same trace to prevent row
  // duplication and downstream join explosions.
  if (context_->storage->wattson_external_device_info_table().row_count() > 0) {
    return;
  }

  WattsonSocModel::Decoder model(blob);

  // 1. Device Info / Metadata
  StringId device_name_id =
      model.has_device_name()
          ? context_->storage->InternString(model.device_name())
          : context_->storage->InternString("External_SoC");

  tables::WattsonExternalDeviceInfoTable::Row info_row;
  info_row.device_name = device_name_id;
  info_row.use_devfreq =
      (model.has_use_devfreq() && model.use_devfreq()) ? 1 : 0;
  if (model.has_gpu_id()) {
    info_row.gpu_id = model.gpu_id();
  }
  context_->storage->mutable_wattson_external_device_info_table()->Insert(
      info_row);

  // 2. CPU Policies & Topology
  for (auto pol_it = model.cpu_policies(); pol_it; ++pol_it) {
    WattsonCpuPolicy::Decoder pol(*pol_it);
    int32_t policy = static_cast<int32_t>(pol.policy());
    bool parse_error = false;
    for (auto cpu_it = pol.cpus(&parse_error); cpu_it; ++cpu_it) {
      int32_t cpu = static_cast<int32_t>(*cpu_it);

      tables::WattsonExternalCpuPolicyTable::Row row;
      row.cpu = cpu;
      row.policy = policy;
      context_->storage->mutable_wattson_external_cpu_policy_table()->Insert(
          row);

      if (pol.has_deep_idle_offset_ns()) {
        tables::WattsonExternalDeepIdleOffsetTable::Row dio_row;
        dio_row.cpu = cpu;
        dio_row.offset_ns = pol.deep_idle_offset_ns();
        context_->storage->mutable_wattson_external_deep_idle_offset_table()
            ->Insert(dio_row);
      }

      if (pol.has_vote_by_freq() && pol.vote_by_freq()) {
        tables::WattsonExternalVoteByFreqTable::Row vbf_row;
        vbf_row.cpu = cpu;
        context_->storage->mutable_wattson_external_vote_by_freq_table()
            ->Insert(vbf_row);
      }
    }
  }

  // 3. Idle State Map
  for (auto it = model.idle_state_map(); it; ++it) {
    WattsonIdleStateMap::Decoder idle(*it);
    tables::WattsonExternalIdleStateMapTable::Row row;
    row.nominal_idle = idle.nominal_idle();
    row.override_idle = static_cast<int32_t>(idle.override_idle());
    context_->storage->mutable_wattson_external_idle_state_map_table()->Insert(
        row);
  }

  // 4. Unified CPU Curves (1D, 2D, and L3)
  for (auto cpu_it = model.cpu_curves(); cpu_it; ++cpu_it) {
    WattsonCpuCurve::Decoder curve(*cpu_it);
    const bool is_2d = curve.has_dep_policy() || curve.has_dep_freq_khz();
    if (!is_2d) {
      tables::WattsonExternalCurvesCpu1DTable::Row row;
      row.policy = static_cast<int32_t>(curve.policy());
      row.freq_khz = static_cast<int64_t>(curve.freq_khz());
      row.static_mw = static_cast<double>(curve.static_mw());
      row.active_mw = static_cast<double>(curve.active_mw());
      const auto idle = ReadIdleMw(curve);
      row.idle0_mw = idle[0];
      row.idle1_mw = idle[1];
      context_->storage->mutable_wattson_external_curves_cpu_1d_table()->Insert(
          row);
      continue;
    }

    if (curve.has_active_mw() || curve.has_static_mw() || curve.has_idle_mw() ||
        curve.has_interconnect_mw()) {
      tables::WattsonExternalCurvesCpu2DTable::Row row;
      row.policy = static_cast<int32_t>(curve.policy());
      row.freq_khz = static_cast<int64_t>(curve.freq_khz());
      row.dep_policy = static_cast<int32_t>(curve.dep_policy());
      row.dep_freq = static_cast<int64_t>(curve.dep_freq_khz());
      row.static_mw = static_cast<double>(curve.static_mw());
      row.active_mw = static_cast<double>(curve.active_mw());
      const auto idle = ReadIdleMw(curve);
      row.idle0_mw = idle[0];
      row.idle1_mw = idle[1];
      row.interconnect_mw = static_cast<double>(curve.interconnect_mw());
      context_->storage->mutable_wattson_external_curves_cpu_2d_table()->Insert(
          row);
    }

    if (curve.has_l3_hit_mw() || curve.has_l3_miss_mw()) {
      tables::WattsonExternalCurvesL3Table::Row row;
      row.freq_khz = static_cast<int64_t>(curve.freq_khz());
      row.dep_policy = static_cast<int32_t>(curve.dep_policy());
      row.dep_freq = static_cast<int64_t>(curve.dep_freq_khz());
      row.l3_hit_mw = static_cast<double>(curve.l3_hit_mw());
      row.l3_miss_mw = static_cast<double>(curve.l3_miss_mw());
      context_->storage->mutable_wattson_external_curves_l3_table()->Insert(
          row);
    }
  }

  // 5. Generic Subsystem Curves (GPU, TPU)
  for (auto sub_it = model.subsystem_curves(); sub_it; ++sub_it) {
    WattsonSubsystemCurve::Decoder curve(*sub_it);
    base::StringView subsystem = curve.subsystem();
    if (subsystem == "gpu") {
      tables::WattsonExternalCurvesGpuTable::Row row;
      row.freq_khz = static_cast<int64_t>(curve.freq_khz());
      bool parse_error = false;
      auto active_it = curve.active_mw(&parse_error);
      row.active_mw = active_it ? static_cast<double>(*active_it) : 0.0;
      // The GPU curve set characterises idle levels 1 and 2 (there is no
      // measured level 0), so the shallowest entry of the array lands in
      // `idle1_mw`.
      const auto idle = ReadIdleMw(curve);
      row.idle1_mw = idle[0];
      row.idle2_mw = idle[1];
      context_->storage->mutable_wattson_external_curves_gpu_table()->Insert(
          row);
    } else if (subsystem == "tpu") {
      bool parse_error = false;
      int32_t requests = 0;
      for (auto active_it = curve.active_mw(&parse_error); active_it;
           ++active_it, ++requests) {
        tables::WattsonExternalCurvesTpuTable::Row row;
        row.cluster = static_cast<int32_t>(curve.domain());
        row.requests = requests;
        row.freq = static_cast<int64_t>(curve.freq_khz());
        row.active_mw = static_cast<double>(*active_it);
        context_->storage->mutable_wattson_external_curves_tpu_table()->Insert(
            row);
      }
    }
  }
}

}  // namespace perfetto::trace_processor
