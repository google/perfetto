/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/android_stats/statsd_logging_helper.h"

#include <string>

#include "perfetto/base/compiler.h"
#include "src/android_internal/lazy_library_loader.h"
#include "src/android_internal/statsd_logging.h"

namespace perfetto {
namespace android_stats {
namespace {
// |g_android_logging_enabled| is one mechanism to make
// sure we don't accidentally log on non-Android tree
// platforms. The other is that PERFETTO_LAZY_LOAD will
// return a nullptr on all non-Android in-tree platforms
// as libperfetto_android_internal will not be available.
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
constexpr bool g_android_logging_enabled = true;
#else
constexpr bool g_android_logging_enabled = false;
#endif
}  // namespace

void MaybeLogUploadEvent(PerfettoStatsdAtom atom,
                         int64_t uuid_lsb,
                         int64_t uuid_msb) {
  if (!g_android_logging_enabled)
    return;

  PERFETTO_LAZY_LOAD(android_internal::StatsdLogUploadEvent, log_event_fn);
  if (log_event_fn) {
    log_event_fn(atom, uuid_lsb, uuid_msb);
  }
}

void MaybeLogTriggerEvents(PerfettoTriggerAtom atom,
                           const std::vector<std::string>& triggers) {
  if (!g_android_logging_enabled)
    return;

  PERFETTO_LAZY_LOAD(android_internal::StatsdLogTriggerEvent, log_event_fn);
  if (log_event_fn) {
    for (const std::string& trigger_name : triggers) {
      log_event_fn(atom, trigger_name.c_str());
    }
  }
}

}  // namespace android_stats
}  // namespace perfetto
