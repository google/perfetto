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

#ifndef INCLUDE_PERFETTO_PUBLIC_ABI_PRODUCER_ABI_H_
#define INCLUDE_PERFETTO_PUBLIC_ABI_PRODUCER_ABI_H_

#include <stdint.h>

#include "perfetto/public/abi/export.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the global system perfetto producer.
PERFETTO_SDK_EXPORT void PerfettoProducerSystemInit(void);

// Initializes the global in-process perfetto producer.
PERFETTO_SDK_EXPORT void PerfettoProducerInProcessInit(void);

// Informs the tracing services to activate any of these triggers if any tracing
// session was waiting for them.
//
// `trigger_names` is an array of `const char*` (zero terminated strings). The
// last pointer in the array must be NULL.
//
// Sends the trigger signal to all the initialized backends that are currently
// connected and that connect in the next `ttl_ms` milliseconds (but
// returns immediately anyway).
PERFETTO_SDK_EXPORT void PerfettoProducerActivateTriggers(
    const char* trigger_names[],
    uint32_t ttl_ms);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PERFETTO_PUBLIC_ABI_PRODUCER_ABI_H_
