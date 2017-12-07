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
  FtraceConfig();
  explicit FtraceConfig(std::set<std::string> events);
  ~FtraceConfig();

  void AddEvent(const std::string&);

  const std::set<std::string>& events() const { return events_; }

 private:
  std::set<std::string> events_;
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
             std::unique_ptr<EventFilter>,
             Delegate*);
  ~FtraceSink();

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
  std::unique_ptr<EventFilter> filter_;
  FtraceSink::Delegate* delegate_;
};

// Utility class for controlling ftrace.
class FtraceController {
 public:
  static std::unique_ptr<FtraceController> Create(base::TaskRunner*);
  virtual ~FtraceController();

  std::unique_ptr<FtraceSink> CreateSink(FtraceConfig, FtraceSink::Delegate*);

 protected:
  // Protected for testing.
  FtraceController(std::unique_ptr<FtraceProcfs>,
                   base::TaskRunner*,
                   std::unique_ptr<ProtoTranslationTable>);

 private:
  friend FtraceSink;
  FRIEND_TEST(FtraceControllerIntegrationTest, EnableDisableEvent);

  FtraceController(const FtraceController&) = delete;
  FtraceController& operator=(const FtraceController&) = delete;

  void Register(FtraceSink*);
  void Unregister(FtraceSink*);
  void RegisterForEvent(const std::string& event_name);
  void UnregisterForEvent(const std::string& event_name);

  void StartIfNeeded();
  void StopIfNeeded();

  // Called when we know there is data for the raw pipe
  // for the given |cpu|. Kicks off the reading/parsing
  // of the pipe.
  void OnRawFtraceDataAvailable(size_t cpu);

  // Returns a cached CpuReader for |cpu|.
  // CpuReaders are constructed lazily and owned by the controller.
  CpuReader* GetCpuReader(size_t cpu);

  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
  bool listening_for_raw_trace_data_ = false;
  base::TaskRunner* task_runner_ = nullptr;
  base::WeakPtrFactory<FtraceController> weak_factory_;
  std::vector<size_t> enabled_count_;
  std::unique_ptr<ProtoTranslationTable> table_;
  std::map<size_t, std::unique_ptr<CpuReader>> readers_;
  std::set<FtraceSink*> sinks_;
  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_FTRACE_READER_FTRACE_CONTROLLER_H_
