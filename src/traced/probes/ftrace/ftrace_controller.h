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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "gtest/gtest_prod.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/utils.h"
#include "perfetto/base/weak_ptr.h"
#include "src/traced/probes/ftrace/ftrace_config.h"

namespace perfetto {

class CpuReader;
class FtraceConfigMuxer;
class FtraceDataSource;
class FtraceProcfs;
class ProtoTranslationTable;
struct FtraceStats;

// Method of last resort to reset ftrace state.
void HardResetFtraceState();

// Utility class for controlling ftrace.
class FtraceController {
 public:
  class Observer {
   public:
    virtual ~Observer();
    virtual void OnFtraceDataWrittenIntoDataSourceBuffers() = 0;
  };

  // The passed Observer must outlive the returned FtraceController instance.
  static std::unique_ptr<FtraceController> Create(base::TaskRunner*, Observer*);
  virtual ~FtraceController();

  void DisableAllEvents();
  void WriteTraceMarker(const std::string& s);
  void ClearTrace();

  bool AddDataSource(FtraceDataSource*) PERFETTO_WARN_UNUSED_RESULT;
  void RemoveDataSource(FtraceDataSource*);

  void DumpFtraceStats(FtraceStats*);

  base::WeakPtr<FtraceController> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 protected:
  // Protected for testing.
  FtraceController(std::unique_ptr<FtraceProcfs>,
                   std::unique_ptr<ProtoTranslationTable>,
                   std::unique_ptr<FtraceConfigMuxer>,
                   base::TaskRunner*,
                   Observer*);

  virtual void OnDrainCpuForTesting(size_t /*cpu*/) {}

  // Protected and virtual for testing.
  virtual uint64_t NowMs() const;

 private:
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

  void StartIfNeeded();
  void StopIfNeeded();

  // Begin lock-protected members.
  std::mutex lock_;
  std::condition_variable data_drained_;
  std::bitset<base::kMaxCpus> cpus_to_drain_;
  bool listening_for_raw_trace_data_ = false;
  // End lock-protected members.

  base::TaskRunner* const task_runner_;
  Observer* const observer_;
  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
  std::unique_ptr<ProtoTranslationTable> table_;
  std::unique_ptr<FtraceConfigMuxer> ftrace_config_muxer_;
  size_t generation_ = 0;
  bool atrace_running_ = false;
  std::map<size_t, std::unique_ptr<CpuReader>> cpu_readers_;
  std::set<FtraceDataSource*> data_sources_;
  base::WeakPtrFactory<FtraceController> weak_factory_;  // Keep last.
  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_CONTROLLER_H_
