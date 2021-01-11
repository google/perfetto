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

#include "perfetto/ext/base/utils.h"

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <unistd.h>  // For getpagesize() and geteuid().
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <mach/vm_page_size.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <dlfcn.h>
#include <malloc.h>

#ifdef M_PURGE
#define PERFETTO_M_PURGE M_PURGE
#else
// Only available in in-tree builds and on newer SDKs.
#define PERFETTO_M_PURGE -101
#endif

namespace {
extern "C" {
using MalloptType = void (*)(int, int);
}
}  // namespace
#endif  // OS_ANDROID

namespace perfetto {
namespace base {

void MaybeReleaseAllocatorMemToOS() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  // mallopt() on Android requires SDK level 26. Many targets and embedders
  // still depend on a lower SDK level. Given mallopt() is a quite simple API,
  // use reflection to do this rather than bumping the SDK level for all
  // embedders. This keeps the behavior of standalone builds aligned with
  // in-tree builds.
  static MalloptType mallopt_fn =
      reinterpret_cast<MalloptType>(dlsym(RTLD_DEFAULT, "mallopt"));
  if (!mallopt_fn)
    return;
  mallopt_fn(PERFETTO_M_PURGE, 0);
#endif
}

uint32_t GetSysPageSize() {
  ignore_result(kPageSize);  // Just to keep the amalgamated build happy.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  static std::atomic<uint32_t> page_size{0};
  // This function might be called in hot paths. Avoid calling getpagesize() all
  // the times, in many implementations getpagesize() calls sysconf() which is
  // not cheap.
  uint32_t cached_value = page_size.load(std::memory_order_relaxed);
  if (PERFETTO_UNLIKELY(cached_value == 0)) {
    cached_value = static_cast<uint32_t>(getpagesize());
    page_size.store(cached_value, std::memory_order_relaxed);
  }
  return cached_value;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  return static_cast<uint32_t>(vm_page_size);
#else
  return 4096;
#endif
}

uid_t GetCurrentUserId() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  return geteuid();
#else
  // TODO(primiano): On Windows we could hash the current user SID and derive a
  // numeric user id [1]. It is not clear whether we need that. Right now that
  // would not bring any benefit. Returning 0 unil we can prove we need it.
  // [1]:https://android-review.googlesource.com/c/platform/external/perfetto/+/1513879/25/src/base/utils.cc
  return 0;
#endif
}

void SetEnv(const std::string& key, const std::string& value) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  PERFETTO_CHECK(::_putenv_s(key.c_str(), value.c_str()) == 0);
#else
  PERFETTO_CHECK(::setenv(key.c_str(), value.c_str(), /*overwrite=*/true) == 0);
#endif
}

}  // namespace base
}  // namespace perfetto
