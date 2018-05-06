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

#include "src/traced/probes/process_stats_data_source.h"

#include <stdlib.h>

#include <utility>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/string_splitter.h"
#include "perfetto/trace/trace_packet.pbzero.h"

// TODO(primiano): the code in this file assumes that PIDs are never recycled
// and that processes/threads never change names. Neither is always true.

// The notion of PID in the Linux kernel is a bit confusing.
// - PID: is really the thread id (for the main thread: PID == TID).
// - TGID (thread group ID): is the Unix Process ID (the actual PID).
// - PID == TGID for the main thread: the TID of the main thread is also the PID
//   of the process.
// So, in this file, |pid| might refer to either a process id or a thread id.

namespace perfetto {

namespace {

bool IsNumeric(const char* str) {
  if (!str || !*str)
    return false;
  for (const char* c = str; *c; c++) {
    if (!isdigit(*c))
      return false;
  }
  return true;
}

int32_t ReadNextNumericDir(DIR* dirp) {
  while (struct dirent* dir_ent = readdir(dirp)) {
    if (dir_ent->d_type == DT_DIR && IsNumeric(dir_ent->d_name))
      return atoi(dir_ent->d_name);
  }
  return 0;
}

inline int ToInt(const std::string& str) {
  return atoi(str.c_str());
}

}  // namespace

ProcessStatsDataSource::ProcessStatsDataSource(
    TracingSessionID id,
    std::unique_ptr<TraceWriter> writer,
    const DataSourceConfig& config)
    : session_id_(id),
      writer_(std::move(writer)),
      config_(config),
      record_thread_names_(config.process_stats_config().record_thread_names()),
      weak_factory_(this) {}

ProcessStatsDataSource::~ProcessStatsDataSource() = default;

base::WeakPtr<ProcessStatsDataSource> ProcessStatsDataSource::GetWeakPtr()
    const {
  return weak_factory_.GetWeakPtr();
}

void ProcessStatsDataSource::WriteAllProcesses() {
  PERFETTO_DCHECK(!cur_ps_tree_);
  base::ScopedDir proc_dir(opendir("/proc"));
  if (!proc_dir) {
    PERFETTO_PLOG("Failed to opendir(/proc)");
    return;
  }
  while (int32_t pid = ReadNextNumericDir(*proc_dir)) {
    WriteProcessOrThread(pid);
    char task_path[255];
    sprintf(task_path, "/proc/%d/task", pid);
    base::ScopedDir task_dir(opendir(task_path));
    if (!task_dir)
      continue;
    while (int32_t tid = ReadNextNumericDir(*task_dir)) {
      if (tid == pid)
        continue;
      WriteProcessOrThread(tid);
    }
  }
  FinalizeCurPsTree();
}

void ProcessStatsDataSource::OnPids(const std::vector<int32_t>& pids) {
  PERFETTO_DCHECK(!cur_ps_tree_);
  for (int32_t pid : pids) {
    if (seen_pids_.count(pid) || pid == 0)
      continue;
    WriteProcessOrThread(pid);
  }
  FinalizeCurPsTree();
}

void ProcessStatsDataSource::Flush() {
  // We shouldn't get this in the middle of WriteAllProcesses() or OnPids().
  PERFETTO_DCHECK(!cur_ps_tree_);

  writer_->Flush();
}

void ProcessStatsDataSource::WriteProcessOrThread(int32_t pid) {
  std::string proc_status = ReadProcPidFile(pid, "status");
  if (proc_status.empty())
    return;
  int tgid = ToInt(ReadProcStatusEntry(proc_status, "Tgid:"));
  if (tgid <= 0)
    return;
  if (!seen_pids_.count(tgid))
    WriteProcess(tgid, proc_status);
  if (pid != tgid) {
    PERFETTO_DCHECK(!seen_pids_.count(pid));
    WriteThread(pid, tgid, proc_status);
  }
}

void ProcessStatsDataSource::WriteProcess(int32_t pid,
                                          const std::string& proc_status) {
  PERFETTO_DCHECK(ToInt(ReadProcStatusEntry(proc_status, "Tgid:")) == pid);
  auto* proc = GetOrCreatePsTree()->add_processes();
  proc->set_pid(pid);
  proc->set_ppid(ToInt(ReadProcStatusEntry(proc_status, "PPid:")));

  std::string cmdline = ReadProcPidFile(pid, "cmdline");
  if (!cmdline.empty()) {
    using base::StringSplitter;
    for (StringSplitter ss(&cmdline[0], cmdline.size(), '\0'); ss.Next();)
      proc->add_cmdline(ss.cur_token());
  } else {
    // Nothing in cmdline so use the thread name instead (which is == "comm").
    proc->add_cmdline(ReadProcStatusEntry(proc_status, "Name:").c_str());
  }
  seen_pids_.emplace(pid);
}

void ProcessStatsDataSource::WriteThread(int32_t tid,
                                         int32_t tgid,
                                         const std::string& proc_status) {
  auto* thread = GetOrCreatePsTree()->add_threads();
  thread->set_tid(tid);
  thread->set_tgid(tgid);
  if (record_thread_names_)
    thread->set_name(ReadProcStatusEntry(proc_status, "Name:").c_str());
  seen_pids_.emplace(tid);
}

std::string ProcessStatsDataSource::ReadProcPidFile(int32_t pid,
                                                    const std::string& file) {
  std::string contents;
  contents.reserve(4096);
  if (!base::ReadFile("/proc/" + std::to_string(pid) + "/" + file, &contents))
    return "";
  return contents;
}

std::string ProcessStatsDataSource::ReadProcStatusEntry(const std::string& buf,
                                                        const char* key) {
  auto begin = buf.find(key);
  if (begin == std::string::npos)
    return "";
  begin = buf.find_first_not_of(" \t", begin + strlen(key));
  if (begin == std::string::npos)
    return "";
  auto end = buf.find('\n', begin);
  if (end == std::string::npos || end <= begin)
    return "";
  return buf.substr(begin, end - begin);
}

protos::pbzero::ProcessTree* ProcessStatsDataSource::GetOrCreatePsTree() {
  if (!cur_ps_tree_) {
    cur_packet_ = writer_->NewTracePacket();
    cur_ps_tree_ = cur_packet_->set_process_tree();
  }
  return cur_ps_tree_;
}

void ProcessStatsDataSource::FinalizeCurPsTree() {
  if (!cur_ps_tree_) {
    PERFETTO_DCHECK(!cur_packet_);
    return;
  }
  cur_ps_tree_ = nullptr;
  cur_packet_ = TraceWriter::TracePacketHandle{};
}

}  // namespace perfetto
