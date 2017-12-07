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

#ifndef SRC_FTRACE_READER_FTRACE_PROCFS_H_
#define SRC_FTRACE_READER_FTRACE_PROCFS_H_

#include <string>

#include "perfetto/base/scoped_file.h"

namespace perfetto {

class FtraceProcfs {
 public:
  FtraceProcfs(const std::string& root);
  virtual ~FtraceProcfs();

  // Enable the event under with the given |group| and |name|.
  bool EnableEvent(const std::string& group, const std::string& name);

  // Disable the event under with the given |group| and |name|.
  bool DisableEvent(const std::string& group, const std::string& name);

  // Disable all events by writing to the global enable file.
  bool DisableAllEvents();

  // Read the format for event with the given |group| and |name|.
  std::string ReadEventFormat(const std::string& group,
                              const std::string& name) const;

  // Read the available_events file.
  std::string ReadAvailableEvents() const;

  // Returns the number of CPUs.
  // This will match the number of tracing/per_cpu/cpuXX directories.
  size_t virtual NumberOfCpus() const;

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

  virtual base::ScopedFile OpenPipeForCpu(size_t cpu);
  virtual bool WriteToFile(const std::string& path, const std::string& str);

 private:
  const std::string root_;
};

}  // namespace perfetto

#endif  // SRC_FTRACE_READER_FTRACE_PROCFS_H_
