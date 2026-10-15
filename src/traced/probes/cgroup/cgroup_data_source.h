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

#ifndef SRC_TRACED_PROBES_CGROUP_CGROUP_DATA_SOURCE_H_
#define SRC_TRACED_PROBES_CGROUP_CGROUP_DATA_SOURCE_H_

#include <functional>
#include <memory>
#include <optional>

#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/traced/probes/cgroup/memcg_reader.h"
#include "src/traced/probes/probes_data_source.h"

namespace perfetto {

class TraceWriter;
namespace base {
class TaskRunner;
}

// Data source name: "linux.cgroup".
// Config: CgroupConfig in cgroup_config.proto.
class CgroupDataSource : public ProbesDataSource {
 public:
  static const ProbesDataSource::Descriptor descriptor;

  CgroupDataSource(const DataSourceConfig& ds_config,
                   base::TaskRunner* task_runner,
                   TracingSessionID session_id,
                   std::unique_ptr<TraceWriter> writer);
  ~CgroupDataSource() override;

  // ProbesDataSource implementation.
  void Start() override;
  void Flush(FlushRequestID, std::function<void()> callback) override;

 private:
  // Reads the cgroups, and reposts itself if the config is periodic.
  void Tick();

  base::TaskRunner* const task_runner_;
  std::unique_ptr<TraceWriter> writer_;
  // Unset if the config doesn't have a valid memcg_config. The data source is
  // still created, so that it acks start and stop, but records nothing.
  std::optional<MemcgReader> memcg_reader_;

  base::WeakPtrFactory<CgroupDataSource> weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_CGROUP_CGROUP_DATA_SOURCE_H_
