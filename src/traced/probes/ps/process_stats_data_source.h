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

#ifndef SRC_TRACED_PROBES_PS_PROCESS_STATS_DATA_SOURCE_H_
#define SRC_TRACED_PROBES_PS_PROCESS_STATS_DATA_SOURCE_H_

#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "perfetto/base/scoped_file.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "src/traced/probes/probes_data_source.h"

namespace perfetto {

namespace base {
class TaskRunner;
}

namespace protos {
namespace pbzero {
class ProcessTree;
class ProcessStats;
class ProcessStats_Process;
}  // namespace pbzero
}  // namespace protos

class ProcessStatsDataSource : public ProbesDataSource {
 public:
  static constexpr int kTypeId = 3;

  ProcessStatsDataSource(base::TaskRunner*,
                         TracingSessionID,
                         std::unique_ptr<TraceWriter> writer,
                         const DataSourceConfig&);
  ~ProcessStatsDataSource() override;

  base::WeakPtr<ProcessStatsDataSource> GetWeakPtr() const;
  void WriteAllProcesses();
  void OnPids(const std::vector<int32_t>& pids);

  // ProbesDataSource implementation.
  void Start() override;
  void Flush(FlushRequestID, std::function<void()> callback) override;

  bool on_demand_dumps_enabled() const { return enable_on_demand_dumps_; }

  // Virtual for testing.
  virtual base::ScopedDir OpenProcDir();
  virtual std::string ReadProcPidFile(int32_t pid, const std::string& file);

 private:
  struct CachedProcessStats {
    uint32_t vm_size_kb = std::numeric_limits<uint32_t>::max();
    uint32_t vm_rss_kb = std::numeric_limits<uint32_t>::max();
    uint32_t rss_anon_kb = std::numeric_limits<uint32_t>::max();
    uint32_t rss_file_kb = std::numeric_limits<uint32_t>::max();
    uint32_t rss_shmem_kb = std::numeric_limits<uint32_t>::max();
    uint32_t vm_swap_kb = std::numeric_limits<uint32_t>::max();
    uint32_t vm_locked_kb = std::numeric_limits<uint32_t>::max();
    uint32_t vm_hvm_kb = std::numeric_limits<uint32_t>::max();
    int oom_score_adj = std::numeric_limits<int>::max();
  };

  // Common functions.
  ProcessStatsDataSource(const ProcessStatsDataSource&) = delete;
  ProcessStatsDataSource& operator=(const ProcessStatsDataSource&) = delete;

  void StartNewPacketIfNeeded();
  void FinalizeCurPacket();
  protos::pbzero::ProcessTree* GetOrCreatePsTree();
  protos::pbzero::ProcessStats* GetOrCreateStats();
  protos::pbzero::ProcessStats_Process* GetOrCreateStatsProcess(int32_t pid);

  // Functions for snapshotting process/thread long-term info and relationships.
  void WriteProcess(int32_t pid, const std::string& proc_status);
  void WriteThread(int32_t tid, int32_t tgid, const char* optional_name);
  void WriteProcessOrThread(int32_t pid);
  std::string ReadProcStatusEntry(const std::string& buf, const char* key);

  // Functions for periodically sampling process stats/counters.
  static void Tick(base::WeakPtr<ProcessStatsDataSource>);
  void WriteAllProcessStats();
  bool WriteMemCounters(int32_t pid, const std::string& proc_status);

  // Common fields used for both process/tree relationships and stats/counters.
  base::TaskRunner* const task_runner_;
  std::unique_ptr<TraceWriter> writer_;
  TraceWriter::TracePacketHandle cur_packet_;

  // Fields for keeping track of the state of process/tree relationships.
  protos::pbzero::ProcessTree* cur_ps_tree_ = nullptr;
  bool record_thread_names_ = false;
  bool enable_on_demand_dumps_ = true;
  bool dump_all_procs_on_start_ = false;

  // This set contains PIDs as per the Linux kernel notion of a PID (which is
  // really a TID). In practice this set will contain all TIDs for all processes
  // seen, not just the main thread id (aka thread group ID).
  std::set<int32_t> seen_pids_;

  // Fields for keeping track of the periodic stats/counters.
  uint32_t poll_period_ms_ = 0;
  uint64_t ticks_ = 0;
  protos::pbzero::ProcessStats* cur_ps_stats_ = nullptr;
  protos::pbzero::ProcessStats_Process* cur_ps_stats_process_ = nullptr;
  std::vector<bool> skip_stats_for_pids_;

  // Cached process stats per process. Cleared every |cache_ttl_ticks_| *
  // |poll_period_ms_| ms.
  uint32_t process_stats_cache_ttl_ticks_ = 0;
  std::unordered_map<int32_t, CachedProcessStats> process_stats_cache_;

  base::WeakPtrFactory<ProcessStatsDataSource> weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_PS_PROCESS_STATS_DATA_SOURCE_H_
