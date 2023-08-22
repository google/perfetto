/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/ext/tracing/core/consumer.h"
#include "perfetto/ext/tracing/core/producer.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "perfetto/ext/tracing/core/tracing_service.h"

// This translation unit contains the definitions for the destructor of pure
// virtual interfaces for the current build target. The alternative would be
// introducing a one-liner .cc file for each pure virtual interface, which is
// overkill. This is for compliance with -Wweak-vtables.

namespace perfetto {

Consumer::~Consumer() = default;
Producer::~Producer() = default;
TracingService::~TracingService() = default;
ConsumerEndpoint::~ConsumerEndpoint() = default;
ProducerEndpoint::~ProducerEndpoint() = default;
SharedMemory::~SharedMemory() = default;
SharedMemory::Factory::~Factory() = default;
SharedMemoryArbiter::~SharedMemoryArbiter() = default;

// TODO(primiano): make pure virtual after various 3way patches.
void ConsumerEndpoint::CloneSession(TracingSessionID) {}
void Consumer::OnSessionCloned(const OnSessionClonedArgs&) {}

void ConsumerEndpoint::Flush(uint32_t, FlushCallback, FlushFlags) {
  // In the perfetto codebase, this 3-arg Flush is always overridden and this
  // FATAL is never reached. The only case where this is used is in
  // arctraceservice's PerfettoClient_test.cpp. That test mocks the old
  // 2-arg version of Flush but doesn't actually invoke the 3-arg version.
  PERFETTO_FATAL("ConsumerEndpoint::Flush(3) not implemented");
}

void ConsumerEndpoint::Flush(uint32_t timeout_ms, FlushCallback callback) {
  // This 2-arg version of Flush() is invoked by arctraceservice's
  // PerfettoClient::Flush().
  Flush(timeout_ms, std::move(callback), FlushFlags(0));
}

}  // namespace perfetto
