/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_PROFILING_SMAPS_SMAPS_DATA_SOURCE_H_
#define SRC_PROFILING_SMAPS_SMAPS_DATA_SOURCE_H_

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "protos/perfetto/config/profiling/smaps_config.gen.h"

namespace perfetto {
namespace profiling {

// Data source name: "linux.smaps".
// Config: ProcessSmapsConfig in smaps_config.proto.
class SmapsDataSource {
 public:
  static constexpr char kDataSourceName[] = "linux.smaps";

  // Validated data source config.
  class Config {
   public:
    static std::optional<Config> Create(const DataSourceConfig& ds_config);

    protos::gen::SmapsConfig recording_config;
    std::vector<std::string> target_cmdlines;
    uint32_t read_period_ms = 0;            // if zero: capture once
    uint32_t max_processes_per_period = 0;  // if zero: unlimited
   private:
    Config() = default;
  };

  SmapsDataSource(Config config,
                  base::TaskRunner* task_runner,
                  std::unique_ptr<TraceWriter> trace_writer);

  void Start();
  void Flush();
  // Marks the data source as stopping, which lets the currently-enqueued work
  // to complete before stopping. |on_stopped| is called from a separate task,
  // and is allowed to destroy this instance.
  void Stop(std::function<void()> on_stopped);

  ~SmapsDataSource() = default;

  SmapsDataSource(const SmapsDataSource&) = delete;
  SmapsDataSource& operator=(const SmapsDataSource&) = delete;
  SmapsDataSource(SmapsDataSource&&) = delete;
  SmapsDataSource& operator=(SmapsDataSource&&) = delete;

  using HashAndPid = std::pair<uint64_t, pid_t>;

  // public for testing
  static std::vector<pid_t> PickMatchingTargets(
      const std::vector<HashAndPid>& pids,
      const std::function<bool(pid_t)>& filter,
      uint32_t max_count,
      HashAndPid* last_picked);

 private:
  // Enqueues the work necessary for one pass, and reposts itself if the config
  // is periodic.
  void Tick();
  // Walks all processes in a pseudo-random order, and queues a read for each
  // one matching the target patterns, up to |max_processes_per_period|.
  void QueueSmapsReads();
  // Serializes the smaps of one queued process, and reposts itself if there are
  // more targets.
  void ReadOnePending();
  // Commits the recorded data and notifies the caller of |Stop|.
  void FinishStop();
  // Writes a packet with the smaps of the given process.
  void SerializeSmapsForPid(pid_t pid);

  base::TaskRunner* const task_runner_;
  std::unique_ptr<TraceWriter> trace_writer_;
  const Config config_;
  // Per-instance seed for the order in which processes are considered.
  const uint64_t walk_seed_;
  // Last process selected by the previous tick.
  HashAndPid last_picked_{0, 0};
  // Processes that the current tick hasn't serialized yet, drained in separate
  // per-process tasks.
  std::vector<pid_t> pending_reads_;
  bool stopping_ = false;
  std::function<void()> on_stopped_;

  base::WeakPtrFactory<SmapsDataSource> weak_factory_;  // keep last
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_SMAPS_SMAPS_DATA_SOURCE_H_
