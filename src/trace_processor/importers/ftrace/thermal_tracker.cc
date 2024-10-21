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

#include "src/trace_processor/importers/ftrace/thermal_tracker.h"
#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/trace/ftrace/thermal.pbzero.h"
#include "protos/perfetto/trace/ftrace/thermal_exynos.pbzero.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"

namespace perfetto {
namespace trace_processor {

constexpr const char* kTemperatureSuffix = "Temperature";
constexpr const char* kCoolingDeviceSuffix = "Cooling Device";

ThermalTracker::ThermalTracker(TraceProcessorContext* context)
    : context_(context) {
  const std::array<const char*, kAcpmThermalZones> acpm_thermal_zones = {
      "BIG", "MID", "LITTLE", "GPU", "ISP", "TPU", "AUR",
  };
  for (size_t i = 0; i < kAcpmThermalZones; i++) {
    base::StackString<32> temperature_counter("%s %s", acpm_thermal_zones[i],
                                              kTemperatureSuffix);
    acpm_temperature_counters_[i] =
        context_->storage->InternString(temperature_counter.string_view());
    base::StackString<32> cooling_device_counter(
        "Tj-%s %s", acpm_thermal_zones[i], kCoolingDeviceSuffix);
    acpm_cooling_device_counters_[i] =
        context_->storage->InternString(cooling_device_counter.string_view());
  }
}

void ThermalTracker::ParseThermalTemperature(int64_t timestamp,
                                             protozero::ConstBytes blob) {
  protos::pbzero::ThermalTemperatureFtraceEvent::Decoder event(blob);
  base::StringView tz = event.thermal_zone();
  base::StackString<255> counter_name("%.*s %s", static_cast<int>(tz.size()),
                                      tz.data(), kTemperatureSuffix);
  StringId counter_id =
      context_->storage->InternString(counter_name.string_view());
  PushCounter(timestamp, counter_id, static_cast<double>(event.temp()));
}

void ThermalTracker::ParseCdevUpdate(int64_t timestamp,
                                     protozero::ConstBytes blob) {
  protos::pbzero::CdevUpdateFtraceEvent::Decoder event(blob);
  base::StringView cdev = event.type();
  base::StackString<255> counter_name("%.*s %s", static_cast<int>(cdev.size()),
                                      cdev.data(), kCoolingDeviceSuffix);
  StringId counter_id =
      context_->storage->InternString(counter_name.string_view());
  PushCounter(timestamp, counter_id, static_cast<double>(event.target()));
}

void ThermalTracker::ParseThermalExynosAcpmBulk(protozero::ConstBytes blob) {
  protos::pbzero::ThermalExynosAcpmBulkFtraceEvent::Decoder event(blob);
  auto tz_id = static_cast<size_t>(event.tz_id());
  if (tz_id >= kAcpmThermalZones) {
    context_->storage->IncrementStats(
        stats::ftrace_thermal_exynos_acpm_unknown_tz_id);
    return;
  }
  auto timestamp = static_cast<int64_t>(event.timestamp());
  // Record acpm tz's temperature.
  PushCounter(timestamp, acpm_temperature_counters_[tz_id],
              static_cast<double>(event.current_temp()));
  // Record cdev target of acpm tz's cooling device.
  PushCounter(timestamp, acpm_cooling_device_counters_[tz_id],
              static_cast<double>(event.cdev_state()));
}

void ThermalTracker::ParseThermalExynosAcpmHighOverhead(
    int64_t timestamp,
    protozero::ConstBytes blob) {
  protos::pbzero::ThermalExynosAcpmHighOverheadFtraceEvent::Decoder event(blob);
  auto tz_id = static_cast<size_t>(event.tz_id());
  if (tz_id >= kAcpmThermalZones) {
    context_->storage->IncrementStats(
        stats::ftrace_thermal_exynos_acpm_unknown_tz_id);
    return;
  }
  // Record acpm tz's temperature.
  PushCounter(timestamp, acpm_temperature_counters_[tz_id],
              static_cast<double>(event.current_temp()));
  // Record cdev target of acpm tz's cooling device.
  PushCounter(timestamp, acpm_cooling_device_counters_[tz_id],
              static_cast<double>(event.cdev_state()));
}

void ThermalTracker::PushCounter(int64_t timestamp,
                                 StringId counter_id,
                                 double value) {
  TrackId track = context_->track_tracker->LegacyInternGlobalCounterTrack(
      TrackTracker::Group::kThermals, counter_id);
  context_->event_tracker->PushCounter(timestamp, value, track);
}

}  // namespace trace_processor
}  // namespace perfetto
