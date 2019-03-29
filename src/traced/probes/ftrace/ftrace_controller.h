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

#include <stdint.h>
#include <unistd.h>

#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "perfetto/base/gtest_prod_util.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/utils.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/tracing/core/basic_types.h"
#include "src/traced/probes/ftrace/ftrace_config.h"
#include "src/traced/probes/ftrace/ftrace_thread_sync.h"

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
  static const char* const kTracingPaths[];

  class Observer {
   public:
    virtual ~Observer();
    virtual void OnFtraceDataWrittenIntoDataSourceBuffers() = 0;
  };

  // The passed Observer must outlive the returned FtraceController instance.
  static std::unique_ptr<FtraceController> Create(base::TaskRunner*, Observer*);
  virtual ~FtraceController();

  // These two methods are called by CpuReader(s) from their worker threads.
  static void OnCpuReaderRead(size_t cpu, int generation, FtraceThreadSync*);
  static void OnCpuReaderFlush(size_t cpu, int generation, FtraceThreadSync*);

  void DisableAllEvents();
  void WriteTraceMarker(const std::string& s);
  void ClearTrace();

  bool AddDataSource(FtraceDataSource*) PERFETTO_WARN_UNUSED_RESULT;
  bool StartDataSource(FtraceDataSource*);
  void RemoveDataSource(FtraceDataSource*);

  // Force a read of the ftrace buffers, including kernel buffer pages that
  // are not full. Will call OnFtraceFlushComplete() on all
  // |started_data_sources_| once all workers have flushed (or timed out).
  void Flush(FlushRequestID);

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

  void OnFlushTimeout(FlushRequestID);
  void DrainCPUs(int generation);
  void UnblockReaders();
  void NotifyFlushCompleteToStartedDataSources(FlushRequestID);
  void IssueThreadSyncCmd(FtraceThreadSync::Cmd,
                          std::unique_lock<std::mutex> = {});

  uint32_t GetDrainPeriodMs();

  void StartIfNeeded();
  void StopIfNeeded();

  base::TaskRunner* const task_runner_;
  Observer* const observer_;
  FtraceThreadSync thread_sync_;
  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
  std::unique_ptr<ProtoTranslationTable> table_;
  std::unique_ptr<FtraceConfigMuxer> ftrace_config_muxer_;
  int generation_ = 0;
  FlushRequestID cur_flush_request_id_ = 0;
  bool atrace_running_ = false;
  std::vector<std::unique_ptr<CpuReader>> cpu_readers_;
  std::set<FtraceDataSource*> data_sources_;
  std::set<FtraceDataSource*> started_data_sources_;
  PERFETTO_THREAD_CHECKER(thread_checker_)
  base::WeakPtrFactory<FtraceController> weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_CONTROLLER_H_
