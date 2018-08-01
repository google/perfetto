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

namespace {
int MaybeOpenTraceFile() {
  static const char* tracing_path = getenv("PERFETTO_METATRACE_FILE");
  if (tracing_path == nullptr)
    return -1;
  static int fd = open(tracing_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  return fd;
}
}  // namespace

void MetaTrace::WriteEvent(char type, const char* evt_name, size_t cpu) {
  int fd = MaybeOpenTraceFile();
  if (fd == -1)
    return;

  char json[256];
  int len = sprintf(json,
                    "{\"ts\": %f, \"cat\": \"PERF\", \"ph\": \"%c\", \"name\": "
                    "\"%s\", \"pid\": %zu},\n",
                    GetWallTimeNs().count() / 1000.0, type, evt_name, cpu);
  ignore_result(write(fd, json, static_cast<size_t>(len)));
}

}  // namespace base
}  // namespace perfetto
