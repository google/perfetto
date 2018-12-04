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

#include <dlfcn.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/optional.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/android_internal/health_hal.h"

#include "perfetto/trace/power/battery_counters.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {
constexpr uint32_t kMinPollRateMs = 250;
}

// Dynamically loads / unloads the libperfetto_android_internal.so library which
// allows to proxy calls to android hwbinder in in-tree builds.
struct AndroidPowerDataSource::DynamicLibLoader {
  using ScopedDlHandle = base::ScopedResource<void*, dlclose, nullptr>;

  DynamicLibLoader() {
    static const char kLibName[] = "libperfetto_android_internal.so";
    handle_.reset(dlopen(kLibName, RTLD_NOW));
    if (!handle_) {
      PERFETTO_PLOG("dlopen(%s) failed", kLibName);
      return;
    }
    void* fn = dlsym(*handle_, "GetBatteryCounter");
    if (!fn) {
      PERFETTO_PLOG("dlsym(GetBatteryCounter) failed");
      return;
    }
    get_battery_counter_ = reinterpret_cast<decltype(get_battery_counter_)>(fn);
  }

  base::Optional<int64_t> GetCounter(android_internal::BatteryCounter counter) {
    if (!get_battery_counter_)
      return base::nullopt;
    int64_t value = 0;
    if (get_battery_counter_(counter, &value))
      return base::make_optional(value);
    return base::nullopt;
  }

  bool is_loaded() const { return !!handle_; }

 private:
  decltype(&android_internal::GetBatteryCounter) get_battery_counter_ = nullptr;
  ScopedDlHandle handle_;
};

AndroidPowerDataSource::AndroidPowerDataSource(
    DataSourceConfig cfg,
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, kTypeId),
      task_runner_(task_runner),
      poll_rate_ms_(cfg.android_power_config().battery_poll_ms()),
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
  if (!lib_->is_loaded())
    return;
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
        PERFETTO_DCHECK(false);
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

void AndroidPowerDataSource::Flush(FlushRequestID,
                                   std::function<void()> callback) {
  writer_->Flush(callback);
}

}  // namespace perfetto
