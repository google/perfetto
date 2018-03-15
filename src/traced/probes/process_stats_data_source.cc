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

#include "process_stats_data_source.h"

#include <utility>

#include "perfetto/trace/ps/process_tree.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/process_stats/file_utils.h"
#include "src/process_stats/procfs_utils.h"

namespace perfetto {

ProcessStatsDataSource::ProcessStatsDataSource(
    TracingSessionID id,
    std::unique_ptr<TraceWriter> writer)
    : session_id_(id), writer_(std::move(writer)), weak_factory_(this) {}

ProcessStatsDataSource::~ProcessStatsDataSource() = default;

base::WeakPtr<ProcessStatsDataSource> ProcessStatsDataSource::GetWeakPtr()
    const {
  return weak_factory_.GetWeakPtr();
}

void ProcessStatsDataSource::WriteAllProcesses() {
  procfs_utils::ProcessMap processes;
  auto trace_packet = writer_->NewTracePacket();
  protos::pbzero::ProcessTree* process_tree = trace_packet->set_process_tree();

  file_utils::ForEachPidInProcPath(
      "/proc", [&processes, process_tree](int pid) {
        // ForEachPid will list all processes and threads. Here we want to
        // iterate first only by processes (for which pid == thread group id)
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
}

void ProcessStatsDataSource::OnPids(const std::vector<int32_t>& pids) {
  PERFETTO_DLOG("Saw FtraceBundle with %zu pids.", pids.size());
}

}  // namespace perfetto
