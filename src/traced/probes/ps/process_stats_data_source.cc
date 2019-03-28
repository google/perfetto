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
  if (poll_period_ms_ > 0 && poll_period_ms_ < 100) {
    PERFETTO_ILOG("proc_stats_poll_ms %" PRIu32
                  " is less than minimum of 100ms. Increasing to 100ms.",
                  poll_period_ms_);
    poll_period_ms_ = 100;
  }

  if (poll_period_ms_ > 0) {
    auto proc_stats_ttl_ms = ps_config.proc_stats_cache_ttl_ms();
    process_stats_cache_ttl_ticks_ =
        std::max(proc_stats_ttl_ms / poll_period_ms_, 1u);
  }
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
  PERFETTO_METATRACE("WriteAllProcesses", 0);
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
      if (record_thread_names_) {
        WriteProcessOrThread(tid);
      } else {
        // If we are not interested in thread names, there is no need to open
        // a proc file for each thread. We can save time and directly write the
        // thread record.
        WriteThread(tid, pid, /*optional_name=*/nullptr);
      }
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

void ProcessStatsDataSource::Flush(FlushRequestID,
                                   std::function<void()> callback) {
  // We shouldn't get this in the middle of WriteAllProcesses() or OnPids().
  PERFETTO_DCHECK(!cur_ps_tree_);
  PERFETTO_DCHECK(!cur_ps_stats_);
  PERFETTO_DCHECK(!cur_ps_stats_process_);
  writer_->Flush(callback);
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
    std::string thread_name;
    if (record_thread_names_)
      thread_name = ReadProcStatusEntry(proc_status, "Name:");
    WriteThread(pid, tgid, thread_name.empty() ? nullptr : thread_name.c_str());
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
                                         const char* optional_name) {
  auto* thread = GetOrCreatePsTree()->add_threads();
  thread->set_tid(tid);
  thread->set_tgid(tgid);
  if (optional_name)
    thread->set_name(optional_name);
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
  cur_ps_stats_process_ = nullptr;
  return cur_ps_tree_;
}

protos::pbzero::ProcessStats* ProcessStatsDataSource::GetOrCreateStats() {
  StartNewPacketIfNeeded();
  if (!cur_ps_stats_)
    cur_ps_stats_ = cur_packet_->set_process_stats();
  cur_ps_tree_ = nullptr;
  cur_ps_stats_process_ = nullptr;
  return cur_ps_stats_;
}

protos::pbzero::ProcessStats_Process*
ProcessStatsDataSource::GetOrCreateStatsProcess(int32_t pid) {
  if (cur_ps_stats_process_)
    return cur_ps_stats_process_;
  cur_ps_stats_process_ = GetOrCreateStats()->add_processes();
  cur_ps_stats_process_->set_pid(pid);
  return cur_ps_stats_process_;
}

void ProcessStatsDataSource::FinalizeCurPacket() {
  PERFETTO_DCHECK(!cur_ps_tree_ || cur_packet_);
  PERFETTO_DCHECK(!cur_ps_stats_ || cur_packet_);
  cur_ps_tree_ = nullptr;
  cur_ps_stats_ = nullptr;
  cur_ps_stats_process_ = nullptr;
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

  // We clear the cache every process_stats_cache_ttl_ticks_ ticks.
  if (++thiz.ticks_ == thiz.process_stats_cache_ttl_ticks_) {
    thiz.ticks_ = 0;
    thiz.process_stats_cache_.clear();
  }
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
    cur_ps_stats_process_ = nullptr;

    uint32_t pid_u = static_cast<uint32_t>(pid);
    if (skip_stats_for_pids_.size() > pid_u && skip_stats_for_pids_[pid_u])
      continue;

    std::string proc_status = ReadProcPidFile(pid, "status");
    if (proc_status.empty())
      continue;

    if (!WriteMemCounters(pid, proc_status)) {
      // If WriteMemCounters() fails the pid is very likely a kernel thread
      // that has a valid /proc/[pid]/status but no memory values. In this
      // case avoid keep polling it over and over.
      if (skip_stats_for_pids_.size() <= pid_u)
        skip_stats_for_pids_.resize(pid_u + 1);
      skip_stats_for_pids_[pid_u] = true;
      continue;
    }

    std::string oom_score_adj = ReadProcPidFile(pid, "oom_score_adj");
    if (!oom_score_adj.empty()) {
      CachedProcessStats& cached = process_stats_cache_[pid];
      auto counter = ToInt(oom_score_adj);
      if (counter != cached.oom_score_adj) {
        GetOrCreateStatsProcess(pid)->set_oom_score_adj(counter);
        cached.oom_score_adj = counter;
      }
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
bool ProcessStatsDataSource::WriteMemCounters(int32_t pid,
                                              const std::string& proc_status) {
  bool proc_status_has_mem_counters = false;
  CachedProcessStats& cached = process_stats_cache_[pid];

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

        auto counter = ToU32(value.data());
        if (counter != cached.vm_size_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_size_kb(counter);
          cached.vm_size_kb = counter;
        }
      } else if (strcmp(key.data(), "VmLck") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.vm_locked_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_locked_kb(counter);
          cached.vm_locked_kb = counter;
        }
      } else if (strcmp(key.data(), "VmHWM") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.vm_hvm_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_hwm_kb(counter);
          cached.vm_hvm_kb = counter;
        }
      } else if (strcmp(key.data(), "VmRSS") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.vm_rss_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_rss_kb(counter);
          cached.vm_rss_kb = counter;
        }
      } else if (strcmp(key.data(), "RssAnon") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.rss_anon_kb) {
          GetOrCreateStatsProcess(pid)->set_rss_anon_kb(counter);
          cached.rss_anon_kb = counter;
        }
      } else if (strcmp(key.data(), "RssFile") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.rss_file_kb) {
          GetOrCreateStatsProcess(pid)->set_rss_file_kb(counter);
          cached.rss_file_kb = counter;
        }
      } else if (strcmp(key.data(), "RssShmem") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.rss_shmem_kb) {
          GetOrCreateStatsProcess(pid)->set_rss_shmem_kb(counter);
          cached.rss_shmem_kb = counter;
        }
      } else if (strcmp(key.data(), "VmSwap") == 0) {
        auto counter = ToU32(value.data());
        if (counter != cached.vm_swap_kb) {
          GetOrCreateStatsProcess(pid)->set_vm_swap_kb(counter);
          cached.vm_swap_kb = counter;
        }
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
