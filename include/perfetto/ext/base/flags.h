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

#ifndef INCLUDE_PERFETTO_EXT_BASE_FLAGS_H_
#define INCLUDE_PERFETTO_EXT_BASE_FLAGS_H_

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/flags_list.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) && \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <perfetto_flags.h>
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) && \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define PERFETTO_FLAGS_DEFINE_FN(name, default_non_android_value) \
  constexpr bool name = ::perfetto::flags::name();
#else
#define PERFETTO_FLAGS_DEF_GETTER(name, default_non_android_value) \
  constexpr bool name = static_cast<bool>(default_non_android_value);
#endif

namespace perfetto::base::flags {

PERFETTO_READ_ONLY_FLAGS(PERFETTO_FLAGS_DEF_GETTER)

}  // namespace perfetto::base::flags

#endif  // INCLUDE_PERFETTO_EXT_BASE_FLAGS_H_
