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

#ifndef FTRACE_READER_FTRACE_CONTROLLER_H_
#define FTRACE_READER_FTRACE_CONTROLLER_H_

#include <unistd.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/task_runner.h"
#include "base/weak_ptr.h"
#include "ftrace_reader/ftrace_cpu_reader.h"
#include "protozero/protozero_message_handle.h"

#include "protos/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto {

class FtraceController;

class FtraceConfig {
 public:
  FtraceConfig();
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
  class Delegate {
   public:
    virtual protozero::ProtoZeroMessageHandle<pbzero::FtraceEventBundle>
        GetBundleForCpu(size_t) = 0;
    virtual void OnBundleComplete(
        int,
        protozero::ProtoZeroMessageHandle<pbzero::FtraceEventBundle>) = 0;
    virtual ~Delegate();
  };

  // TODO(hjd): Make private.
  const std::set<std::string>& enabled_events() { return config_.events(); }
  FtraceSink(base::WeakPtr<FtraceController>, FtraceConfig);
  ~FtraceSink();

 private:
  base::WeakPtr<FtraceController> controller_weak_;
  FtraceConfig config_;
};

// Utility class for controlling ftrace.
class FtraceController {
 public:
  static std::unique_ptr<FtraceController> Create(base::TaskRunner*);
  ~FtraceController();

  std::unique_ptr<FtraceSink> CreateSink(FtraceConfig, FtraceSink::Delegate*);

  // Clears the trace buffers for all CPUs. Blocks until this is done.
  void ClearTrace();

  // Writes the string |str| as an event into the trace buffer.
  bool WriteTraceMarker(const std::string& str);

  // Enable tracing.
  bool EnableTracing();

  // Disables tracing, does not clear the buffer.
  bool DisableTracing();

  // Returns true iff tracing is enabled.
  // Necessarily racy: another program could enable/disable tracing at any
  // point.
  bool IsTracingEnabled();

  // Returns a cached FtraceCpuReader for |cpu|.
  // FtraceCpuReaders are constructed lazily.
  FtraceCpuReader* GetCpuReader(size_t cpu);

  // Returns the number of CPUs.
  // This will match the number of tracing/per_cpu/cpuXX directories.
  size_t NumberOfCpus() const;

 private:
  friend FtraceSink;
  FRIEND_TEST(FtraceControllerIntegrationTest, EnableDisableEvent);

  FtraceController(base::TaskRunner* runner,
                   std::unique_ptr<FtraceToProtoTranslationTable>);
  FtraceController(const FtraceController&) = delete;
  FtraceController& operator=(const FtraceController&) = delete;

  void Register(FtraceSink*);
  void Unregister(FtraceSink*);
  void RegisterForEvent(const std::string& event_name);
  void UnregisterForEvent(const std::string& event_name);

  // Enable the event under with the given |group| and |name|.
  bool EnableEvent(const std::string& group, const std::string& name);
  // Disable the event under with the given |group| and |name|.
  bool DisableEvent(const std::string& group, const std::string& name);

  base::TaskRunner* task_runner_;
  base::WeakPtrFactory<FtraceController> weak_factory_;
  std::vector<size_t> enabled_count_;
  std::unique_ptr<FtraceToProtoTranslationTable> table_;
  std::map<size_t, FtraceCpuReader> readers_;
  std::set<FtraceSink*> sinks_;
  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto

#endif  // FTRACE_READER_FTRACE_CONTROLLER_H_
