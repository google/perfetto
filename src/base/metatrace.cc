/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/base/metatrace.h"

#include <fcntl.h>
#include <stdlib.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/time.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <corecrt_io.h>
#endif

namespace perfetto {
namespace base {

int MaybeOpenTraceFile() {
  static const char* tracing_path = getenv("PERFETTO_METATRACE_FILE");
  if (tracing_path == nullptr)
    return -1;
  static int fd = open(tracing_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  return fd;
}

template <>
std::string FormatJSON<std::string>(std::string value) {
  return "\"" + value + "\"";
}

template <>
std::string FormatJSON<const char*>(const char* value) {
  return std::string("\"") + value + "\"";
}

void MetaTrace::WriteEvent(std::string type) {
  int fd = MaybeOpenTraceFile();
  if (fd == -1)
    return;

  std::string data = "{";
  data.reserve(128);
  for (size_t i = 0; i < trace_.size(); ++i) {
    const std::pair<std::string, std::string>& p = trace_[i];
    data += p.first;
    data += ": ";
    data += p.second;
    data += ", ";
  }
  data += "\"ts\": " + std::to_string(GetWallTimeNs().count() / 1000.) +
          ", \"cat\": \"PERF\", \"ph\": \"" + type + "\"},\n";
  ignore_result(write(fd, data.c_str(), data.size()));
}

}  // namespace base
}  // namespace perfetto
