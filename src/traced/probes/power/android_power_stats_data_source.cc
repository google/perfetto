/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/traced/probes/power/android_power_stats_data_source.h"

#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/android_internal/health_hal.h"
#include "src/android_internal/lazy_library_loader.h"
#include "src/android_internal/power_stats_aidl.h"

#include "protos/perfetto/trace/power/android_energy_estimation_breakdown.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {
constexpr uint32_t kPollIntervalMs = 100;
constexpr size_t kMaxNumPowerEntities = 256;
}  // namespace

// static
const ProbesDataSource::Descriptor AndroidPowerStatsDataSource::descriptor = {
    /*name*/ "android.power_stats",
    /*flags*/ Descriptor::kFlagsNone,
};

// Dynamically loads the libperfetto_android_internal.so library which
// allows to proxy calls to android hwbinder in in-tree builds.
struct AndroidPowerStatsDataSource::DynamicLibLoader {
  PERFETTO_LAZY_LOAD(android_internal::GetEnergyConsumed, get_energy_consumed_);

  std::vector<android_internal::EnergyEstimationBreakdown> GetEnergyConsumed() {
    if (!get_energy_consumed_)
      return std::vector<android_internal::EnergyEstimationBreakdown>();

    std::vector<android_internal::EnergyEstimationBreakdown> energy_breakdown(
        kMaxNumPowerEntities);
    size_t num_power_entities = energy_breakdown.size();
    get_energy_consumed_(&energy_breakdown[0], &num_power_entities);
    energy_breakdown.resize(num_power_entities);
    return energy_breakdown;
  }
};

AndroidPowerStatsDataSource::AndroidPowerStatsDataSource(
    DataSourceConfig,
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {}

AndroidPowerStatsDataSource::~AndroidPowerStatsDataSource() = default;

void AndroidPowerStatsDataSource::Start() {
  lib_.reset(new DynamicLibLoader());
  Tick();
}

void AndroidPowerStatsDataSource::Tick() {
  // Post next task.
  auto now_ms = base::GetWallTimeMs().count();
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this] {
        if (weak_this)
          weak_this->Tick();
      },
      kPollIntervalMs - static_cast<uint32_t>(now_ms % kPollIntervalMs));
  WriteEnergyEstimationBreakdown();
}

void AndroidPowerStatsDataSource::WriteEnergyEstimationBreakdown() {
  auto energy_breakdowns = lib_->GetEnergyConsumed();
  auto timestamp = static_cast<uint64_t>(base::GetBootTimeNs().count());

  TraceWriter::TracePacketHandle packet;
  protos::pbzero::AndroidEnergyEstimationBreakdown* energy_estimation_proto =
      nullptr;
  for (const auto& breakdown : energy_breakdowns) {
    if (breakdown.uid == android_internal::ALL_UIDS_FOR_CONSUMER) {
      // Finalize packet before calling NewTracePacket.
      if (packet) {
        packet->Finalize();
      }
      packet = writer_->NewTracePacket();
      packet->set_timestamp(timestamp);
      energy_estimation_proto =
          packet->set_android_energy_estimation_breakdown();
      energy_estimation_proto->set_energy_consumer_id(
          breakdown.energy_consumer_id);
      energy_estimation_proto->set_energy_uws(breakdown.energy_uws);
    } else {
      PERFETTO_CHECK(energy_estimation_proto != nullptr);
      auto* uid_breakdown_proto =
          energy_estimation_proto->add_per_uid_breakdown();
      uid_breakdown_proto->set_uid(breakdown.uid);
      uid_breakdown_proto->set_energy_uws(breakdown.energy_uws);
    }
  }
}

void AndroidPowerStatsDataSource::Flush(FlushRequestID,
                                        std::function<void()> callback) {
  writer_->Flush(callback);
}

}  // namespace perfetto
