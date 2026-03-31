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

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) && \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
// Android: aconfig generates PERFETTO_FLAGS_* macros in perfetto_flags.h
#include <perfetto_flags.h>
#else
// Non-Android: Define fallback PERFETTO_FLAGS_* macros
// These match the pattern from Android's aconfig codegen
#define PERFETTO_FLAGS(FLAG) PERFETTO_FLAGS_##FLAG

#define PERFETTO_FLAGS_TEST_READ_ONLY_FLAG false
#define PERFETTO_FLAGS_USE_LOCKFREE_TASKRUNNER \
  PERFETTO_BUILDFLAG(PERFETTO_ENABLE_LOCKFREE_TASKRUNNER)
#define PERFETTO_FLAGS_USE_RT_MUTEX false
#define PERFETTO_FLAGS_USE_RT_FUTEX false
#define PERFETTO_FLAGS_BUFFER_CLONE_PRESERVE_READ_ITER true
#define PERFETTO_FLAGS_USE_UNIX_SOCKET_INOTIFY \
  PERFETTO_BUILDFLAG(PERFETTO_ENABLE_SOCK_INOTIFY)
#define PERFETTO_FLAGS_TRACK_EVENT_INCREMENTAL_STATE_CLEAR_NOT_DESTROY true
#define PERFETTO_FLAGS_TRIGGER_PERFETTO_ON_TRACED_PROBES_DISCONNECT false
// PCRE2 is only used on Android device builds (controlled by aconfig).
// On Android host, PCRE2 must be disabled because Soong does not
// transitively propagate deps from static archive variants to external
// consumers (b/169779783). libandroid_runtime (frameworks/base, see
// https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/jni/Android.bp)
// lists libperfetto_c in android.shared_libs, but Soong resolves it as
// a static archive (.a) on host. libperfetto_c.a contains PCRE2 code,
// but libpcre2 is not in the host link line, causing:
//
//   ld.lld: error: undefined symbol: pcre2_compile_8
//     >>> regex.o:(...) in archive .../libperfetto_c.a
//   ld.lld: error: undefined symbol: pcre2_match_8
//     >>> regex.o:(...) in archive .../libperfetto_c.a
//
// Host tools fall back to std::regex instead.
// TODO(b/169779783): re-enable PCRE2 on host when Soong propagates deps
// transitively, by changing this to PERFETTO_BUILDFLAG(PERFETTO_PCRE2).
#define PERFETTO_FLAGS_USE_PCRE2                  \
  (!PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) && \
   PERFETTO_BUILDFLAG(PERFETTO_PCRE2))

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) && ...

#endif  // INCLUDE_PERFETTO_EXT_BASE_FLAGS_H_
