/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PUBLIC_PRODUCER_H_
#define INCLUDE_PERFETTO_PUBLIC_PRODUCER_H_

#include <stdint.h>

#include "perfetto/public/abi/backend_type.h"
#include "perfetto/public/abi/producer_abi.h"
#include "perfetto/public/compiler.h"

// Arguments for PerfettoProducerInit. This struct is not ABI-stable, fields can
// be added and rearranged.
struct PerfettoProducerInitArgs {
  // Bitwise-or of backends that should be enabled.
  PerfettoBackendTypes backends;
};

// Initializes a PerfettoProducerInitArgs struct.
#define PERFETTO_PRODUCER_INIT_ARGS_INIT() \
  { 0 }

// Initializes the global perfetto producer.
static inline void PerfettoProducerInit(struct PerfettoProducerInitArgs args) {
  if (args.backends & PERFETTO_BACKEND_IN_PROCESS) {
    PerfettoProducerInProcessInit();
  }
  if (args.backends & PERFETTO_BACKEND_SYSTEM) {
    PerfettoProducerSystemInit();
  }
}

// Informs the tracing services to activate the single trigger `trigger_name` if
// any tracing session was waiting for it.
//
// Sends the trigger signal to all the initialized backends that are currently
// connected and that connect in the next `ttl_ms` milliseconds (but
// returns immediately anyway).
static inline void PerfettoProducerActivateTrigger(const char* trigger_name,
                                                   uint32_t ttl_ms) {
  const char* trigger_names[2];
  trigger_names[0] = trigger_name;
  trigger_names[1] = PERFETTO_NULL;
  PerfettoProducerActivateTriggers(trigger_names, ttl_ms);
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PRODUCER_H_
