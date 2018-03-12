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

#include "src/traced/probes/filesystem/fs_mount.h"

#include <sys/types.h>
#include <unistd.h>
#include <fstream>
#include <sstream>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"

namespace perfetto {

namespace {
constexpr const char kMountsPath[] = "/proc/mounts";

std::vector<std::string> split(const std::string& text, char s) {
  std::vector<std::string> result;
  size_t start = 0;
  size_t end = 0;
  do {
    end = text.find(s, start);
    if (end == std::string::npos)
      end = text.size();
    std::string sub = text.substr(start, end - start);
    if (!sub.empty())
      result.emplace_back(std::move(sub));
    start = end + 1;
  } while (start < text.size());
  return result;
}
}  // namespace

std::multimap<BlockDeviceID, std::string> ParseMounts() {
  std::string data;
  if (!base::ReadFile(kMountsPath, &data)) {
    PERFETTO_ELOG("Failed to read %s.", kMountsPath);
    return {};
  }
  std::multimap<BlockDeviceID, std::string> device_to_mountpoints;
  std::vector<std::string> lines = split(data, '\n');
  struct stat buf {};
  for (const std::string& line : lines) {
    std::vector<std::string> words = split(line, ' ');
    if (words.size() < 2) {
      PERFETTO_DLOG("Encountered incomplete row in %s: %s.", kMountsPath,
                    line.c_str());
      continue;
    }
    std::string& mountpoint = words[1];
    if (stat(mountpoint.c_str(), &buf) == -1) {
      PERFETTO_PLOG("stat");
      continue;
    }
    device_to_mountpoints.emplace(buf.st_dev, std::move(mountpoint));
  }
  return device_to_mountpoints;
}

}  // namespace perfetto
