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

#ifndef SRC_TRACED_PROBES_POWER_ANDROID_POWER_STATS_DATA_SOURCE_H_
#define SRC_TRACED_PROBES_POWER_ANDROID_POWER_STATS_DATA_SOURCE_H_

#include <bitset>
#include <memory>

#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/traced/probes/probes_data_source.h"

namespace perfetto {

class TraceWriter;
namespace base {
class TaskRunner;
}

class AndroidPowerStatsDataSource : public ProbesDataSource {
 public:
  static const ProbesDataSource::Descriptor descriptor;

  AndroidPowerStatsDataSource(DataSourceConfig,
                              base::TaskRunner*,
                              TracingSessionID,
                              std::unique_ptr<TraceWriter> writer);

  ~AndroidPowerStatsDataSource() override;

  base::WeakPtr<AndroidPowerStatsDataSource> GetWeakPtr() const;

  // ProbesDataSource implementation.
  void Start() override;
  void Flush(FlushRequestID, std::function<void()> callback) override;

 private:
  struct DynamicLibLoader;

  void Tick();
  void WriteEnergyEstimationBreakdown();

  base::TaskRunner* const task_runner_;
  std::unique_ptr<TraceWriter> writer_;
  std::unique_ptr<DynamicLibLoader> lib_;
  base::WeakPtrFactory<AndroidPowerStatsDataSource>
      weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_POWER_ANDROID_POWER_STATS_DATA_SOURCE_H_
