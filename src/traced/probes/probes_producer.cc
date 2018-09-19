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

#include "src/traced/probes/probes_producer.h"

#include <stdio.h>
#include <sys/stat.h>

#include <algorithm>
#include <queue>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/ftrace_config.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"
#include "src/traced/probes/filesystem/inode_file_data_source.h"
#include "src/traced/probes/ftrace/ftrace_data_source.h"
#include "src/traced/probes/probes_data_source.h"

#include "perfetto/trace/filesystem/inode_file_map.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_stats.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

constexpr uint32_t kInitialConnectionBackoffMs = 100;
constexpr uint32_t kMaxConnectionBackoffMs = 30 * 1000;
constexpr char kFtraceSourceName[] = "linux.ftrace";
constexpr char kProcessStatsSourceName[] = "linux.process_stats";
constexpr char kInodeMapSourceName[] = "linux.inode_file_map";
constexpr char kSysStatsSourceName[] = "linux.sys_stats";

}  // namespace.

// State transition diagram:
//                    +----------------------------+
//                    v                            +
// NotStarted -> NotConnected -> Connecting -> Connected
//                    ^              +
//                    +--------------+
//

ProbesProducer::ProbesProducer() : weak_factory_(this) {}
ProbesProducer::~ProbesProducer() {
  // The ftrace data sources must be deleted before the ftrace controller.
  data_sources_.clear();
  ftrace_.reset();
}

void ProbesProducer::OnConnect() {
  PERFETTO_DCHECK(state_ == kConnecting);
  state_ = kConnected;
  ResetConnectionBackoff();
  PERFETTO_LOG("Connected to the service");

  {
    DataSourceDescriptor desc;
    desc.set_name(kFtraceSourceName);
    endpoint_->RegisterDataSource(desc);
  }

  {
    DataSourceDescriptor desc;
    desc.set_name(kProcessStatsSourceName);
    endpoint_->RegisterDataSource(desc);
  }

  {
    DataSourceDescriptor desc;
    desc.set_name(kInodeMapSourceName);
    endpoint_->RegisterDataSource(desc);
  }

  {
    DataSourceDescriptor desc;
    desc.set_name(kSysStatsSourceName);
    endpoint_->RegisterDataSource(desc);
  }
}

void ProbesProducer::OnDisconnect() {
  PERFETTO_DCHECK(state_ == kConnected || state_ == kConnecting);
  PERFETTO_LOG("Disconnected from tracing service");
  if (state_ == kConnected)
    return task_runner_->PostTask([this] { this->Restart(); });

  state_ = kNotConnected;
  IncreaseConnectionBackoff();
  task_runner_->PostDelayedTask([this] { this->Connect(); },
                                connection_backoff_ms_);
}

void ProbesProducer::Restart() {
  // We lost the connection with the tracing service. At this point we need
  // to reset all the data sources. Trying to handle that manually is going to
  // be error prone. What we do here is simply desroying the instance and
  // recreating it again.
  // TODO(hjd): Add e2e test for this.

  base::TaskRunner* task_runner = task_runner_;
  const char* socket_name = socket_name_;

  // Invoke destructor and then the constructor again.
  this->~ProbesProducer();
  new (this) ProbesProducer();

  ConnectWithRetries(socket_name, task_runner);
}

void ProbesProducer::CreateDataSourceInstance(DataSourceInstanceID instance_id,
                                              const DataSourceConfig& config) {
  PERFETTO_DCHECK(data_sources_.count(instance_id) == 0);
  TracingSessionID session_id = config.tracing_session_id();
  PERFETTO_CHECK(session_id > 0);

  std::unique_ptr<ProbesDataSource> data_source;
  if (config.name() == kFtraceSourceName) {
    data_source = CreateFtraceDataSource(session_id, instance_id, config);
  } else if (config.name() == kInodeMapSourceName) {
    data_source = CreateInodeFileDataSource(session_id, instance_id, config);
  } else if (config.name() == kProcessStatsSourceName) {
    data_source = CreateProcessStatsDataSource(session_id, instance_id, config);
  } else if (config.name() == kSysStatsSourceName) {
    data_source = CreateSysStatsDataSource(session_id, instance_id, config);
  }

  if (!data_source) {
    PERFETTO_ELOG("Failed to create data source '%s'", config.name().c_str());
    return;
  }

  session_data_sources_.emplace(session_id, data_source.get());
  data_sources_[instance_id] = std::move(data_source);

  if (config.trace_duration_ms() != 0) {
    uint32_t timeout = 5000 + 2 * config.trace_duration_ms();
    watchdogs_.emplace(
        instance_id, base::Watchdog::GetInstance()->CreateFatalTimer(timeout));
  }
}

std::unique_ptr<ProbesDataSource> ProbesProducer::CreateFtraceDataSource(
    TracingSessionID session_id,
    DataSourceInstanceID id,
    const DataSourceConfig& config) {
  // Don't retry if FtraceController::Create() failed once.
  // This can legitimately happen on user builds where we cannot access the
  // debug paths, e.g., because of SELinux rules.
  if (ftrace_creation_failed_)
    return nullptr;

  // Lazily create on the first instance.
  if (!ftrace_) {
    ftrace_ = FtraceController::Create(task_runner_, this);

    if (!ftrace_) {
      PERFETTO_ELOG("Failed to create FtraceController");
      ftrace_creation_failed_ = true;
      return nullptr;
    }

    ftrace_->DisableAllEvents();
    ftrace_->ClearTrace();
  }

  PERFETTO_LOG("Ftrace start (id=%" PRIu64 ", target_buf=%" PRIu32 ")", id,
               config.target_buffer());
  const BufferID buffer_id = static_cast<BufferID>(config.target_buffer());
  std::unique_ptr<FtraceDataSource> data_source(new FtraceDataSource(
      ftrace_->GetWeakPtr(), session_id, config.ftrace_config(),
      endpoint_->CreateTraceWriter(buffer_id)));
  if (!ftrace_->AddDataSource(data_source.get())) {
    PERFETTO_ELOG(
        "Failed to start tracing (too many concurrent sessions or ftrace is "
        "already in use)");
    return nullptr;
  }
  return std::move(data_source);
}

std::unique_ptr<ProbesDataSource> ProbesProducer::CreateInodeFileDataSource(
    TracingSessionID session_id,
    DataSourceInstanceID id,
    DataSourceConfig source_config) {
  PERFETTO_LOG("Inode file map start (id=%" PRIu64 ", target_buf=%" PRIu32 ")",
               id, source_config.target_buffer());
  auto buffer_id = static_cast<BufferID>(source_config.target_buffer());
  if (system_inodes_.empty())
    CreateStaticDeviceToInodeMap("/system", &system_inodes_);
  return std::unique_ptr<InodeFileDataSource>(new InodeFileDataSource(
      std::move(source_config), task_runner_, session_id, &system_inodes_,
      &cache_, endpoint_->CreateTraceWriter(buffer_id)));
}

std::unique_ptr<ProbesDataSource> ProbesProducer::CreateProcessStatsDataSource(
    TracingSessionID session_id,
    DataSourceInstanceID id,
    const DataSourceConfig& config) {
  base::ignore_result(id);
  auto buffer_id = static_cast<BufferID>(config.target_buffer());
  auto data_source =
      std::unique_ptr<ProcessStatsDataSource>(new ProcessStatsDataSource(
          session_id, endpoint_->CreateTraceWriter(buffer_id), config));
  if (config.process_stats_config().scan_all_processes_on_start()) {
    data_source->WriteAllProcesses();
  }
  return std::move(data_source);
}

std::unique_ptr<SysStatsDataSource> ProbesProducer::CreateSysStatsDataSource(
    TracingSessionID session_id,
    DataSourceInstanceID id,
    const DataSourceConfig& config) {
  base::ignore_result(id);
  auto buffer_id = static_cast<BufferID>(config.target_buffer());
  auto data_source = std::unique_ptr<SysStatsDataSource>(
      new SysStatsDataSource(task_runner_, session_id,
                             endpoint_->CreateTraceWriter(buffer_id), config));
  return data_source;
}

void ProbesProducer::TearDownDataSourceInstance(DataSourceInstanceID id) {
  PERFETTO_LOG("Producer stop (id=%" PRIu64 ")", id);
  auto it = data_sources_.find(id);
  if (it == data_sources_.end()) {
    PERFETTO_ELOG("Cannot stop data source id=%" PRIu64 ", not found", id);
    return;
  }
  ProbesDataSource* data_source = it->second.get();
  TracingSessionID session_id = data_source->tracing_session_id;
  auto range = session_data_sources_.equal_range(session_id);
  for (auto kv = range.first; kv != range.second; kv++) {
    if (kv->second != data_source)
      continue;
    session_data_sources_.erase(kv);
    break;
  }
  data_sources_.erase(it);
  watchdogs_.erase(id);
}

void ProbesProducer::OnTracingSetup() {}

void ProbesProducer::Flush(FlushRequestID flush_request_id,
                           const DataSourceInstanceID* data_source_ids,
                           size_t num_data_sources) {
  for (size_t i = 0; i < num_data_sources; i++) {
    auto it = data_sources_.find(data_source_ids[i]);
    if (it == data_sources_.end())
      continue;
    it->second->Flush();
  }
  endpoint_->NotifyFlushComplete(flush_request_id);
}

// This function is called by the FtraceController in batches, whenever it has
// read one or more pages from one or more cpus and written that into the
// userspace tracing buffer. If more than one ftrace data sources are active,
// this call typically happens after writing for all session has been handled.
void ProbesProducer::OnFtraceDataWrittenIntoDataSourceBuffers() {
  TracingSessionID last_session_id = 0;
  FtraceMetadata* metadata = nullptr;
  InodeFileDataSource* inode_data_source = nullptr;
  ProcessStatsDataSource* ps_data_source = nullptr;

  // unordered_multimap guarantees that entries with the same key are contiguous
  // in the iteration.
  for (auto it = session_data_sources_.begin(); /* check below*/; it++) {
    // If this is the last iteration or this is the session id has changed,
    // dispatch the metadata update to the linked data sources, if any.
    if (it == session_data_sources_.end() || it->first != last_session_id) {
      bool has_inodes = metadata && !metadata->inode_and_device.empty();
      bool has_pids = metadata && !metadata->pids.empty();
      if (has_inodes && inode_data_source)
        inode_data_source->OnInodes(metadata->inode_and_device);
      if (has_pids && ps_data_source)
        ps_data_source->OnPids(metadata->pids);
      if (metadata)
        metadata->Clear();
      metadata = nullptr;
      inode_data_source = nullptr;
      ps_data_source = nullptr;
      if (it == session_data_sources_.end())
        break;
      last_session_id = it->first;
    }
    ProbesDataSource* ds = it->second;
    switch (ds->type_id) {
      case FtraceDataSource::kTypeId:
        metadata = static_cast<FtraceDataSource*>(ds)->mutable_metadata();
        break;
      case InodeFileDataSource::kTypeId:
        inode_data_source = static_cast<InodeFileDataSource*>(ds);
        break;
      case ProcessStatsDataSource::kTypeId:
        ps_data_source = static_cast<ProcessStatsDataSource*>(ds);
        break;
      case SysStatsDataSource::kTypeId:
        break;
      default:
        PERFETTO_DCHECK(false);
    }  // switch (type_id)
  }    // for (session_data_sources_)
}

void ProbesProducer::ConnectWithRetries(const char* socket_name,
                                        base::TaskRunner* task_runner) {
  PERFETTO_DCHECK(state_ == kNotStarted);
  state_ = kNotConnected;

  ResetConnectionBackoff();
  socket_name_ = socket_name;
  task_runner_ = task_runner;
  Connect();
}

void ProbesProducer::Connect() {
  PERFETTO_DCHECK(state_ == kNotConnected);
  state_ = kConnecting;
  endpoint_ = ProducerIPCClient::Connect(
      socket_name_, this, "perfetto.traced_probes", task_runner_);
}

void ProbesProducer::IncreaseConnectionBackoff() {
  connection_backoff_ms_ *= 2;
  if (connection_backoff_ms_ > kMaxConnectionBackoffMs)
    connection_backoff_ms_ = kMaxConnectionBackoffMs;
}

void ProbesProducer::ResetConnectionBackoff() {
  connection_backoff_ms_ = kInitialConnectionBackoffMs;
}

}  // namespace perfetto
