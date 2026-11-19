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

#include "src/trace_processor/plugins/wattson/wattson_soc_model_module.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_wattson_soc_model.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/wattson/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::wattson {

// Holds the plugin-owned intrinsic tables populated from an out-of-tree
// WattsonSocModel TracePacket extension.
struct WattsonExternalTables {
  explicit WattsonExternalTables(StringPool* pool)
      : device_info(pool),
        cpu_cluster(pool),
        idle_state_map(pool),
        curves_cpu_1d(pool),
        curves_cpu_2d(pool),
        curves_l3(pool),
        curves_gpu(pool),
        curves_tpu(pool) {}

  tables::WattsonExternalDeviceInfoTable device_info;
  tables::WattsonExternalCpuClusterTable cpu_cluster;
  tables::WattsonExternalIdleStateMapTable idle_state_map;
  tables::WattsonExternalCurvesCpu1DTable curves_cpu_1d;
  tables::WattsonExternalCurvesCpu2DTable curves_cpu_2d;
  tables::WattsonExternalCurvesL3Table curves_l3;
  tables::WattsonExternalCurvesGpuTable curves_gpu;
  tables::WattsonExternalCurvesTpuTable curves_tpu;
};

void WattsonExternalTablesDeleter::operator()(
    WattsonExternalTables* tables) const {
  delete tables;
}

WattsonExternalTablesPtr CreateWattsonExternalTables(StringPool* pool) {
  return WattsonExternalTablesPtr(new WattsonExternalTables(pool));
}

void RegisterWattsonExternalDataframes(WattsonExternalTables* tables,
                                       std::vector<PluginDataframe>& out) {
  out.push_back({&tables->device_info.dataframe(),
                 tables::WattsonExternalDeviceInfoTable::Name(),
                 {}});
  out.push_back({&tables->cpu_cluster.dataframe(),
                 tables::WattsonExternalCpuClusterTable::Name(),
                 {}});
  out.push_back({&tables->idle_state_map.dataframe(),
                 tables::WattsonExternalIdleStateMapTable::Name(),
                 {}});
  out.push_back({&tables->curves_cpu_1d.dataframe(),
                 tables::WattsonExternalCurvesCpu1DTable::Name(),
                 {}});
  out.push_back({&tables->curves_cpu_2d.dataframe(),
                 tables::WattsonExternalCurvesCpu2DTable::Name(),
                 {}});
  out.push_back({&tables->curves_l3.dataframe(),
                 tables::WattsonExternalCurvesL3Table::Name(),
                 {}});
  out.push_back({&tables->curves_gpu.dataframe(),
                 tables::WattsonExternalCurvesGpuTable::Name(),
                 {}});
  out.push_back({&tables->curves_tpu.dataframe(),
                 tables::WattsonExternalCurvesTpuTable::Name(),
                 {}});
}

using ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using ::com::android::internal::pbzero::WattsonCpuCluster;
using ::com::android::internal::pbzero::WattsonCurve;
using ::com::android::internal::pbzero::WattsonIdleStateMap;
using ::com::android::internal::pbzero::WattsonSocModel;
using ::com::android::internal::pbzero::WattsonSubsystem;
using ::protozero::ConstBytes;

namespace {

// Number of idle levels the Wattson tables have a column for. Every curve set
// in the proto characterises at most two.
constexpr size_t kMaxIdleLevels = 2;

// Sentinel cluster ID used for the shared DSU/SCU domain on CPU curves.
constexpr uint32_t kDsuClusterId = 255;

// Reads the first entry of the packed `active_mw` array on single-request DVFS
// curves (CPU, GPU), returning `std::nullopt` if absent or malformed.
std::optional<double> ReadFirstActiveMw(const WattsonCurve::Decoder& curve) {
  bool parse_error = false;
  auto it = curve.active_mw(&parse_error);
  if (!it || parse_error) {
    return std::nullopt;
  }
  return static_cast<double>(*it);
}

// Decodes the packed `idle_mw` array of a curve. The array is ordered from the
// shallowest idle level to the deepest; levels the curve set does not
// characterise are absent and stay at 0. Returns `std::nullopt` if `idle_mw`
// is absent or malformed.
std::optional<std::array<double, kMaxIdleLevels>> ReadIdleMw(
    const WattsonCurve::Decoder& curve) {
  bool parse_error = false;
  auto it = curve.idle_mw(&parse_error);
  if (!it || parse_error) {
    return std::nullopt;
  }
  std::array<double, kMaxIdleLevels> idle{};
  for (size_t i = 0; it && i < idle.size(); ++it, ++i) {
    idle[i] = static_cast<double>(*it);
  }
  return idle;
}

}  // namespace

WattsonSocModelModule::WattsonSocModelModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context,
    WattsonExternalTables* tables)
    : ProtoImporterModule(module_context), context_(context), tables_(tables) {
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
  if (tables_->device_info.row_count() > 0) {
    return;
  }

  WattsonSocModel::Decoder model(blob);

  StringId device_name_id =
      (model.has_device_name() && model.device_name().size > 0)
          ? context_->storage->InternString(model.device_name())
          : context_->storage->InternString("External_SoC");

  tables::WattsonExternalDeviceInfoTable::Row info_row;
  info_row.device_name = device_name_id;
  info_row.use_devfreq =
      (model.has_use_devfreq() && model.use_devfreq()) ? 1 : 0;
  if (model.has_gpu_id()) {
    info_row.gpu_id = model.gpu_id();
  }
  if (model.has_model_version()) {
    info_row.model_version = static_cast<int32_t>(model.model_version());
  }
  if (model.has_model_source()) {
    info_row.model_source = model.model_source();
  }
  tables_->device_info.Insert(info_row);

  for (auto it = model.cpu_clusters(); it; ++it) {
    ParseCpuCluster(*it);
  }
  for (auto it = model.idle_state_map(); it; ++it) {
    ParseIdleStateMap(*it);
  }
  for (auto it = model.subsystems(); it; ++it) {
    ParseSubsystem(*it);
  }
}

void WattsonSocModelModule::ParseCpuCluster(ConstBytes blob) {
  WattsonCpuCluster::Decoder cluster_dec(blob);
  if (!cluster_dec.has_cluster()) {
    return;
  }
  const int32_t cluster = static_cast<int32_t>(cluster_dec.cluster());
  const int64_t offset_ns = cluster_dec.has_deep_idle_offset_ns()
                                ? cluster_dec.deep_idle_offset_ns()
                                : 0;
  const int32_t vote_by_freq =
      (cluster_dec.has_vote_by_freq() && cluster_dec.vote_by_freq()) ? 1 : 0;
  bool parse_error = false;
  auto cpu_it = cluster_dec.cpus(&parse_error);
  if (parse_error) {
    return;
  }
  for (; cpu_it; ++cpu_it) {
    tables::WattsonExternalCpuClusterTable::Row row;
    row.cpu = static_cast<int32_t>(*cpu_it);
    row.cluster = cluster;
    row.deep_idle_offset_ns = offset_ns;
    row.vote_by_freq = vote_by_freq;
    tables_->cpu_cluster.Insert(row);
  }
}

void WattsonSocModelModule::ParseIdleStateMap(ConstBytes blob) {
  WattsonIdleStateMap::Decoder idle(blob);
  if (!idle.has_nominal_idle() || !idle.has_override_idle()) {
    return;
  }
  tables::WattsonExternalIdleStateMapTable::Row row;
  row.nominal_idle = idle.nominal_idle();
  row.override_idle = static_cast<int32_t>(idle.override_idle());
  tables_->idle_state_map.Insert(row);
}

void WattsonSocModelModule::ParseSubsystem(ConstBytes blob) {
  WattsonSubsystem::Decoder sub(blob);
  base::StringView name = sub.name();
  if (name == "cpu") {
    for (auto it = sub.curves(); it; ++it) {
      ParseCpuCurve(*it);
    }
  } else if (name == "gpu") {
    for (auto it = sub.curves(); it; ++it) {
      ParseGpuCurve(*it);
    }
  } else if (name == "tpu") {
    for (auto it = sub.curves(); it; ++it) {
      ParseTpuCurve(*it);
    }
  }
}

void WattsonSocModelModule::ParseCpuCurve(ConstBytes blob) {
  WattsonCurve::Decoder curve(blob);
  if (!curve.has_cluster() || !curve.has_freq()) {
    return;
  }
  // 2D dependency fields and L3 hit/miss coefficients must each be set in
  // pairs when present.
  if (curve.has_dep_cluster() != curve.has_dep_freq() ||
      curve.has_l3_hit() != curve.has_l3_miss()) {
    return;
  }

  const auto active_mw = ReadFirstActiveMw(curve);
  const auto idle_mw = ReadIdleMw(curve);
  const bool is_2d = curve.has_dep_cluster() && curve.has_dep_freq();

  if (!is_2d) {
    // DSU 1D curves (cluster == 255) only characterise active power; regular
    // CPU cluster 1D curves must provide both active_mw and idle_mw.
    const bool valid_1d = (curve.cluster() == kDsuClusterId)
                              ? active_mw.has_value()
                              : (active_mw.has_value() && idle_mw.has_value());
    if (valid_1d) {
      tables::WattsonExternalCurvesCpu1DTable::Row row;
      row.cluster = static_cast<int32_t>(curve.cluster());
      row.freq_khz = static_cast<int64_t>(curve.freq());
      row.static_mw = static_cast<double>(curve.static_mw());
      row.active = *active_mw;
      if (idle_mw.has_value()) {
        row.idle0 = (*idle_mw)[0];
        row.idle1 = (*idle_mw)[1];
      }
      tables_->curves_cpu_1d.Insert(row);
    }
  } else if ((active_mw.has_value() && idle_mw.has_value()) ||
             curve.has_interconnect_mw()) {
    tables::WattsonExternalCurvesCpu2DTable::Row row;
    row.cluster = static_cast<int32_t>(curve.cluster());
    row.freq_khz = static_cast<int64_t>(curve.freq());
    row.dep_cluster = static_cast<int32_t>(curve.dep_cluster());
    row.dep_freq = static_cast<int64_t>(curve.dep_freq());
    row.static_mw = static_cast<double>(curve.static_mw());
    row.active = active_mw.value_or(0.0);
    if (idle_mw.has_value()) {
      row.idle0 = (*idle_mw)[0];
      row.idle1 = (*idle_mw)[1];
    }
    row.interconnect = static_cast<double>(curve.interconnect_mw());
    tables_->curves_cpu_2d.Insert(row);
  }

  if (curve.has_l3_hit() && curve.has_l3_miss()) {
    tables::WattsonExternalCurvesL3Table::Row row;
    row.freq_khz = static_cast<int64_t>(curve.freq());
    row.dep_cluster = static_cast<int32_t>(curve.dep_cluster());
    row.dep_freq = static_cast<int64_t>(curve.dep_freq());
    row.l3_hit = static_cast<double>(curve.l3_hit());
    row.l3_miss = static_cast<double>(curve.l3_miss());
    tables_->curves_l3.Insert(row);
  }
}

void WattsonSocModelModule::ParseGpuCurve(ConstBytes blob) {
  WattsonCurve::Decoder curve(blob);
  const auto active_mw = ReadFirstActiveMw(curve);
  const auto idle_mw = ReadIdleMw(curve);
  if (!curve.has_freq() || !active_mw.has_value() || !idle_mw.has_value()) {
    return;
  }

  tables::WattsonExternalCurvesGpuTable::Row row;
  row.freq_khz = static_cast<int64_t>(curve.freq());
  row.active = *active_mw;
  // The GPU curve set characterises idle levels 1 and 2 (there is no
  // measured level 0), so the shallowest entry of the array lands in `idle1`.
  row.idle1 = (*idle_mw)[0];
  row.idle2 = (*idle_mw)[1];
  tables_->curves_gpu.Insert(row);
}

void WattsonSocModelModule::ParseTpuCurve(ConstBytes blob) {
  WattsonCurve::Decoder curve(blob);
  if (!curve.has_cluster() || !curve.has_freq()) {
    return;
  }

  bool parse_error = false;
  auto active_it = curve.active_mw(&parse_error);
  if (!active_it || parse_error) {
    return;
  }

  int32_t requests = 0;
  for (; active_it; ++active_it, ++requests) {
    tables::WattsonExternalCurvesTpuTable::Row row;
    row.cluster = static_cast<int32_t>(curve.cluster());
    row.requests = requests;
    row.freq = static_cast<int64_t>(curve.freq());
    row.active = static_cast<double>(*active_it);
    tables_->curves_tpu.Insert(row);
  }
}

}  // namespace perfetto::trace_processor::wattson
