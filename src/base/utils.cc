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

}  // namespace base
}  // namespace perfetto
