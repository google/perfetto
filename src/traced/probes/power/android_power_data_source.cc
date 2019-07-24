/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/traced/probes/power/android_power_data_source.h"

#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/optional.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/android_internal/health_hal.h"
#include "src/android_internal/lazy_library_loader.h"
#include "src/android_internal/power_stats_hal.h"

#include "perfetto/trace/power/battery_counters.pbzero.h"
#include "perfetto/trace/power/power_rails.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {
constexpr uint32_t kMinPollRateMs = 250;
constexpr size_t kMaxNumRails = 32;
}  // namespace

// Dynamically loads the libperfetto_android_internal.so library which
// allows to proxy calls to android hwbinder in in-tree builds.
struct AndroidPowerDataSource::DynamicLibLoader {
  PERFETTO_LAZY_LOAD(android_internal::GetBatteryCounter, get_battery_counter_);
  PERFETTO_LAZY_LOAD(android_internal::GetAvailableRails, get_available_rails_);
  PERFETTO_LAZY_LOAD(android_internal::GetRailEnergyData,
                     get_rail_energy_data_);

  base::Optional<int64_t> GetCounter(android_internal::BatteryCounter counter) {
    if (!get_battery_counter_)
      return base::nullopt;
    int64_t value = 0;
    if (get_battery_counter_(counter, &value))
      return base::make_optional(value);
    return base::nullopt;
  }

  std::vector<android_internal::RailDescriptor> GetRailDescriptors() {
    if (!get_available_rails_)
      return std::vector<android_internal::RailDescriptor>();

    std::vector<android_internal::RailDescriptor> rail_descriptors(
        kMaxNumRails);
    size_t num_rails = rail_descriptors.size();
    get_available_rails_(&rail_descriptors[0], &num_rails);
    rail_descriptors.resize(num_rails);
    return rail_descriptors;
  }

  std::vector<android_internal::RailEnergyData> GetRailEnergyData() {
    if (!get_rail_energy_data_)
      return std::vector<android_internal::RailEnergyData>();

    std::vector<android_internal::RailEnergyData> energy_data(kMaxNumRails);
    size_t num_rails = energy_data.size();
    get_rail_energy_data_(&energy_data[0], &num_rails);
    energy_data.resize(num_rails);
    return energy_data;
  }
};

AndroidPowerDataSource::AndroidPowerDataSource(
    DataSourceConfig cfg,
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, kTypeId),
      task_runner_(task_runner),
      poll_rate_ms_(cfg.android_power_config().battery_poll_ms()),
      rails_collection_enabled_(
          cfg.android_power_config().collect_power_rails()),
      rail_descriptors_logged_(false),
      writer_(std::move(writer)),
      weak_factory_(this) {
  if (poll_rate_ms_ < kMinPollRateMs) {
    PERFETTO_ELOG("Battery poll interval of %" PRIu32
                  " ms is too low. Capping to %" PRIu32 " ms",
                  poll_rate_ms_, kMinPollRateMs);
    poll_rate_ms_ = kMinPollRateMs;
  }
  for (auto counter : cfg.android_power_config().battery_counters()) {
    auto hal_id = android_internal::BatteryCounter::kUnspecified;
    switch (counter) {
      case AndroidPowerConfig::BATTERY_COUNTER_UNSPECIFIED:
        break;
      case AndroidPowerConfig::BATTERY_COUNTER_CHARGE:
        hal_id = android_internal::BatteryCounter::kCharge;
        break;
      case AndroidPowerConfig::BATTERY_COUNTER_CAPACITY_PERCENT:
        hal_id = android_internal::BatteryCounter::kCapacityPercent;
        break;
      case AndroidPowerConfig::BATTERY_COUNTER_CURRENT:
        hal_id = android_internal::BatteryCounter::kCurrent;
        break;
      case AndroidPowerConfig::BATTERY_COUNTER_CURRENT_AVG:
        hal_id = android_internal::BatteryCounter::kCurrentAvg;
        break;
    }
    PERFETTO_CHECK(static_cast<size_t>(hal_id) < counters_enabled_.size());
    counters_enabled_.set(static_cast<size_t>(hal_id));
  }
}

AndroidPowerDataSource::~AndroidPowerDataSource() = default;

void AndroidPowerDataSource::Start() {
  lib_.reset(new DynamicLibLoader());
  Tick();
}

void AndroidPowerDataSource::Tick() {
  // Post next task.
  auto now_ms = base::GetWallTimeMs().count();
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this] {
        if (weak_this)
          weak_this->Tick();
      },
      poll_rate_ms_ - (now_ms % poll_rate_ms_));

  WriteBatteryCounters();
  WritePowerRailsData();
}

void AndroidPowerDataSource::WriteBatteryCounters() {
  if (counters_enabled_.none())
    return;

  auto packet = writer_->NewTracePacket();
  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
  auto* counters_proto = packet->set_battery();

  for (size_t i = 0; i < counters_enabled_.size(); i++) {
    if (!counters_enabled_.test(i))
      continue;
    auto counter = static_cast<android_internal::BatteryCounter>(i);
    auto value = lib_->GetCounter(counter);
    if (!value.has_value())
      continue;

    switch (counter) {
      case android_internal::BatteryCounter::kUnspecified:
        PERFETTO_DFATAL("Unspecified counter");
        break;

      case android_internal::BatteryCounter::kCharge:
        counters_proto->set_charge_counter_uah(*value);
        break;

      case android_internal::BatteryCounter::kCapacityPercent:
        counters_proto->set_capacity_percent(static_cast<float>(*value));
        break;

      case android_internal::BatteryCounter::kCurrent:
        counters_proto->set_current_ua(*value);
        break;

      case android_internal::BatteryCounter::kCurrentAvg:
        counters_proto->set_current_avg_ua(*value);
        break;
    }
  }
}

void AndroidPowerDataSource::WritePowerRailsData() {
  if (!rails_collection_enabled_)
    return;

  auto packet = writer_->NewTracePacket();
  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
  auto* rails_proto = packet->set_power_rails();

  if (!rail_descriptors_logged_) {
    // We only add the rail descriptors to the first package, to avoid logging
    // all rail names etc. on each one.
    rail_descriptors_logged_ = true;
    auto rail_descriptors = lib_->GetRailDescriptors();
    if (rail_descriptors.size() == 0) {
      // No rails to collect data for. Don't try again in the next iteration.
      rails_collection_enabled_ = false;
      return;
    }

    for (const auto& rail_descriptor : rail_descriptors) {
      auto* descriptor = rails_proto->add_rail_descriptor();
      descriptor->set_index(rail_descriptor.index);
      descriptor->set_rail_name(rail_descriptor.rail_name);
      descriptor->set_subsys_name(rail_descriptor.subsys_name);
      descriptor->set_sampling_rate(rail_descriptor.sampling_rate);
    }
  }

  for (const auto& energy_data : lib_->GetRailEnergyData()) {
    auto* data = rails_proto->add_energy_data();
    data->set_index(energy_data.index);
    data->set_timestamp_ms(energy_data.timestamp);
    data->set_energy(energy_data.energy);
  }
}

void AndroidPowerDataSource::Flush(FlushRequestID,
                                   std::function<void()> callback) {
  writer_->Flush(callback);
}

}  // namespace perfetto
