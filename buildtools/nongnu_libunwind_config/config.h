/*
 * Copyright (C) 2024 The Android Open Source Project
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

// Pre-baked config.h for nongnu.org libunwind, used by Perfetto's GN build.

#ifndef BUILDTOOLS_NONGNU_LIBUNWIND_CONFIG_CONFIG_H_
#define BUILDTOOLS_NONGNU_LIBUNWIND_CONFIG_CONFIG_H_

#define HAVE_ELF_H 1
#define HAVE_ENDIAN_H 1

#define PACKAGE_STRING "libunwind 1.8.0"
#define PACKAGE_BUGREPORT "https://github.com/libunwind/libunwind"

// Enable DWARF-based unwinding.
#define HAVE_DWARF 1

// Linux-specific features.
#define HAVE_LINK_H 1
#define HAVE_DL_ITERATE_PHDR 1
#define _GNU_SOURCE 1

// Disable .debug_frame support. We use .eh_frame_hdr for remote unwinding
// and the debug_frame code path requires elfxx.c which is excluded by
// UNW_REMOTE_ONLY.
/* #undef CONFIG_DEBUG_FRAME */

// We have atomic builtins.
#define HAVE_ATOMIC_OPS_H 0
#define HAVE_SYNC_ATOMICS 1

#endif  // BUILDTOOLS_NONGNU_LIBUNWIND_CONFIG_CONFIG_H_
