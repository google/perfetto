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

#ifndef INCLUDE_PERFETTO_EXT_BASE_PROC_UTILS_H_
#define INCLUDE_PERFETTO_EXT_BASE_PROC_UTILS_H_

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

#include <optional>
#include <string>
#include <vector>

namespace perfetto::base {
std::optional<std::vector<std::string>> SplitProcStatString(
    const std::string& proc_stat_string);
std::optional<std::vector<std::string>> ReadProcPidStatFile(pid_t pid);
std::optional<std::vector<std::string>> ReadProcSelfStatFile();
}  // namespace perfetto::base

#endif

#endif  // INCLUDE_PERFETTO_EXT_BASE_PROC_UTILS_H_
