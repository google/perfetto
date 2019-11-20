/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_PROFILING_PERF_PERF_PRODUCER_H_
#define SRC_PROFILING_PERF_PERF_PRODUCER_H_

#include <map>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "src/profiling/perf/event_config.h"
#include "src/profiling/perf/event_reader.h"

namespace perfetto {
namespace profiling {

// TODO(b/144281346): work in progress. Do not use.
class PerfProducer : public Producer {
 public:
  PerfProducer(base::TaskRunner* task_runner);
  ~PerfProducer() override = default;

  PerfProducer(const PerfProducer&) = delete;
  PerfProducer& operator=(const PerfProducer&) = delete;
  PerfProducer(PerfProducer&&) = delete;
  PerfProducer& operator=(PerfProducer&&) = delete;

  void ConnectWithRetries(const char* socket_name);

  // Producer Impl:
  void OnConnect() override;
  void OnDisconnect() override;
  void OnTracingSetup() override {}
  void SetupDataSource(DataSourceInstanceID, const DataSourceConfig&) override;
  void StartDataSource(DataSourceInstanceID, const DataSourceConfig&) override;
  void StopDataSource(DataSourceInstanceID) override;
  void Flush(FlushRequestID,
             const DataSourceInstanceID* data_source_ids,
             size_t num_data_sources) override;
  void ClearIncrementalState(const DataSourceInstanceID* /*data_source_ids*/,
                             size_t /*num_data_sources*/) override {}

 private:
  // State of the connection to tracing service (traced).
  enum State {
    kNotStarted = 0,
    kNotConnected,
    kConnecting,
    kConnected,
  };

  // TODO(rsavitski): proc-fds need to live elsewhere, as they can be shared
  // across data sources. We might also have arbitrarily many tasks handled
  // by one data source (if scoping events to a cpu).
  struct DataSource {
    DataSource(EventReader _event_reader,
               base::ScopedFile _maps_fd,
               base::ScopedFile _mem_fd)
        : event_reader(std::move(_event_reader)),
          maps_fd(std::move(_maps_fd)),
          mem_fd(std::move(_mem_fd)) {}

    EventReader event_reader;

    // note: currently populated, but unused.
    base::ScopedFile maps_fd;
    base::ScopedFile mem_fd;
  };

  void ConnectService();
  void Restart();
  void ResetConnectionBackoff();
  void IncreaseConnectionBackoff();

  base::TaskRunner* const task_runner_;
  State state_ = kNotStarted;
  const char* producer_socket_name_ = nullptr;
  uint32_t connection_backoff_ms_ = 0;

  std::unique_ptr<TracingService::ProducerEndpoint> endpoint_;
  std::map<DataSourceInstanceID, DataSource> data_sources_;

  base::WeakPtrFactory<PerfProducer> weak_factory_;  // keep last
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_PERF_PRODUCER_H_
