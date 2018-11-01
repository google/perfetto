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

#include "src/traced/probes/ps/process_stats_data_source.h"

#include <stdlib.h>

#include <algorithm>
#include <utility>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/metatrace.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/string_splitter.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"

#include "perfetto/trace/ps/process_stats.pbzero.h"
#include "perfetto/trace/ps/process_tree.pbzero.h"
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

inline int32_t ParseIntValue(const char* str) {
  int32_t ret = 0;
  for (;;) {
    char c = *(str++);
    if (!c)
      break;
    if (c < '0' || c > '9')
      return 0;
    ret *= 10;
    ret += static_cast<int32_t>(c - '0');
  }
  return ret;
}

int32_t ReadNextNumericDir(DIR* dirp) {
  while (struct dirent* dir_ent = readdir(dirp)) {
    if (dir_ent->d_type != DT_DIR)
      continue;
    int32_t int_value = ParseIntValue(dir_ent->d_name);
    if (int_value)
      return int_value;
  }
  return 0;
}

inline int ToInt(const std::string& str) {
  return atoi(str.c_str());
}

inline uint32_t ToU32(const char* str) {
  return static_cast<uint32_t>(strtol(str, nullptr, 10));
}

}  // namespace

// static
constexpr int ProcessStatsDataSource::kTypeId;

ProcessStatsDataSource::ProcessStatsDataSource(
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer,
    const DataSourceConfig& config)
    : ProbesDataSource(session_id, kTypeId),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      record_thread_names_(config.process_stats_config().record_thread_names()),
      dump_all_procs_on_start_(
          config.process_stats_config().scan_all_processes_on_start()),
      weak_factory_(this) {
  const auto& ps_config = config.process_stats_config();
  const auto& quirks = ps_config.quirks();
  enable_on_demand_dumps_ =
      (std::find(quirks.begin(), quirks.end(),
                 ProcessStatsConfig::DISABLE_ON_DEMAND) == quirks.end());
  poll_period_ms_ = ps_config.proc_stats_poll_ms();
}

ProcessStatsDataSource::~ProcessStatsDataSource() = default;

void ProcessStatsDataSource::Start() {
  if (dump_all_procs_on_start_)
    WriteAllProcesses();

  if (poll_period_ms_) {
    auto weak_this = GetWeakPtr();
    task_runner_->PostTask(std::bind(&ProcessStatsDataSource::Tick, weak_this));
  }
}

base::WeakPtr<ProcessStatsDataSource> ProcessStatsDataSource::GetWeakPtr()
    const {
  return weak_factory_.GetWeakPtr();
}

void ProcessStatsDataSource::WriteAllProcesses() {
  PERFETTO_DCHECK(!cur_ps_tree_);
  base::ScopedDir proc_dir = OpenProcDir();
  if (!proc_dir)
    return;
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
  FinalizeCurPacket();
}

void ProcessStatsDataSource::OnPids(const std::vector<int32_t>& pids) {
  PERFETTO_METATRACE("OnPids", 0);
  if (!enable_on_demand_dumps_)
    return;
  PERFETTO_DCHECK(!cur_ps_tree_);
  for (int32_t pid : pids) {
    if (seen_pids_.count(pid) || pid == 0)
      continue;
    WriteProcessOrThread(pid);
  }
  FinalizeCurPacket();
}

void ProcessStatsDataSource::Flush() {
  // We shouldn't get this in the middle of WriteAllProcesses() or OnPids().
  PERFETTO_DCHECK(!cur_ps_tree_);
  PERFETTO_DCHECK(!cur_ps_stats_);
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

base::ScopedDir ProcessStatsDataSource::OpenProcDir() {
  base::ScopedDir proc_dir(opendir("/proc"));
  if (!proc_dir)
    PERFETTO_PLOG("Failed to opendir(/proc)");
  return proc_dir;
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

void ProcessStatsDataSource::StartNewPacketIfNeeded() {
  if (cur_packet_)
    return;
  cur_packet_ = writer_->NewTracePacket();
  uint64_t now = static_cast<uint64_t>(base::GetBootTimeNs().count());
  cur_packet_->set_timestamp(now);
}

protos::pbzero::ProcessTree* ProcessStatsDataSource::GetOrCreatePsTree() {
  StartNewPacketIfNeeded();
  if (!cur_ps_tree_)
    cur_ps_tree_ = cur_packet_->set_process_tree();
  cur_ps_stats_ = nullptr;
  return cur_ps_tree_;
}

protos::pbzero::ProcessStats* ProcessStatsDataSource::GetOrCreateStats() {
  StartNewPacketIfNeeded();
  if (!cur_ps_stats_)
    cur_ps_stats_ = cur_packet_->set_process_stats();
  cur_ps_tree_ = nullptr;
  return cur_ps_stats_;
}

void ProcessStatsDataSource::FinalizeCurPacket() {
  PERFETTO_DCHECK(!cur_ps_tree_ || cur_packet_);
  PERFETTO_DCHECK(!cur_ps_stats_ || cur_packet_);
  cur_ps_tree_ = nullptr;
  cur_ps_stats_ = nullptr;
  cur_packet_ = TraceWriter::TracePacketHandle{};
}

// static
void ProcessStatsDataSource::Tick(
    base::WeakPtr<ProcessStatsDataSource> weak_this) {
  if (!weak_this)
    return;
  ProcessStatsDataSource& thiz = *weak_this;
  uint32_t period_ms = thiz.poll_period_ms_;
  uint32_t delay_ms = period_ms - (base::GetWallTimeMs().count() % period_ms);
  thiz.task_runner_->PostDelayedTask(
      std::bind(&ProcessStatsDataSource::Tick, weak_this), delay_ms);
  thiz.WriteAllProcessStats();
}

void ProcessStatsDataSource::WriteAllProcessStats() {
  // TODO(primiano): implement whitelisting of processes by names.
  // TODO(primiano): Have a pid cache to avoid wasting cycles reading kthreads
  // proc files over and over. Same for non-whitelist processes (see above).

  PERFETTO_METATRACE("WriteAllProcessStats", 0);
  base::ScopedDir proc_dir = OpenProcDir();
  if (!proc_dir)
    return;
  std::vector<int32_t> pids;
  while (int32_t pid = ReadNextNumericDir(*proc_dir)) {
    uint32_t pid_u = static_cast<uint32_t>(pid);
    if (pids_to_skip_.size() > pid_u && pids_to_skip_[pid_u])
      continue;
    std::string proc_status = ReadProcPidFile(pid, "status");
    if (proc_status.empty())
      continue;
    if (!WriteProcessStats(pid, proc_status)) {
      // If WriteProcessStats() fails the pid is very likely a kernel thread
      // that has a valid /proc/[pid]/status but no memory values. In this
      // case avoid keep polling it over and over.
      if (pids_to_skip_.size() <= pid_u)
        pids_to_skip_.resize(pid_u + 1);
      pids_to_skip_[pid_u] = true;
      continue;
    }
    pids.push_back(pid);
  }
  FinalizeCurPacket();

  // Ensure that we write once long-term process info (e.g., name) for new pids
  // that we haven't seen before.
  OnPids(pids);
}

// Returns true if the stats for the given |pid| have been written, false it
// it failed (e.g., |pid| was a kernel thread and, as such, didn't report any
// memory counters).
bool ProcessStatsDataSource::WriteProcessStats(int32_t pid,
                                               const std::string& proc_status) {
  // The MemCounters entry for a process is created lazily on the first call.
  // This is to prevent creating empty entries that have only a pid for
  // kernel threads and other /proc/[pid] entries that have no counters
  // associated.
  bool proc_status_has_mem_counters = false;
  protos::pbzero::ProcessStats::MemCounters* mem_counters = nullptr;
  auto get_counters_lazy = [this, &mem_counters, pid] {
    if (!mem_counters) {
      mem_counters = GetOrCreateStats()->add_mem_counters();
      mem_counters->set_pid(pid);
    }
    return mem_counters;
  };

  // Parse /proc/[pid]/status, which looks like this:
  // Name:   cat
  // Umask:  0027
  // State:  R (running)
  // FDSize: 256
  // Groups: 4 20 24 46 997
  // VmPeak:     5992 kB
  // VmSize:     5992 kB
  // VmLck:         0 kB
  // ...
  std::vector<char> key;
  std::vector<char> value;
  enum { kKey, kSeparator, kValue } state = kKey;
  for (char c : proc_status) {
    if (c == '\n') {
      key.push_back('\0');
      value.push_back('\0');

      // |value| will contain "1234 KB". We rely on strtol() (in ToU32()) to
      // stop parsing at the first non-numeric character.
      if (strcmp(key.data(), "VmSize") == 0) {
        // Assume that if we see VmSize we'll see also the others.
        proc_status_has_mem_counters = true;
        get_counters_lazy()->set_vm_size_kb(ToU32(value.data()));
      } else if (strcmp(key.data(), "VmLck") == 0) {
        get_counters_lazy()->set_vm_locked_kb(ToU32(value.data()));
      } else if (strcmp(key.data(), "VmHWM") == 0) {
        get_counters_lazy()->set_vm_hwm_kb(ToU32(value.data()));
      } else if (strcmp(key.data(), "VmRSS") == 0) {
        get_counters_lazy()->set_vm_rss_kb(ToU32(value.data()));
      } else if (strcmp(key.data(), "RssAnon") == 0) {
        get_counters_lazy()->set_rss_anon_kb(ToU32(value.data()));
      } else if (strcmp(key.data(), "RssFile") == 0) {
        get_counters_lazy()->set_rss_file_kb(ToU32(value.data()));
      } else if (strcmp(key.data(), "RssShmem") == 0) {
        get_counters_lazy()->set_rss_shmem_kb(ToU32(value.data()));
      } else if (strcmp(key.data(), "VmSwap") == 0) {
        get_counters_lazy()->set_vm_swap_kb(ToU32(value.data()));
      }

      key.clear();
      state = kKey;
      continue;
    }

    if (state == kKey) {
      if (c == ':') {
        state = kSeparator;
        continue;
      }
      key.push_back(c);
      continue;
    }

    if (state == kSeparator) {
      if (isspace(c))
        continue;
      value.clear();
      value.push_back(c);
      state = kValue;
      continue;
    }

    if (state == kValue) {
      value.push_back(c);
    }
  }
  return proc_status_has_mem_counters;
}

}  // namespace perfetto
