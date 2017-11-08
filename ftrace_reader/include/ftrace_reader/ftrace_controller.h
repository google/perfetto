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

#include <string>
#include <vector>

namespace perfetto {

// Utility class for controling ftrace.
class FtraceController {
 public:
  FtraceController();

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

  // Enable the event |name|.
  bool EnableEvent(const std::string& name);

  // Disable the event |name|.
  bool DisableEvent(const std::string& name);

 private:
  FtraceController(const FtraceController&) = delete;
  FtraceController& operator=(const FtraceController&) = delete;
};

}  // namespace perfetto

#endif  // FTRACE_READER_FTRACE_CONTROLLER_H_
