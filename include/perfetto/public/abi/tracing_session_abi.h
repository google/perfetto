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

#ifndef INCLUDE_PERFETTO_PUBLIC_ABI_TRACING_SESSION_ABI_H_
#define INCLUDE_PERFETTO_PUBLIC_ABI_TRACING_SESSION_ABI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "perfetto/public/abi/backend_type.h"
#include "perfetto/public/abi/export.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque pointer to the internal representation of a tracing session.
struct PerfettoTracingSessionImpl;

PERFETTO_SDK_EXPORT struct PerfettoTracingSessionImpl*
PerfettoTracingSessionCreate(PerfettoBackendTypes backend);

PERFETTO_SDK_EXPORT void PerfettoTracingSessionSetup(
    struct PerfettoTracingSessionImpl*,
    void* cfg_begin,
    size_t cfg_len);

PERFETTO_SDK_EXPORT void PerfettoTracingSessionStartAsync(
    struct PerfettoTracingSessionImpl*);

PERFETTO_SDK_EXPORT void PerfettoTracingSessionStartBlocking(
    struct PerfettoTracingSessionImpl*);

PERFETTO_SDK_EXPORT void PerfettoTracingSessionStopAsync(
    struct PerfettoTracingSessionImpl*);

PERFETTO_SDK_EXPORT void PerfettoTracingSessionStopBlocking(
    struct PerfettoTracingSessionImpl*);

// Called back to read pieces of tracing data. `data` points to a chunk of trace
// data, `size` bytes long. `has_more` is true if there is more tracing data and
// the callback will be invoked again.
typedef void (*PerfettoTracingSessionReadCb)(struct PerfettoTracingSessionImpl*,
                                             const void* data,
                                             size_t size,
                                             bool has_more,
                                             void* user_arg);

// Repeatedly calls cb with data from the tracing session. `user_arg` is passed
// as is to the callback.
PERFETTO_SDK_EXPORT void PerfettoTracingSessionReadTraceBlocking(
    struct PerfettoTracingSessionImpl*,
    PerfettoTracingSessionReadCb cb,
    void* user_arg);

PERFETTO_SDK_EXPORT void PerfettoTracingSessionDestroy(
    struct PerfettoTracingSessionImpl*);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PERFETTO_PUBLIC_ABI_TRACING_SESSION_ABI_H_
