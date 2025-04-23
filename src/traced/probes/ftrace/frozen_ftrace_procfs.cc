/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/traced/probes/ftrace/frozen_ftrace_procfs.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"

namespace perfetto {

namespace {
bool IsDirectory(const std::string& path) {
  struct stat info;
  if (stat(path.c_str(), &info) != 0) {
    return false;
  }
  return (info.st_mode & S_IFMT) == S_IFDIR;
}
}  // namespace

// static
std::unique_ptr<FrozenFtraceProcfs>
FrozenFtraceProcfs::CreateGuessingMountPoint(
    const std::string& instance_name,
    const std::string& event_format_path) {
  std::unique_ptr<FrozenFtraceProcfs> tracefs = nullptr;

  std::string epath = event_format_path;

  if (!epath.empty()) {
    if (!IsDirectory(epath)) {
      PERFETTO_ELOG("%s is not a directory.", epath.c_str());
      return nullptr;
    }
  }

  size_t index = 0;
  while (!tracefs && FtraceProcfs::kTracingPaths[index]) {
    std::string path = FtraceProcfs::kTracingPaths[index++];
    path += "instances/" + instance_name + "/";

    // Ensure the directory exists and it's a persistent ring buffer.
    if (!CheckRootPath(path) || !CheckFrozenPath(path)) {
      PERFETTO_ILOG(
          "%s is not instance root (no trace file or no last_boot_info)",
          path.c_str());
      continue;
    }

    if (epath.empty())
      epath = path + "events/";

    tracefs = std::unique_ptr<FrozenFtraceProcfs>(
        new FrozenFtraceProcfs(path, epath));
  }
  return tracefs;
}

FrozenFtraceProcfs::FrozenFtraceProcfs(const std::string& root,
                                       const std::string& event_format_path)
    : FtraceProcfs(root), event_format_path_(event_format_path) {}

FrozenFtraceProcfs::~FrozenFtraceProcfs() = default;

std::string FrozenFtraceProcfs::ReadEventFormat(const std::string& group,
                                                const std::string& name) const {
  std::string path = event_format_path_ + "/" + group + "/" + name + "/format";
  return ReadFileIntoString(path);
}

// static
bool FrozenFtraceProcfs::CheckFrozenPath(const std::string& /*root*/) {
  return true;  // base::FileExists(root + "last_boot_info");
}
}  // namespace perfetto
