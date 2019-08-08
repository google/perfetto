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

#ifndef INCLUDE_PERFETTO_EXT_BASE_WATCHDOG_H_
#define INCLUDE_PERFETTO_EXT_BASE_WATCHDOG_H_

#include <functional>

#include "perfetto/base/build_config.h"

// The POSIX watchdog is only supported on Linux and Android in non-embedder
// builds.
#if (PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||    \
     PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)) && \
    !PERFETTO_BUILDFLAG(PERFETTO_EMBEDDER_BUILD)
#define PERFETTO_USE_POSIX_WATCHDOG() 1
#else
#define PERFETTO_USE_POSIX_WATCHDOG() 0
#endif

#if PERFETTO_USE_POSIX_WATCHDOG()
#include "perfetto/ext/base/watchdog_posix.h"
#else
#include "perfetto/ext/base/watchdog_noop.h"
#endif

namespace perfetto {
namespace base {

inline void RunTaskWithWatchdogGuard(const std::function<void()>& task) {
  // Maximum time a single task can take in a TaskRunner before the
  // program suicides.
  constexpr int64_t kWatchdogMillis = 30000;  // 30s

  Watchdog::Timer handle =
      base::Watchdog::GetInstance()->CreateFatalTimer(kWatchdogMillis);
  task();
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_WATCHDOG_H_
