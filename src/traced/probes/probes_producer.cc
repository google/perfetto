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
#include <queue>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/ftrace_config.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"

#include "src/process_stats/file_utils.h"
#include "src/process_stats/procfs_utils.h"

#include "perfetto/trace/filesystem/inode_file_map.pbzero.h"
#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "perfetto/trace/ps/process_tree.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

uint64_t kInitialConnectionBackoffMs = 100;
uint64_t kMaxConnectionBackoffMs = 30 * 1000;
const char* kFtraceSourceName = "com.google.perfetto.ftrace";
const char* kProcessStatsSourceName = "com.google.perfetto.process_stats";
const char* kInodeFileMapSourceName = "com.google.perfetto.inode_file_map";

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
  endpoint_->RegisterDataSource(ftrace_descriptor, [](DataSourceInstanceID) {});

  DataSourceDescriptor process_stats_descriptor;
  process_stats_descriptor.set_name(kProcessStatsSourceName);
  endpoint_->RegisterDataSource(process_stats_descriptor,
                                [](DataSourceInstanceID) {});

  DataSourceDescriptor inode_map_descriptor;
  inode_map_descriptor.set_name(kInodeFileMapSourceName);
  endpoint_->RegisterDataSource(inode_map_descriptor,
                                [](DataSourceInstanceID) {});
}

void ProbesProducer::OnDisconnect() {
  PERFETTO_DCHECK(state_ == kConnected || state_ == kConnecting);
  state_ = kNotConnected;
  PERFETTO_LOG("Disconnected from tracing service");
  IncreaseConnectionBackoff();

  // TODO(hjd): Erase all sinks and add e2e test for this.
  task_runner_->PostDelayedTask([this] { this->Connect(); },
                                connection_backoff_ms_);
}

void ProbesProducer::CreateDataSourceInstance(
    DataSourceInstanceID id,
    const DataSourceConfig& source_config) {
  instances_[id] = source_config.name();
  if (source_config.name() == kFtraceSourceName) {
    CreateFtraceDataSourceInstance(id, source_config);
  } else if (source_config.name() == kProcessStatsSourceName) {
    CreateProcessStatsDataSourceInstance(source_config);
  } else if (source_config.name() == kInodeFileMapSourceName) {
    CreateInodeFileMapDataSourceInstance(id, source_config);
  } else {
    PERFETTO_ELOG("Data source name: %s not recognised.",
                  source_config.name().c_str());
  }
}

void ProbesProducer::AddWatchdogsTimer(DataSourceInstanceID id,
                                       const DataSourceConfig& source_config) {
  if (source_config.trace_duration_ms() != 0)
    watchdogs_.emplace(id, base::Watchdog::GetInstance()->CreateFatalTimer(
                               5000 + 2 * source_config.trace_duration_ms()));
}

void ProbesProducer::CreateFtraceDataSourceInstance(
    DataSourceInstanceID id,
    const DataSourceConfig& source_config) {
  // Don't retry if FtraceController::Create() failed once.
  // This can legitimately happen on user builds where we cannot access the
  // debug paths, e.g., because of SELinux rules.
  if (ftrace_creation_failed_)
    return;

  // Lazily create on the first instance.
  if (!ftrace_) {
    ftrace_ = FtraceController::Create(task_runner_);

    if (!ftrace_) {
      PERFETTO_ELOG("Failed to create FtraceController");
      ftrace_creation_failed_ = true;
      return;
    }

    ftrace_->DisableAllEvents();
    ftrace_->ClearTrace();
  }

  PERFETTO_LOG("Ftrace start (id=%" PRIu64 ", target_buf=%" PRIu32 ")", id,
               source_config.target_buffer());

  FtraceConfig proto_config = source_config.ftrace_config();

  // TODO(hjd): Static cast is bad, target_buffer() should return a BufferID.
  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(source_config.target_buffer()));
  auto delegate = std::unique_ptr<SinkDelegate>(
      new SinkDelegate(task_runner_, std::move(trace_writer)));
  auto sink = ftrace_->CreateSink(std::move(proto_config), delegate.get());
  if (!sink) {
    PERFETTO_ELOG("Failed to start tracing (maybe someone else is using it?)");
    return;
  }
  delegate->sink(std::move(sink));
  delegates_.emplace(id, std::move(delegate));
  AddWatchdogsTimer(id, source_config);
}

void ProbesProducer::CreateInodeFileMapDataSourceInstance(
    DataSourceInstanceID id,
    const DataSourceConfig& source_config) {
  PERFETTO_LOG("Inode file map start (id=%" PRIu64 ", target_buf=%" PRIu32 ")",
               id, source_config.target_buffer());
  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(source_config.target_buffer()));
  auto file_map_source = std::unique_ptr<InodeFileMapDataSource>(
      new InodeFileMapDataSource(std::move(trace_writer)));
  file_map_sources_.emplace(id, std::move(file_map_source));
  AddWatchdogsTimer(id, source_config);
}

void ProbesProducer::CreateProcessStatsDataSourceInstance(
    const DataSourceConfig& source_config) {
  auto trace_writer = endpoint_->CreateTraceWriter(
      static_cast<BufferID>(source_config.target_buffer()));
  procfs_utils::ProcessMap processes;
  auto trace_packet = trace_writer->NewTracePacket();
  protos::pbzero::ProcessTree* process_tree = trace_packet->set_process_tree();

  file_utils::ForEachPidInProcPath(
      "/proc", [&processes, &process_tree](int pid) {
        if (!processes.count(pid)) {
          if (procfs_utils::ReadTgid(pid) != pid)
            return;
          processes[pid] = procfs_utils::ReadProcessInfo(pid);
        }
        ProcessInfo* process = processes[pid].get();
        procfs_utils::ReadProcessThreads(process);
        auto* process_writer = process_tree->add_processes();
        process_writer->set_pid(process->pid);
        process_writer->set_ppid(process->ppid);
        for (const auto& field : process->cmdline)
          process_writer->add_cmdline(field.c_str());
        for (auto& thread : process->threads) {
          auto* thread_writer = process_writer->add_threads();
          thread_writer->set_tid(thread.second.tid);
          thread_writer->set_name(thread.second.name);
        }
      });
  trace_packet->Finalize();
}

void ProbesProducer::TearDownDataSourceInstance(DataSourceInstanceID id) {
  PERFETTO_LOG("Producer stop (id=%" PRIu64 ")", id);
  PERFETTO_DCHECK(instances_.count(id));
  if (instances_[id] == kFtraceSourceName) {
    size_t removed = delegates_.erase(id);
    PERFETTO_DCHECK(removed == 1);
    // Might return 0 if trace_duration_ms == 0.
    watchdogs_.erase(id);
  } else if (instances_[id] == kInodeFileMapSourceName) {
    size_t removed = file_map_sources_.erase(id);
    PERFETTO_DCHECK(removed == 1);
    watchdogs_.erase(id);
  }
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
  endpoint_ = ProducerIPCClient::Connect(socket_name_, this, task_runner_);
}

void ProbesProducer::IncreaseConnectionBackoff() {
  connection_backoff_ms_ *= 2;
  if (connection_backoff_ms_ > kMaxConnectionBackoffMs)
    connection_backoff_ms_ = kMaxConnectionBackoffMs;
}

void ProbesProducer::ResetConnectionBackoff() {
  connection_backoff_ms_ = kInitialConnectionBackoffMs;
}

ProbesProducer::SinkDelegate::SinkDelegate(base::TaskRunner* task_runner,
                                           std::unique_ptr<TraceWriter> writer)
    : task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {}

ProbesProducer::SinkDelegate::~SinkDelegate() = default;

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
  if (!metadata.inodes.empty()) {
    auto weak_this = weak_factory_.GetWeakPtr();
    auto inodes = metadata.inodes;
    task_runner_->PostTask([weak_this, inodes] {
      if (weak_this)
        weak_this->OnInodes(inodes);
    });
  }
}

void ProbesProducer::SinkDelegate::OnInodes(
    const std::vector<uint64_t>& inodes) {
  PERFETTO_DLOG("Saw FtraceBundle with %zu inodes.", inodes.size());
}

ProbesProducer::InodeFileMapDataSource::InodeFileMapDataSource(
    std::unique_ptr<TraceWriter> writer)
    : writer_(std::move(writer)) {}

ProbesProducer::InodeFileMapDataSource::~InodeFileMapDataSource() = default;

void ProbesProducer::InodeFileMapDataSource::WriteInodes(
    const FtraceMetadata& metadata) {
  auto trace_packet = writer_->NewTracePacket();
  auto inode_file_map = trace_packet->set_inode_file_map();
  // TODO(azappone): Add block_device_id and mount_points
  auto inodes = metadata.inodes;
  for (const auto& inode : inodes) {
    auto* entry = inode_file_map->add_entries();
    entry->set_inode_number(inode);
    // TODO(azappone): Add resolving filepaths and type
  }
  trace_packet->Finalize();
}

}  // namespace perfetto
