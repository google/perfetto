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

#include "perfetto/public/abi/backend_type.h"
#include "perfetto/public/abi/producer.h"

// Arguments for PerfettoProducerInit. This struct is not ABI-stable, fields can
// be added and rearranged.
struct PerfettoProducerInitArgs {
  // Bitwise-or of backends that should be enabled.
  PerfettoBackendTypes backends;
};

// Initializes the global perfetto producer.
static inline void PerfettoProducerInit(struct PerfettoProducerInitArgs args) {
  if (args.backends & PERFETTO_BACKEND_IN_PROCESS &&
      args.backends & PERFETTO_BACKEND_SYSTEM) {
    PerfettoProducerInProcessAndSystemInit();
  } else if (args.backends & PERFETTO_BACKEND_IN_PROCESS) {
    PerfettoProducerInProcessInit();
  } else if (args.backends & PERFETTO_BACKEND_SYSTEM) {
    PerfettoProducerSystemInit();
  }
}

#endif  // INCLUDE_PERFETTO_PUBLIC_PRODUCER_H_
