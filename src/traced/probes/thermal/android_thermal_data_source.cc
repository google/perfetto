/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/traced/probes/thermal/android_thermal_data_source.h"

#include <unordered_map>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/proc_utils.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/thermal/android_thermal_config.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/thermal.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {
constexpr uint32_t kDefaultPollIntervalMs = 1000;
constexpr uint32_t kPlaceholderCpuId = 0;
constexpr char kThermalSysfsRootDir[] = "/sys/class/thermal/";
constexpr char kThermalZoneNameFile[] = "/type";
constexpr char kThermalZoneTempFile[] = "/temp";

// Return map of sensor name to temp paths.
std::unordered_map<std::string, std::string> ScanThermalZones() {
  std::unordered_map<std::string, std::string> map;
  base::ScopedDir thermal_dir(opendir(kThermalSysfsRootDir));
  if (!thermal_dir) {
    PERFETTO_PLOG("Failed to opendir(%s)", kThermalSysfsRootDir);
    return map;
  }

  // Scan all thermal zones.
  while (struct dirent* dir_ent = readdir(*thermal_dir)) {
    std::string dir_name = dir_ent->d_name;
    if (!base::StartsWith(dir_name, "thermal")) {
      continue;
    }
    std::string thermal_name_path =
        kThermalSysfsRootDir + dir_name + kThermalZoneNameFile;
    std::string sensor_name;
    base::ReadFile(thermal_name_path, &sensor_name);
    if (sensor_name.empty()) {
      PERFETTO_ELOG("Could not read %s", thermal_name_path.c_str());
      continue;
    }
    // Remove trailing newline.
    sensor_name.pop_back();
    map[sensor_name] = kThermalSysfsRootDir + dir_name + kThermalZoneTempFile;
    PERFETTO_ILOG("Found thermal sensor %s", sensor_name.c_str());
  }

  return map;
}

}  // namespace

// static
const ProbesDataSource::Descriptor AndroidThermalDataSource::descriptor = {
    /*name*/ "android.thermal",
    /*flags*/ Descriptor::kFlagsNone,
};

AndroidThermalDataSource::AndroidThermalDataSource(
    DataSourceConfig cfg,
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {
  using protos::pbzero::AndroidThermalConfig;
  AndroidThermalConfig::Decoder tcfg(cfg.android_thermal_config_raw());
  poll_interval_ms_ = tcfg.poll_ms();
  pid_ = base::GetProcessId();

  // Scan thermal zones to determine available sensors by name.
  auto thermal_zone_map = ScanThermalZones();

  if (poll_interval_ms_ == 0)
    poll_interval_ms_ = kDefaultPollIntervalMs;

  // Determine the thermal zone that each requested sensor name maps to.
  for (auto sensor = tcfg.sensors(); sensor; ++sensor) {
    std::string sensor_name = sensor->as_std_string();
    if (thermal_zone_map.count(sensor_name) != 0) {
      enabled_sensors_.push_back(ThermalZoneNameAndFd(
          sensor_name, fopen(thermal_zone_map[sensor_name].c_str(), "r")));
      if (!enabled_sensors_.back().second) {
        PERFETTO_ELOG("Failed to open %s for %s",
                      thermal_zone_map[sensor_name].c_str(),
                      sensor_name.c_str());
        enabled_sensors_.pop_back();
      }
    } else {
      PERFETTO_ELOG("sensor(%s) not found", sensor_name.c_str());
    }
  }

  // Explicit sysfs nodes requested.
  for (auto sensor = tcfg.sensors_sysfs(); sensor; ++sensor) {
    std::string sensor_path = sensor->as_std_string();
    enabled_sensors_.push_back(
        ThermalZoneNameAndFd(sensor_path, fopen(sensor_path.c_str(), "r")));
    if (!enabled_sensors_.back().second) {
      PERFETTO_ELOG("Failed to open %s", sensor_path.c_str());
      enabled_sensors_.pop_back();
    }
  }
}

AndroidThermalDataSource::~AndroidThermalDataSource() = default;

void AndroidThermalDataSource::Start() {
  Tick();
}

void AndroidThermalDataSource::Tick() {
  // Post next task.
  auto now_ms = base::GetWallTimeMs().count();
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this] {
        if (weak_this)
          weak_this->Tick();
      },
      poll_interval_ms_ - static_cast<uint32_t>(now_ms % poll_interval_ms_));

  WriteTemperatureCountersData();
}

void AndroidThermalDataSource::WriteTemperatureCountersData() {
  auto packet = writer_->NewTracePacket();
  auto timestamp = static_cast<uint64_t>(base::GetBootTimeNs().count());
  packet->set_timestamp(timestamp);
  auto* bundle = packet->set_ftrace_events();
  bundle->set_cpu(kPlaceholderCpuId);
  for (const auto& sensor : enabled_sensors_) {
    std::string temp_str;
    std::rewind(*sensor.second);
    base::ReadFileStream(*sensor.second, &temp_str);
    temp_str.pop_back();
    auto temp = base::StringToUInt32(temp_str);
    if (!temp.has_value()) {
      PERFETTO_ELOG("Failed to read temperature for %s. Read '%s'",
                    sensor.first.c_str(), temp_str.c_str());
      continue;
    }
    auto* event = bundle->add_event();
    event->set_timestamp(timestamp);
    event->set_pid(static_cast<int32_t>(pid_));
    auto* thermal_temperature = event->set_thermal_temperature();
    thermal_temperature->set_thermal_zone(sensor.first);
    thermal_temperature->set_temp(temp.value());
    // This is normally the thermal zone number. This data source supports
    // reading arbitrary sysfs nodes for temperatures, even those without
    // a thermal zone number. Set this to -1 to signify that this event isn't
    // a "true" ftrace thermal_temperature event.
    thermal_temperature->set_id(-1);
  }
}

void AndroidThermalDataSource::Flush(FlushRequestID,
                                     std::function<void()> callback) {
  writer_->Flush(callback);
}

}  // namespace perfetto
