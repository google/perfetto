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

#ifndef INCLUDE_PERFETTO_FTRACE_READER_FTRACE_CONTROLLER_H_
#define INCLUDE_PERFETTO_FTRACE_READER_FTRACE_CONTROLLER_H_

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
#include "perfetto/ftrace_reader/ftrace_config.h"
#include "perfetto/protozero/protozero_message_handle.h"

namespace perfetto {

namespace protos {
namespace pbzero {
class FtraceEventBundle;
}  // namespace pbzero
}  // namespace protos

const size_t kMaxSinks = 32;
const size_t kMaxCpus = 64;

// Method of last resort to reset ftrace state.
void HardResetFtraceState();

class FtraceController;
class ProtoTranslationTable;
class CpuReader;
class FtraceProcfs;
class EventFilter;

// To consume ftrace data clients implement a |FtraceSink::Delegate| and use it
// to create a |FtraceSink|. While the FtraceSink lives FtraceController will
// call |GetBundleForCpu|, write data into the bundle then call
// |OnBundleComplete| allowing the client to perform finalization.
class FtraceSink {
 public:
  using FtraceEventBundle = protos::pbzero::FtraceEventBundle;
  class Delegate {
   public:
    virtual protozero::MessageHandle<FtraceEventBundle> GetBundleForCpu(
        size_t) = 0;
    virtual void OnBundleComplete(
        size_t,
        protozero::MessageHandle<FtraceEventBundle>) = 0;
    virtual ~Delegate() = default;
  };

  FtraceSink(base::WeakPtr<FtraceController>,
             FtraceConfig config,
             std::unique_ptr<EventFilter>,
             Delegate*);
  ~FtraceSink();

  const FtraceConfig& config() const { return config_; }

 private:
  friend FtraceController;

  EventFilter* get_event_filter() { return filter_.get(); }
  protozero::MessageHandle<FtraceEventBundle> GetBundleForCpu(size_t cpu) {
    return delegate_->GetBundleForCpu(cpu);
  }
  void OnBundleComplete(size_t cpu,
                        protozero::MessageHandle<FtraceEventBundle> bundle) {
    delegate_->OnBundleComplete(cpu, std::move(bundle));
  }

  const std::set<std::string>& enabled_events();
  base::WeakPtr<FtraceController> controller_weak_;
  FtraceConfig config_;
  std::unique_ptr<EventFilter> filter_;
  FtraceSink::Delegate* delegate_;
};

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
                   base::TaskRunner*,
                   std::unique_ptr<ProtoTranslationTable>);

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
  static void UnblockReaders(base::WeakPtr<FtraceController>);

  uint32_t GetDrainPeriodMs();
  uint32_t GetCpuBufferSizeInPages();

  void Register(FtraceSink*);
  void Unregister(FtraceSink*);
  void RegisterForEvent(const std::string& event_name);
  void UnregisterForEvent(const std::string& event_name);

  void StartAtrace(const FtraceConfig&);
  void StopAtrace();

  void StartIfNeeded();
  void StopIfNeeded();

  // Begin lock-protected members.
  std::mutex lock_;
  std::condition_variable data_drained_;
  std::bitset<kMaxCpus> cpus_to_drain_;
  bool listening_for_raw_trace_data_ = false;
  // End lock-protected members.

  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
  size_t generation_ = 0;
  bool atrace_running_ = false;
  base::TaskRunner* task_runner_ = nullptr;
  std::vector<size_t> enabled_count_;
  std::unique_ptr<ProtoTranslationTable> table_;
  std::map<size_t, std::unique_ptr<CpuReader>> readers_;
  std::set<FtraceSink*> sinks_;
  base::WeakPtrFactory<FtraceController> weak_factory_;
  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_FTRACE_READER_FTRACE_CONTROLLER_H_
