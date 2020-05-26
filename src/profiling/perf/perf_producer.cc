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

#include "src/profiling/perf/perf_producer.h"

#include <unistd.h>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/ext/tracing/ipc/producer_ipc_client.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "protos/perfetto/config/profiling/perf_event_config.pbzero.h"
#include "src/profiling/perf/event_reader.h"

namespace perfetto {
namespace profiling {
namespace {

constexpr uint32_t kInitialConnectionBackoffMs = 100;
constexpr uint32_t kMaxConnectionBackoffMs = 30 * 1000;

constexpr char kProducerName[] = "perfetto.traced_perf";
constexpr char kDataSourceName[] = "linux.perf";

}  // namespace

PerfProducer::PerfProducer(base::TaskRunner* task_runner)
    : task_runner_(task_runner), weak_factory_(this) {}

// TODO(rsavitski): configure at setup + enable at start, or do everything on
// start? Also, do we try to work around the old(?) cpu hotplug bugs as
// simpleperf does?
void PerfProducer::SetupDataSource(DataSourceInstanceID,
                                   const DataSourceConfig&) {}

void PerfProducer::StartDataSource(DataSourceInstanceID instance_id,
                                   const DataSourceConfig& config) {
  PERFETTO_LOG("StartDataSource(id=%" PRIu64 ", name=%s)", instance_id,
               config.name().c_str());

  if (config.name() != kDataSourceName)
    return;

  base::Optional<EventConfig> event_config = EventConfig::Create(config);
  if (!event_config.has_value()) {
    PERFETTO_LOG("PerfEventConfig rejected.");
    return;
  }

  std::string maps_path = std::string("/proc/") +
                          std::to_string(event_config->target_tid()) +
                          std::string("/maps");
  auto maps_fd = base::OpenFile(maps_path, O_RDONLY);
  if (!maps_fd)
    PERFETTO_PLOG("failed /proc/pid/maps open (proceeding)");

  std::string mem_path = std::string("/proc/") +
                         std::to_string(event_config->target_tid()) +
                         std::string("/mem");
  auto mem_fd = base::OpenFile(mem_path, O_RDONLY);
  if (!mem_fd)
    PERFETTO_PLOG("failed /proc/pid/mem open (proceeding)");

  base::Optional<EventReader> event_reader =
      EventReader::ConfigureEvents(event_config.value());
  if (!event_reader.has_value()) {
    PERFETTO_LOG("Failed to set up perf events.");
    return;
  }

  // Build the DataSource instance.
  auto it_inserted = data_sources_.emplace(
      std::piecewise_construct, std::forward_as_tuple(instance_id),
      std::forward_as_tuple(std::move(event_reader.value()), std::move(maps_fd),
                            std::move(mem_fd)));

  PERFETTO_DCHECK(it_inserted.second);
}

void PerfProducer::StopDataSource(DataSourceInstanceID instance_id) {
  PERFETTO_LOG("StopDataSource(id=%" PRIu64 ")", instance_id);

  data_sources_.erase(instance_id);
}

void PerfProducer::Flush(FlushRequestID,
                         const DataSourceInstanceID* data_source_ids,
                         size_t num_data_sources) {
  for (size_t i = 0; i < num_data_sources; i++) {
    PERFETTO_LOG("Flush(id=%" PRIu64 ")", data_source_ids[i]);

    auto ds_it = data_sources_.find(data_source_ids[i]);
    if (ds_it != data_sources_.end()) {
      auto& ds = ds_it->second;

      // For now, parse whatever's been accumulated in the ring buffer.
      ds.event_reader.ParseNextSampleBatch();
    }
  }
}

void PerfProducer::ConnectWithRetries(const char* socket_name) {
  PERFETTO_DCHECK(state_ == kNotStarted);
  state_ = kNotConnected;

  ResetConnectionBackoff();
  producer_socket_name_ = socket_name;
  ConnectService();
}

void PerfProducer::ConnectService() {
  PERFETTO_DCHECK(state_ == kNotConnected);
  state_ = kConnecting;
  endpoint_ = ProducerIPCClient::Connect(producer_socket_name_, this,
                                         kProducerName, task_runner_);
}

void PerfProducer::IncreaseConnectionBackoff() {
  connection_backoff_ms_ *= 2;
  if (connection_backoff_ms_ > kMaxConnectionBackoffMs)
    connection_backoff_ms_ = kMaxConnectionBackoffMs;
}

void PerfProducer::ResetConnectionBackoff() {
  connection_backoff_ms_ = kInitialConnectionBackoffMs;
}

void PerfProducer::OnConnect() {
  PERFETTO_DCHECK(state_ == kConnecting);
  state_ = kConnected;
  ResetConnectionBackoff();
  PERFETTO_LOG("Connected to the service");

  DataSourceDescriptor desc;
  desc.set_name(kDataSourceName);
  endpoint_->RegisterDataSource(desc);
}

void PerfProducer::OnDisconnect() {
  PERFETTO_DCHECK(state_ == kConnected || state_ == kConnecting);
  PERFETTO_LOG("Disconnected from tracing service");

  auto weak_producer = weak_factory_.GetWeakPtr();
  if (state_ == kConnected)
    return task_runner_->PostTask([weak_producer] {
      if (weak_producer)
        weak_producer->Restart();
    });

  state_ = kNotConnected;
  IncreaseConnectionBackoff();
  task_runner_->PostDelayedTask(
      [weak_producer] {
        if (weak_producer)
          weak_producer->ConnectService();
      },
      connection_backoff_ms_);
}

void PerfProducer::Restart() {
  // We lost the connection with the tracing service. At this point we need
  // to reset all the data sources. Trying to handle that manually is going to
  // be error prone. What we do here is simply destroy the instance and
  // recreate it again.
  base::TaskRunner* task_runner = task_runner_;
  const char* socket_name = producer_socket_name_;

  // Invoke destructor and then the constructor again.
  this->~PerfProducer();
  new (this) PerfProducer(task_runner);

  ConnectWithRetries(socket_name);
}

}  // namespace profiling
}  // namespace perfetto
