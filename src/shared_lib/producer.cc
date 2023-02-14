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

#include "perfetto/public/abi/producer.h"

#include <atomic>
#include <bitset>

#include "perfetto/tracing/backend_type.h"
#include "perfetto/tracing/tracing.h"

void PerfettoProducerInProcessInit() {
  perfetto::TracingInitArgs args;
  args.backends = perfetto::kInProcessBackend;
  perfetto::Tracing::Initialize(args);
}

void PerfettoProducerSystemInit() {
  perfetto::TracingInitArgs args;
  args.backends = perfetto::kSystemBackend;
  perfetto::Tracing::Initialize(args);
}

void PerfettoProducerInProcessAndSystemInit() {
  perfetto::TracingInitArgs args;
  args.backends = perfetto::kInProcessBackend | perfetto::kSystemBackend;
  perfetto::Tracing::Initialize(args);
}
