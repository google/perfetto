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

#include "perfetto/ext/base/proc_utils.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::base {
std::optional<std::vector<std::string> > SplitProcStatString(
    const std::string& proc_stat_string) {
  const size_t comm_start = proc_stat_string.find_first_of('(');
  const size_t comm_end = proc_stat_string.find_last_of(')');
  if (comm_start == std::string::npos || comm_end == std::string::npos) {
    return std::nullopt;
  }
  if (comm_end <= comm_start) {
    return std::nullopt;
  }
  if (comm_start <= 1) {
    return std::nullopt;
  }
  const size_t pid_end_exclusive = comm_start - 1;
  const size_t rest_of_string_start = comm_end + 2;
  std::string pid = proc_stat_string.substr(0, pid_end_exclusive);
  std::string comm =
      proc_stat_string.substr(comm_start, comm_end - comm_start + 1);
  if (rest_of_string_start >= proc_stat_string.size()) {
    return std::nullopt;
  }
  const std::string rest_of_string =
      proc_stat_string.substr(rest_of_string_start);
  std::vector<std::string> rest_parts = SplitString(rest_of_string, " ");
  std::vector<std::string> result;
  result.reserve(rest_parts.size() + 2);
  result.emplace_back(pid);
  result.emplace_back(comm);
  result.insert(result.end(), rest_parts.begin(), rest_parts.end());
  return result;
}
std::optional<std::vector<std::string> > ReadProcPidStatFile(pid_t pid) {
  std::string stat;
  const StackString<256> path("/proc/%d/stat", pid);
  if (!ReadFile(path.ToStdString(), &stat)) {
    return std::nullopt;
  }
  return SplitProcStatString(stat);
}
std::optional<std::vector<std::string> > ReadProcSelfStatFile() {
  std::string stat;
  if (!ReadFile("/proc/self/stat", &stat)) {
    return std::nullopt;
  }
  return SplitProcStatString(stat);
}
}  // namespace perfetto::base

#endif
