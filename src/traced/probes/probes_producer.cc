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
#include "perfetto/base/weak_ptr.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/ftrace_config.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"
#include "src/traced/probes/filesystem/inode_file_data_source.h"

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

}  // namespace.

// State transition diagram:
//                    +----------------------------+
//                    v                            +
// NotStarted -> NotConnected -> Connecting -> Connected
//                    ^              +
//                    +--------------+
//

ProbesProducer::ProbesProducer() {}
ProbesProducer::~ProbesProducer() = default;

void ProbesProducer::OnConnect() {
  PERFETTO_DCHECK(state_ == kConnecting);
  state_ = kConnected;
  ResetConnectionBackoff();
  PERFETTO_LOG("Connected to the service");

  DataSourceDescriptor ftrace_descriptor;
  ftrace_descriptor.set_name(kFtraceSourceName);
  endpoint_->RegisterDataSource(ftrace_descriptor);

  DataSourceDescriptor process_stats_descriptor;
  process_stats_descriptor.set_name(kProcessStatsSourceName);
  endpoint_->RegisterDataSource(process_stats_descriptor);

  DataSourceDescriptor inode_map_descriptor;
  inode_map_descriptor.set_name(kInodeMapSourceName);
  endpoint_->RegisterDataSource(inode_map_descriptor);
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
  // TODO(hjd): This a hack since we don't actually know the session id. For
  // now we'll assume anything wit hthe same target buffer is in the same
  // session.
  TracingSessionID session_id = config.target_buffer();

  if (config.name() == kFtraceSourceName) {
    if (!CreateFtraceDataSourceInstance(session_id, instance_id, config))
      failed_sources_.insert(instance_id);
  } else if (config.name() == kInodeMapSourceName) {
    CreateInodeFileDataSourceInstance(session_id, instance_id, config);
  } else if (config.name() == kProcessStatsSourceName) {
    CreateProcessStatsDataSourceInstance(session_id, instance_id, config);
  } else {
    PERFETTO_ELOG("Data source name: %s not recognised.",
                  config.name().c_str());
    return;
  }

  std::map<TracingSessionID, InodeFileDataSource*> file_sources;
  std::map<TracingSessionID, ProcessStatsDataSource*> ps_sources;
  for (const auto& pair : file_map_sources_)
    file_sources[pair.second->session_id()] = pair.second.get();
  for (const auto& pair : process_stats_sources_)
    ps_sources[pair.second->session_id()] = pair.second.get();

  for (const auto& id_to_source : delegates_) {
    const std::unique_ptr<SinkDelegate>& source = id_to_source.second;
    if (session_id != source->session_id())
      continue;
    if (!source->ps_source() && ps_sources.count(session_id))
      source->set_ps_source(ps_sources[session_id]->GetWeakPtr());
    if (!source->file_source() && file_sources.count(session_id))
      source->set_file_source(file_sources[session_id]->GetWeakPtr());
  }
}

void ProbesProducer::AddWatchdogsTimer(DataSourceInstanceID id,
                                       const DataSourceConfig& config) {
  if (config.trace_duration_ms() != 0)
    watchdogs_.emplace(id, base::Watchdog::GetInstance()->CreateFatalTimer(
                               5000 + 2 * config.trace_duration_ms()));
}

bool ProbesProducer::CreateFtraceDataSourceInstance(
    TracingSessionID session_id,
    DataSourceInstanceID id,
    const DataSourceConfig& config) {
  // Don't retry if FtraceController::Create() failed once.
  // This can legitimately happen on user builds where we cannot access the
  // debug paths, e.g., because of SELinux rules.
  if (ftrace_creation_failed_)
    return false;

  // Lazily create on the first instance.
  if (!ftrace_) {
    ftrace_ = FtraceController::Create(task_runner_);

    if (!ftrace_) {
      PERFETTO_ELOG("Failed to create FtraceController");
      ftrace_creation_failed_ = true;
      return false;
    }

    ftrace_->DisableAllEvents();
    ftrace_->ClearTrace();
  }

  PERFETTO_LOG("Ftrace start (id=%" PRIu64 ", target_buf=%" PRIu32 ")", id,
               config.target_buffer());

  FtraceConfig proto_config = config.ftrace_config();

  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(config.target_buffer()));
  auto delegate = std::unique_ptr<SinkDelegate>(
      new SinkDelegate(session_id, task_runner_, std::move(trace_writer)));
  auto sink = ftrace_->CreateSink(std::move(proto_config), delegate.get());
  if (!sink) {
    PERFETTO_ELOG("Failed to start tracing (maybe someone else is using it?)");
    return false;
  }
  delegate->set_sink(std::move(sink));
  delegates_.emplace(id, std::move(delegate));
  AddWatchdogsTimer(id, config);
  return true;
}

void ProbesProducer::CreateInodeFileDataSourceInstance(
    TracingSessionID session_id,
    DataSourceInstanceID id,
    DataSourceConfig source_config) {
  PERFETTO_LOG("Inode file map start (id=%" PRIu64 ", target_buf=%" PRIu32 ")",
               id, source_config.target_buffer());
  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(source_config.target_buffer()));
  if (system_inodes_.empty())
    CreateStaticDeviceToInodeMap("/system", &system_inodes_);
  auto file_map_source =
      std::unique_ptr<InodeFileDataSource>(new InodeFileDataSource(
          std::move(source_config), task_runner_, session_id, &system_inodes_,
          &cache_, std::move(trace_writer)));
  file_map_sources_.emplace(id, std::move(file_map_source));
  AddWatchdogsTimer(id, source_config);
}

void ProbesProducer::CreateProcessStatsDataSourceInstance(
    TracingSessionID session_id,
    DataSourceInstanceID id,
    const DataSourceConfig& config) {
  PERFETTO_DCHECK(process_stats_sources_.count(id) == 0);
  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(config.target_buffer()));
  auto source = std::unique_ptr<ProcessStatsDataSource>(
      new ProcessStatsDataSource(session_id, std::move(trace_writer), config));
  auto it_and_inserted = process_stats_sources_.emplace(id, std::move(source));
  if (!it_and_inserted.second) {
    PERFETTO_DCHECK(false);
    return;
  }
  ProcessStatsDataSource* ps_data_source = it_and_inserted.first->second.get();
  if (config.process_stats_config().scan_all_processes_on_start()) {
    ps_data_source->WriteAllProcesses();
  }
}

void ProbesProducer::TearDownDataSourceInstance(DataSourceInstanceID id) {
  PERFETTO_LOG("Producer stop (id=%" PRIu64 ")", id);
  // |id| could be the id of any of the datasources we handle:
  PERFETTO_DCHECK((failed_sources_.count(id) + delegates_.count(id) +
                   process_stats_sources_.count(id) +
                   file_map_sources_.count(id)) == 1);
  failed_sources_.erase(id);
  delegates_.erase(id);
  process_stats_sources_.erase(id);
  file_map_sources_.erase(id);
  watchdogs_.erase(id);
}

void ProbesProducer::OnTracingSetup() {}

void ProbesProducer::Flush(FlushRequestID flush_request_id,
                           const DataSourceInstanceID* data_source_ids,
                           size_t num_data_sources) {
  for (size_t i = 0; i < num_data_sources; i++) {
    DataSourceInstanceID ds_id = data_source_ids[i];
    {
      auto it = process_stats_sources_.find(ds_id);
      if (it != process_stats_sources_.end())
        it->second->Flush();
    }
    {
      auto it = file_map_sources_.find(ds_id);
      if (it != file_map_sources_.end())
        it->second->Flush();
    }
    {
      auto it = delegates_.find(ds_id);
      if (it != delegates_.end())
        it->second->Flush();
    }
  }
  endpoint_->NotifyFlushComplete(flush_request_id);
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

ProbesProducer::SinkDelegate::SinkDelegate(TracingSessionID id,
                                           base::TaskRunner* task_runner,
                                           std::unique_ptr<TraceWriter> writer)
    : session_id_(id),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {}

ProbesProducer::SinkDelegate::~SinkDelegate() = default;

void ProbesProducer::SinkDelegate::OnCreate(FtraceSink* sink) {
  sink->DumpFtraceStats(&stats_before_);
}

void ProbesProducer::SinkDelegate::Flush() {
  // TODO(primiano): this still doesn't flush data from the kernel ftrace
  // buffers (see b/73886018). We should do that and delay the
  // NotifyFlushComplete() until the ftrace data has been drained from the
  // kernel ftrace buffer and written in the SMB.
  if (writer_ && (!trace_packet_ || trace_packet_->is_finalized())) {
    WriteStats();
    writer_->Flush();
  }
}

void ProbesProducer::SinkDelegate::WriteStats() {
  {
    auto before_packet = writer_->NewTracePacket();
    auto out = before_packet->set_ftrace_stats();
    out->set_phase(protos::pbzero::FtraceStats_Phase_START_OF_TRACE);
    stats_before_.Write(out);
  }
  {
    FtraceStats stats_after{};
    sink_->DumpFtraceStats(&stats_after);
    auto after_packet = writer_->NewTracePacket();
    auto out = after_packet->set_ftrace_stats();
    out->set_phase(protos::pbzero::FtraceStats_Phase_END_OF_TRACE);
    stats_after.Write(out);
  }
}

ProbesProducer::FtraceBundleHandle
ProbesProducer::SinkDelegate::GetBundleForCpu(size_t) {
  trace_packet_ = writer_->NewTracePacket();
  return FtraceBundleHandle(trace_packet_->set_ftrace_events());
}

void ProbesProducer::SinkDelegate::OnBundleComplete(
    size_t,
    FtraceBundleHandle,
    const FtraceMetadata& metadata) {
  trace_packet_->Finalize();

  if (file_source_ && !metadata.inode_and_device.empty()) {
    auto inodes = metadata.inode_and_device;
    auto weak_file_source = file_source_;
    task_runner_->PostTask([weak_file_source, inodes] {
      if (weak_file_source)
        weak_file_source->OnInodes(inodes);
    });
  }
  if (ps_source_ && !metadata.pids.empty()) {
    const auto& quirks = ps_source_->config().process_stats_config().quirks();
    if (std::find(quirks.begin(), quirks.end(),
                  ProcessStatsConfig::DISABLE_ON_DEMAND) != quirks.end()) {
      return;
    }
    const auto& pids = metadata.pids;
    auto weak_ps_source = ps_source_;
    task_runner_->PostTask([weak_ps_source, pids] {
      if (weak_ps_source)
        weak_ps_source->OnPids(pids);
    });
  }
}

}  // namespace perfetto
