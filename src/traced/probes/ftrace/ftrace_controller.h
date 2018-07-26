/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACED_PROBES_FTRACE_FTRACE_CONTROLLER_H_
#define SRC_TRACED_PROBES_FTRACE_FTRACE_CONTROLLER_H_

#include <unistd.h>

#include <bitset>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest_prod.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/traced/data_source_types.h"
#include "src/traced/probes/ftrace/ftrace_sink.h"

namespace perfetto {

namespace protos {
namespace pbzero {
class FtraceEventBundle;
class FtraceStats;
class FtraceCpuStats;
}  // namespace pbzero
}  // namespace protos

struct FtraceCpuStats {
  uint64_t cpu;
  uint64_t entries;
  uint64_t overrun;
  uint64_t commit_overrun;
  uint64_t bytes_read;
  double oldest_event_ts;
  double now_ts;
  uint64_t dropped_events;
  uint64_t read_events;

  void Write(protos::pbzero::FtraceCpuStats*) const;
};

struct FtraceStats {
  std::vector<FtraceCpuStats> cpu_stats;

  void Write(protos::pbzero::FtraceStats*) const;
};

constexpr size_t kMaxSinks = 32;
constexpr size_t kMaxCpus = 64;

// Method of last resort to reset ftrace state.
void HardResetFtraceState();

class CpuReader;
class EventFilter;
class FtraceConfig;
class FtraceController;
class FtraceConfigMuxer;
class FtraceProcfs;
class ProtoTranslationTable;

// Utility class for controlling ftrace.
class FtraceController {
 public:
  static std::unique_ptr<FtraceController> Create(base::TaskRunner*);
  virtual ~FtraceController();

  std::unique_ptr<FtraceSink> CreateSink(FtraceConfig, FtraceSink::Delegate*);

  void DisableAllEvents();
  void WriteTraceMarker(const std::string& s);
  void ClearTrace();

 protected:
  // Protected for testing.
  FtraceController(std::unique_ptr<FtraceProcfs>,
                   std::unique_ptr<ProtoTranslationTable>,
                   std::unique_ptr<FtraceConfigMuxer>,
                   base::TaskRunner*);

  // Write
  void DumpFtraceStats(FtraceStats*);

  // Called to read data from the staging pipe for the given |cpu| and parse it
  // into the sinks. Protected and virtual for testing.
  virtual void OnRawFtraceDataAvailable(size_t cpu);

  // Protected and virtual for testing.
  virtual uint64_t NowMs() const;

 private:
  friend FtraceSink;
  friend class TestFtraceController;
  FRIEND_TEST(FtraceControllerIntegrationTest, EnableDisableEvent);

  FtraceController(const FtraceController&) = delete;
  FtraceController& operator=(const FtraceController&) = delete;

  // Called on a worker thread when |cpu| has at least one page of data
  // available for reading.
  void OnDataAvailable(base::WeakPtr<FtraceController>,
                       size_t generation,
                       size_t cpu,
                       uint32_t drain_period_ms);

  static void DrainCPUs(base::WeakPtr<FtraceController>, size_t generation);
  static void UnblockReaders(const base::WeakPtr<FtraceController>&);

  uint32_t GetDrainPeriodMs();

  void Register(FtraceSink*);
  void Unregister(FtraceSink*);

  void StartIfNeeded();
  void StopIfNeeded();

  // Begin lock-protected members.
  std::mutex lock_;
  std::condition_variable data_drained_;
  std::bitset<kMaxCpus> cpus_to_drain_;
  bool listening_for_raw_trace_data_ = false;
  // End lock-protected members.

  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
  std::unique_ptr<ProtoTranslationTable> table_;
  std::unique_ptr<FtraceConfigMuxer> ftrace_config_muxer_;
  size_t generation_ = 0;
  bool atrace_running_ = false;
  base::TaskRunner* task_runner_ = nullptr;
  std::map<size_t, std::unique_ptr<CpuReader>> cpu_readers_;
  std::set<FtraceSink*> sinks_;
  base::WeakPtrFactory<FtraceController> weak_factory_;
  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_CONTROLLER_H_
