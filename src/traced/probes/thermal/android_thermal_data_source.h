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

#ifndef SRC_TRACED_PROBES_THERMAL_ANDROID_THERMAL_DATA_SOURCE_H_
#define SRC_TRACED_PROBES_THERMAL_ANDROID_THERMAL_DATA_SOURCE_H_

#include <vector>

#include "perfetto/base/proc_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/traced/probes/probes_data_source.h"

namespace perfetto {

class TraceWriter;
namespace base {
class TaskRunner;
}

class AndroidThermalDataSource : public ProbesDataSource {
 public:
  static const ProbesDataSource::Descriptor descriptor;

  AndroidThermalDataSource(DataSourceConfig,
                           base::TaskRunner*,
                           TracingSessionID,
                           std::unique_ptr<TraceWriter> writer);

  ~AndroidThermalDataSource() override;

  base::WeakPtr<AndroidThermalDataSource> GetWeakPtr() const;

  // ProbesDataSource implementation.
  void Start() override;
  void Flush(FlushRequestID, std::function<void()> callback) override;

 private:
  using ThermalZoneNameAndFd = std::pair<std::string, base::ScopedFstream>;

  void Tick();
  void WriteTemperatureCountersData();

  base::PlatformProcessId pid_;
  std::vector<ThermalZoneNameAndFd> enabled_sensors_;
  base::TaskRunner* const task_runner_;
  uint32_t poll_interval_ms_ = 0;
  std::unique_ptr<TraceWriter> writer_;
  base::WeakPtrFactory<AndroidThermalDataSource> weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_THERMAL_ANDROID_THERMAL_DATA_SOURCE_H_
