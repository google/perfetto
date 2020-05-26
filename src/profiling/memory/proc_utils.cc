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

#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/profiling/normalize.h"

namespace perfetto {
namespace profiling {
namespace {

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

std::vector<std::string> NormalizeCmdlines(
    const std::vector<std::string>& cmdlines) {
  std::vector<std::string> normalized_cmdlines;
  for (std::string cmdline : cmdlines) {
    // Add nullbyte to make sure it's a C string.
    cmdline.resize(cmdline.size() + 1, '\0');
    std::string normalized;
    char* cmdline_cstr = &(cmdline[0]);
    ssize_t size = NormalizeCmdLine(&cmdline_cstr, cmdline.size());
    if (size == -1) {
      PERFETTO_PLOG("Failed to normalize cmdline %s. Skipping.",
                    cmdline.c_str());
      continue;
    }
    normalized_cmdlines.emplace_back(
        std::string(cmdline_cstr, static_cast<size_t>(size)));
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
    // We do not expect errors other than permission errors here.
    if (errno != EPERM && errno != EACCES)
      PERFETTO_PLOG("Failed to open %s", filename.c_str());
    else
      PERFETTO_DPLOG("Failed to open %s", filename.c_str());
    return false;
  }
  char cmdline[512];
  ssize_t rd = read(*fd, cmdline, sizeof(cmdline) - 1);
  if (rd == -1) {
    PERFETTO_DPLOG("Failed to read %s", filename.c_str());
    return false;
  }

  if (rd == 0) {
    PERFETTO_DLOG("Empty cmdline for %" PRIdMAX ". Skipping.",
                  static_cast<intmax_t>(pid));
    return false;
  }

  // We did not manage to read the first argument.
  if (memchr(cmdline, '\0', static_cast<size_t>(rd)) == nullptr) {
    PERFETTO_DLOG("Overflow reading cmdline for %" PRIdMAX,
                  static_cast<intmax_t>(pid));
    errno = EOVERFLOW;
    return false;
  }

  cmdline[rd] = '\0';
  char* cmdline_start = cmdline;
  ssize_t size = NormalizeCmdLine(&cmdline_start, static_cast<size_t>(rd));
  if (size == -1)
    return false;
  name->assign(cmdline_start, static_cast<size_t>(size));
  return true;
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
    process_cmdline.reserve(512);
    GetCmdlineForPID(pid, &process_cmdline);
    for (const std::string& cmdline : cmdlines) {
      if (process_cmdline == cmdline)
        pids->emplace(static_cast<pid_t>(pid));
    }
  });
}

}  // namespace profiling
}  // namespace perfetto
