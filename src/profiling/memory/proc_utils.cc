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

#include "src/profiling/memory/proc_utils.h"

#include <sys/stat.h>
#include <unistd.h>

#include "perfetto/base/file_utils.h"

namespace perfetto {
namespace profiling {
namespace {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)

const char* FindChar(const char* s, char c, size_t n) {
  std::string str(s, n);
  auto idx = str.rfind(c);
  if (idx == std::string::npos)
    return nullptr;
  return s + n;
}

void* memrchr(const void* s, int c, size_t n) {
  return static_cast<void*>(const_cast<char*>(
      FindChar(static_cast<const char*>(s), static_cast<char>(c), n)));
}

#endif

bool GetProcFile(pid_t pid, const char* file, char* filename_buf, size_t size) {
  ssize_t written = snprintf(filename_buf, size, "/proc/%d/%s", pid, file);
  if (written < 0 || static_cast<size_t>(written) >= size) {
    if (written < 0)
      PERFETTO_ELOG("Failed to concatenate cmdline file.");
    else
      PERFETTO_ELOG("Overflow when concatenating cmdline file.");
    return false;
  }
  return true;
}

}  // namespace

bool NormalizeCmdLine(char* cmdline, size_t size, std::string* name) {
  char* first_arg = static_cast<char*>(memchr(cmdline, '\0', size));
  if (first_arg == nullptr) {
    PERFETTO_DLOG("Overflow reading cmdline");
    return false;
  }
  // For consistency with what we do with Java app cmdlines, trim everything
  // after the @ sign of the first arg.
  char* first_at = static_cast<char*>(memchr(cmdline, '@', size));
  if (first_at != nullptr && first_at < first_arg) {
    *first_at = '\0';
    first_arg = first_at;
  }
  char* start = static_cast<char*>(
      memrchr(cmdline, '/', static_cast<size_t>(first_arg - cmdline)));
  if (start == first_arg) {
    // The first argument ended in a slash.
    PERFETTO_DLOG("cmdline ends in /");
    return false;
  } else if (start == nullptr) {
    start = cmdline;
  } else {
    // Skip the /.
    start++;
  }
  size_t name_size = static_cast<size_t>(first_arg - start);
  name->assign(start, name_size);
  return true;
}

std::vector<std::string> NormalizeCmdlines(
    const std::vector<std::string>& cmdlines) {
  std::vector<std::string> normalized_cmdlines;
  for (std::string cmdline : cmdlines) {
    // Add nullbyte to make sure it's a C string.
    cmdline.resize(cmdline.size() + 1, '\0');
    std::string normalized;
    if (!NormalizeCmdLine(&(cmdline[0]), cmdline.size(), &normalized)) {
      PERFETTO_ELOG("Failed to normalize cmdline %s. Skipping.",
                    cmdline.c_str());
      continue;
    }
    normalized_cmdlines.emplace_back(std::move(normalized));
  }
  return normalized_cmdlines;
}

// This is mostly the same as GetHeapprofdProgramProperty in
// https://android.googlesource.com/platform/bionic/+/master/libc/bionic/malloc_common.cpp
// This should give the same result as GetHeapprofdProgramProperty.
bool GetCmdlineForPID(pid_t pid, std::string* name) {
  std::string filename = "/proc/" + std::to_string(pid) + "/cmdline";
  base::ScopedFile fd(base::OpenFile(filename, O_RDONLY | O_CLOEXEC));
  if (!fd) {
    PERFETTO_DLOG("Failed to open %s", filename.c_str());
    return false;
  }
  char cmdline[128];
  ssize_t rd = read(*fd, cmdline, sizeof(cmdline) - 1);
  if (rd == -1) {
    PERFETTO_DLOG("Failed to read %s", filename.c_str());
    return false;
  }
  if (rd == sizeof(cmdline) - 1) {
    PERFETTO_DLOG("Overflow reading cmdline");
    return false;
  }
  cmdline[rd] = '\0';
  return NormalizeCmdLine(cmdline, static_cast<size_t>(rd), name);
}

void FindAllProfilablePids(std::set<pid_t>* pids) {
  ForEachPid([pids](pid_t pid) {
    if (pid == getpid())
      return;

    char filename_buf[128];
    if (!GetProcFile(pid, "cmdline", filename_buf, sizeof(filename_buf)))
      return;
    struct stat statbuf;
    // Check if we have permission to the process.
    if (stat(filename_buf, &statbuf) == 0)
      pids->emplace(pid);
  });
}

void FindPidsForCmdlines(const std::vector<std::string>& cmdlines,
                         std::set<pid_t>* pids) {
  ForEachPid([&cmdlines, pids](pid_t pid) {
    if (pid == getpid())
      return;
    std::string process_cmdline;
    process_cmdline.reserve(128);
    GetCmdlineForPID(pid, &process_cmdline);
    for (const std::string& cmdline : cmdlines) {
      if (process_cmdline == cmdline)
        pids->emplace(static_cast<pid_t>(pid));
    }
  });
}

}  // namespace profiling
}  // namespace perfetto
