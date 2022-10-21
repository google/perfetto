/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "perfetto/base/thread_utils.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>
#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)

namespace perfetto {
namespace base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
static PlatformThreadId ResolveThreadId() {
  zx_info_handle_basic_t basic;
  return (zx_object_get_info(zx_thread_self(), ZX_INFO_HANDLE_BASIC, &basic,
                             sizeof(basic), nullptr, nullptr) == ZX_OK)
             ? basic.koid
             : ZX_KOID_INVALID;
}
PlatformThreadId GetThreadId() {
  thread_local static PlatformThreadId thread_id = ResolveThreadId();
  return thread_id;
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)

}  // namespace base
}  // namespace perfetto
