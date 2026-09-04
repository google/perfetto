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
#include <string>

#include "perfetto/ext/base/flat_hash_map.h"
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
using ::com::android::internal::pbzero::WattsonCpu1DCurve;
using ::com::android::internal::pbzero::WattsonCpu2DCurve;
using ::com::android::internal::pbzero::WattsonCpuTopology;
using ::com::android::internal::pbzero::WattsonGpuCurve;
using ::com::android::internal::pbzero::WattsonL3Curve;
using ::com::android::internal::pbzero::WattsonSocModel;
using ::com::android::internal::pbzero::WattsonTpuCurve;
using ::protozero::ConstBytes;

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
  WattsonSocModel::Decoder model(blob);

  // 1. Device Info / Metadata
  StringId device_name_id =
      model.has_device_name()
          ? context_->storage->InternString(model.device_name())
          : context_->storage->InternString("Custom_SoC");

  tables::WattsonCustomDeviceInfoTable::Row info_row;
  info_row.device_name = device_name_id;
  info_row.use_devfreq = 0;
  if (model.has_gpu_id()) {
    info_row.gpu_id = model.gpu_id();
  }

  // 2. Structured CPU Topology
  if (model.has_cpu_topology()) {
    WattsonCpuTopology::Decoder topology(model.cpu_topology());
    if (topology.has_use_devfreq()) {
      info_row.use_devfreq = topology.use_devfreq() ? 1 : 0;
    }

    base::FlatHashMap<int32_t, int64_t> policy_deep_idle;
    base::FlatHashMap<int32_t, bool> policy_vote_by_freq;

    for (auto it = topology.policy_configs(); it; ++it) {
      WattsonCpuTopology::PolicyConfig::Decoder cfg(*it);
      if (cfg.has_policy()) {
        if (cfg.has_deep_idle_offset_ns()) {
          policy_deep_idle[cfg.policy()] = cfg.deep_idle_offset_ns();
        }
        if (cfg.has_vote_by_freq()) {
          policy_vote_by_freq[cfg.policy()] = cfg.vote_by_freq();
        }
      }
    }

    for (auto it = topology.cpu_map(); it; ++it) {
      WattsonCpuTopology::CpuMap::Decoder cpu_map(*it);
      int32_t cpu = cpu_map.cpu();
      int32_t policy = cpu_map.policy();

      tables::WattsonCustomCpuPolicyTable::Row row;
      row.cpu = cpu;
      row.policy = policy;
      context_->storage->mutable_wattson_custom_cpu_policy_table()->Insert(row);

      if (cpu_map.has_deep_idle_offset_ns()) {
        tables::WattsonCustomDeepIdleOffsetTable::Row dio_row;
        dio_row.cpu = cpu;
        dio_row.offset_ns = cpu_map.deep_idle_offset_ns();
        context_->storage->mutable_wattson_custom_deep_idle_offset_table()
            ->Insert(dio_row);
      } else if (auto* offset = policy_deep_idle.Find(policy)) {
        tables::WattsonCustomDeepIdleOffsetTable::Row dio_row;
        dio_row.cpu = cpu;
        dio_row.offset_ns = *offset;
        context_->storage->mutable_wattson_custom_deep_idle_offset_table()
            ->Insert(dio_row);
      }

      bool vote = cpu_map.has_vote_by_freq()
                      ? cpu_map.vote_by_freq()
                      : (policy_vote_by_freq.Find(policy)
                             ? *policy_vote_by_freq.Find(policy)
                             : false);
      if (vote) {
        tables::WattsonCustomVoteByFreqTable::Row vbf_row;
        vbf_row.cpu = cpu;
        context_->storage->mutable_wattson_custom_vote_by_freq_table()->Insert(
            vbf_row);
      }
    }

    for (auto it = topology.idle_state_map(); it; ++it) {
      WattsonCpuTopology::IdleStateOverride::Decoder idle(*it);
      tables::WattsonCustomIdleStateMapTable::Row row;
      row.nominal_idle = idle.nominal_idle();
      row.override_idle = static_cast<int32_t>(idle.override_idle());
      context_->storage->mutable_wattson_custom_idle_state_map_table()->Insert(
          row);
    }
  }

  context_->storage->mutable_wattson_custom_device_info_table()->Insert(
      info_row);

  // 3. CPU 1D Curves (Field 16, fallback Field 8)
  auto cpu_1d_it = model.has_cpu_1d_curves() ? model.cpu_1d_curves()
                                             : model.legacy_cpu_1d_curves();
  for (; cpu_1d_it; ++cpu_1d_it) {
    WattsonCpu1DCurve::Decoder curve(*cpu_1d_it);
    tables::WattsonCustomCurvesCpu1DTable::Row row;
    row.policy = static_cast<int32_t>(curve.policy());
    row.freq_khz = static_cast<int64_t>(curve.freq_khz());
    row.static_mw = curve.static_mw();
    row.active_mw = curve.active_mw();
    row.idle0_mw = curve.idle0_mw();
    row.idle1_mw = curve.idle1_mw();
    context_->storage->mutable_wattson_custom_curves_cpu_1d_table()->Insert(
        row);
  }

  // 4. CPU 2D Curves (Field 17, fallback Field 9)
  auto cpu_2d_it = model.has_cpu_2d_curves() ? model.cpu_2d_curves()
                                             : model.legacy_cpu_2d_curves();
  for (; cpu_2d_it; ++cpu_2d_it) {
    WattsonCpu2DCurve::Decoder curve(*cpu_2d_it);
    tables::WattsonCustomCurvesCpu2DTable::Row row;
    row.policy = static_cast<int32_t>(curve.policy());
    row.freq_khz = static_cast<int64_t>(curve.freq_khz());
    row.dep_policy = static_cast<int32_t>(curve.dep_policy());
    row.dep_freq = static_cast<int64_t>(curve.dep_freq_khz());
    row.static_mw = curve.static_mw();
    row.active_mw = curve.active_mw();
    row.idle0_mw = curve.idle0_mw();
    row.idle1_mw = curve.idle1_mw();
    row.interconnect_mw = curve.interconnect_mw();
    context_->storage->mutable_wattson_custom_curves_cpu_2d_table()->Insert(
        row);
  }

  // 5. L3 Curves (Field 19, fallback Field 10)
  auto l3_it =
      model.has_l3_curves() ? model.l3_curves() : model.legacy_l3_curves();
  for (; l3_it; ++l3_it) {
    WattsonL3Curve::Decoder curve(*l3_it);
    tables::WattsonCustomCurvesL3Table::Row row;
    row.freq_khz = static_cast<int64_t>(curve.freq_khz());
    row.dep_policy = static_cast<int32_t>(curve.dep_policy());
    row.dep_freq = static_cast<int64_t>(curve.dep_freq_khz());
    row.l3_hit_mw = curve.l3_hit_mw();
    row.l3_miss_mw = curve.l3_miss_mw();
    context_->storage->mutable_wattson_custom_curves_l3_table()->Insert(row);
  }

  // 6. GPU Curves (Field 18, fallback Field 11)
  auto gpu_it =
      model.has_gpu_curves() ? model.gpu_curves() : model.legacy_gpu_curves();
  for (; gpu_it; ++gpu_it) {
    WattsonGpuCurve::Decoder curve(*gpu_it);
    tables::WattsonCustomCurvesGpuTable::Row row;
    row.freq_khz = static_cast<int64_t>(curve.freq_khz());
    row.active_mw = curve.active_mw();
    row.idle1_mw = curve.idle1_mw();
    row.idle2_mw = curve.idle2_mw();
    context_->storage->mutable_wattson_custom_curves_gpu_table()->Insert(row);
  }

  // 7. TPU Curves (Field 20, fallback Field 12)
  auto tpu_it =
      model.has_tpu_curves() ? model.tpu_curves() : model.legacy_tpu_curves();
  for (; tpu_it; ++tpu_it) {
    WattsonTpuCurve::Decoder curve(*tpu_it);
    tables::WattsonCustomCurvesTpuTable::Row row;
    row.cluster = static_cast<int32_t>(curve.cluster());
    row.requests = static_cast<int32_t>(curve.requests());
    row.freq = static_cast<int64_t>(curve.freq_khz());
    row.active_mw = curve.active_mw();
    context_->storage->mutable_wattson_custom_curves_tpu_table()->Insert(row);
  }
}

}  // namespace perfetto::trace_processor
