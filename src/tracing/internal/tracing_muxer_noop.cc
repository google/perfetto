/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/tracing/internal/tracing_muxer_noop.h"

#include <new>

#include "perfetto/base/logging.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/internal/data_source_internal.h"
#include "perfetto/tracing/platform.h"
#include "src/tracing/internal/tracing_muxer_fake.h"

namespace perfetto {
namespace internal {

namespace {
TracingMuxerNoOp* g_instance = nullptr;
}  // namespace

// static
void TracingMuxerNoOp::InitializeInstance(Platform* platform) {
  PERFETTO_CHECK(instance_ == TracingMuxerFake::Get());
  g_instance = new TracingMuxerNoOp(platform ? platform
                                             : Platform::GetDefaultPlatform());
  instance_ = g_instance;
}

// static
void TracingMuxerNoOp::ResetInstance() {
  PERFETTO_CHECK(IsInstalled());
  for (DataSourceStaticState* static_state : g_instance->data_sources_)
    static_state->ResetForTesting();
  instance_ = TracingMuxerFake::Get();
  delete g_instance;
  g_instance = nullptr;
}

// static
bool TracingMuxerNoOp::IsInstalled() {
  return g_instance && instance_ == g_instance;
}

TracingMuxerNoOp::~TracingMuxerNoOp() = default;

bool TracingMuxerNoOp::RegisterDataSource(const DataSourceDescriptor&,
                                          DataSourceFactory,
                                          DataSourceParams,
                                          bool,
                                          DataSourceStaticState* static_state) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Ignore repeated registrations, like TracingMuxerImpl.
  if (static_state->index != kMaxDataSources)
    return true;

  auto new_index = static_cast<uint32_t>(data_sources_.size());
  if (new_index >= kMaxDataSources)
    return false;
  data_sources_.push_back(static_state);

  // No instance is ever started, so the data source stays disabled.
  for (size_t i = 0; i < static_state->instances.size(); i++)
    new (&static_state->instances[i]) DataSourceState{};
  static_state->index = new_index;
  static_state->id = new_index + 1;
  return true;
}

void TracingMuxerNoOp::UpdateDataSourceDescriptor(
    const DataSourceDescriptor&,
    const DataSourceStaticState*) {}

std::unique_ptr<TraceWriterBase> TracingMuxerNoOp::CreateTraceWriter(
    DataSourceStaticState*,
    uint32_t,
    DataSourceState*,
    BufferExhaustedPolicy) {
  // Only reachable from a started data source instance.
  PERFETTO_FATAL("CreateTraceWriter() called with kNoOpBackend");
}

void TracingMuxerNoOp::DestroyStoppedTraceWritersForCurrentThread() {}

void TracingMuxerNoOp::RegisterInterceptor(
    const InterceptorDescriptor&,
    InterceptorFactory,
    InterceptorBase::TLSFactory,
    InterceptorBase::TracePacketCallback) {}

void TracingMuxerNoOp::ActivateTriggers(const std::vector<std::string>&,
                                        uint32_t) {}

}  // namespace internal
}  // namespace perfetto
