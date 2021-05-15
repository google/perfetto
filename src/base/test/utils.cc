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

#include "src/base/test/utils.h"

#include <stdlib.h>

#include <memory>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
#include <limits.h>
#include <unistd.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#include <io.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <mach-o/dyld.h>
#endif

namespace perfetto {
namespace base {

std::string GetCurExecutableDir() {
  std::string self_path;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  char buf[PATH_MAX];
  ssize_t size = readlink("/proc/self/exe", buf, sizeof(buf));
  PERFETTO_CHECK(size != -1);
  // readlink does not null terminate.
  self_path = std::string(buf, static_cast<size_t>(size));
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  uint32_t size = 0;
  PERFETTO_CHECK(_NSGetExecutablePath(nullptr, &size));
  self_path.resize(size);
  PERFETTO_CHECK(_NSGetExecutablePath(&self_path[0], &size) == 0);
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  char buf[MAX_PATH];
  auto len = ::GetModuleFileNameA(nullptr /*current*/, buf, sizeof(buf));
  self_path = std::string(buf, len);
  self_path = self_path.substr(0, self_path.find_last_of("\\"));
#else
  PERFETTO_FATAL(
      "GetCurExecutableDir() not implemented on the current platform");
#endif
  // Cut binary name.
  return self_path.substr(0, self_path.find_last_of("/"));
}

std::string GetTestDataPath(const std::string& path) {
  std::string self_path = GetCurExecutableDir();
  std::string full_path = self_path + "/../../" + path;
  if (FileExists(full_path))
    return full_path;
  full_path = self_path + "/" + path;
  if (FileExists(full_path))
    return full_path;
  // Fall back to relative to root dir.
  return path;
}

std::string HexDump(const void* data_void, size_t len, size_t bytes_per_line) {
  const char* data = reinterpret_cast<const char*>(data_void);
  std::string res;
  static const size_t kPadding = bytes_per_line * 3 + 12;
  std::unique_ptr<char[]> line(new char[bytes_per_line * 4 + 128]);
  for (size_t i = 0; i < len; i += bytes_per_line) {
    char* wptr = line.get();
    wptr += sprintf(wptr, "%08zX: ", i);
    for (size_t j = i; j < i + bytes_per_line && j < len; j++)
      wptr += sprintf(wptr, "%02X ", static_cast<unsigned>(data[j]) & 0xFF);
    for (size_t j = static_cast<size_t>(wptr - line.get()); j < kPadding; ++j)
      *(wptr++) = ' ';
    for (size_t j = i; j < i + bytes_per_line && j < len; j++) {
      char c = data[j];
      *(wptr++) = (c >= 32 && c < 127) ? c : '.';
    }
    *(wptr++) = '\n';
    *(wptr++) = '\0';
    res.append(line.get());
  }
  return res;
}

}  // namespace base
}  // namespace perfetto
