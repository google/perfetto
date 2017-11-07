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

#include "ftrace_reader/ftrace_controller.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <string>

#include "base/scoped_file.h"
#include "base/utils.h"

namespace perfetto {

namespace {

// TODO(b/68242551): Do not hardcode these paths.
// This directory contains the 'format' and 'enable' files for each event.
// These are nested like so: group_name/event_name/{format, enable}
const char kTraceEventPath[] = "/sys/kernel/debug/tracing/events/";

// Reading this file produces human readable trace output.
// Writing to this file clears all trace buffers for all CPUS.
const char kTracePath[] = "/sys/kernel/debug/tracing/trace";

// Writing to this file injects an event into the trace buffer.
const char kTraceMarkerPath[] = "/sys/kernel/debug/tracing/trace_marker";

// Reading this file returns 1/0 if tracing is enabled/disabled.
// Writing 1/0 to this file enables/disables tracing.
// Disabling tracing with this file prevents further writes but
// does not clear the buffer.
const char kTracingOnPath[] = "/sys/kernel/debug/tracing/tracing_on";

bool WriteToFile(const std::string& path, const std::string& str) {
  base::ScopedFile fd(open(path.c_str(), O_WRONLY));
  if (!fd)
    return false;
  ssize_t written = PERFETTO_EINTR(write(fd.get(), str.c_str(), str.length()));
  ssize_t length = static_cast<ssize_t>(str.length());
  // This should either fail or write fully.
  PERFETTO_DCHECK(written == length || written == -1);
  return written == length;
}

char ReadOneCharFromFile(const std::string& path) {
  base::ScopedFile fd(open(path.c_str(), O_RDONLY));
  if (!fd)
    return '\0';
  char result = '\0';
  ssize_t bytes = PERFETTO_EINTR(read(fd.get(), &result, 1));
  PERFETTO_DCHECK(bytes == 1 || bytes == -1);
  return result;
}

}  // namespace

FtraceController::FtraceController() {}

void FtraceController::ClearTrace() {
  base::ScopedFile fd(open(kTracePath, O_WRONLY | O_TRUNC));
  PERFETTO_CHECK(fd);  // Could not clear.
}

bool FtraceController::WriteTraceMarker(const std::string& str) {
  return WriteToFile(kTraceMarkerPath, str);
}

bool FtraceController::EnableTracing() {
  return WriteToFile(kTracingOnPath, "1");
}

bool FtraceController::DisableTracing() {
  return WriteToFile(kTracingOnPath, "0");
}

bool FtraceController::IsTracingEnabled() {
  return ReadOneCharFromFile(kTracingOnPath) == '1';
}

bool FtraceController::EnableEvent(const std::string& name) {
  std::string path = std::string(kTraceEventPath) + name + "/enable";
  return WriteToFile(path, "1");
}

bool FtraceController::DisableEvent(const std::string& name) {
  std::string path = std::string(kTraceEventPath) + name + "/enable";
  return WriteToFile(path, "0");
}

}  // namespace perfetto
