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

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest_prod.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/protozero/protozero_message_handle.h"

namespace perfetto {

namespace protos {
namespace pbzero {
class FtraceEventBundle;
}  // namespace pbzero
}  // namespace protos

const size_t kMaxSinks = 32;

class FtraceController;
class ProtoTranslationTable;
class CpuReader;
class FtraceProcfs;
class EventFilter;

class FtraceConfig {
 public:
  explicit FtraceConfig(std::set<std::string> events);
  FtraceConfig();
  ~FtraceConfig();

  void AddEvent(const std::string&);
  void AddAtraceApp(const std::string& app);
  void AddAtraceCategory(const std::string&);
  bool RequiresAtrace() const;

  const std::set<std::string>& events() const { return ftrace_events_; }
  const std::set<std::string>& atrace_categories() const {
    return atrace_categories_;
  }
  const std::set<std::string>& atrace_apps() const { return atrace_apps_; }

  uint32_t buffer_size_kb() const { return buffer_size_kb_; }
  uint32_t drain_period_ms() const { return drain_period_ms_; }
  void set_buffer_size_kb(uint32_t v) { buffer_size_kb_ = v; }
  void set_drain_period_ms(uint32_t v) { drain_period_ms_ = v; }

 private:
  std::set<std::string> ftrace_events_;
  std::set<std::string> atrace_categories_;
  std::set<std::string> atrace_apps_;
  uint32_t buffer_size_kb_ = 0;
  uint32_t drain_period_ms_ = 0;
};

// To consume ftrace data clients implement a |FtraceSink::Delegate| and use it
// to create a |FtraceSink|. While the FtraceSink lives FtraceController will
// call |GetBundleForCpu|, write data into the bundle then call
// |OnBundleComplete| allowing the client to perform finalization.
class FtraceSink {
 public:
  using FtraceEventBundle = protos::pbzero::FtraceEventBundle;
  class Delegate {
   public:
    virtual protozero::ProtoZeroMessageHandle<FtraceEventBundle>
        GetBundleForCpu(size_t) = 0;
    virtual void OnBundleComplete(
        size_t,
        protozero::ProtoZeroMessageHandle<FtraceEventBundle>) = 0;
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
  protozero::ProtoZeroMessageHandle<FtraceEventBundle> GetBundleForCpu(
      size_t cpu) {
    return delegate_->GetBundleForCpu(cpu);
  }
  void OnBundleComplete(
      size_t cpu,
      protozero::ProtoZeroMessageHandle<FtraceEventBundle> bundle) {
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

  std::unique_ptr<FtraceSink> CreateSink(const FtraceConfig&,
                                         FtraceSink::Delegate*);

  void DisableAllEvents();
  void WriteTraceMarker(const std::string& s);
  void ClearTrace();

 protected:
  // Protected for testing.
  FtraceController(std::unique_ptr<FtraceProcfs>,
                   base::TaskRunner*,
                   std::unique_ptr<ProtoTranslationTable>);

  // Called to read  data from the raw pipe
  // for the given |cpu|. Kicks off the reading/parsing
  // of the pipe. Returns true if there is probably more to read.
  // Protected and virtual for testing.
  virtual bool OnRawFtraceDataAvailable(size_t cpu);

 private:
  friend FtraceSink;
  FRIEND_TEST(FtraceControllerIntegrationTest, EnableDisableEvent);

  FtraceController(const FtraceController&) = delete;
  FtraceController& operator=(const FtraceController&) = delete;

  static void PeriodicDrainCPU(base::WeakPtr<FtraceController>,
                               size_t generation,
                               int cpu);

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

  // Returns a cached CpuReader for |cpu|.
  // CpuReaders are constructed lazily and owned by the controller.
  CpuReader* GetCpuReader(size_t cpu);

  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
  size_t generation_ = 0;
  bool listening_for_raw_trace_data_ = false;
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
