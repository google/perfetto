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

#ifndef SRC_TRACING_INTERNAL_TRACING_MUXER_NOOP_H_
#define SRC_TRACING_INTERNAL_TRACING_MUXER_NOOP_H_

#include <cstdint>
#include <mutex>
#include <vector>

#include "perfetto/tracing/internal/tracing_muxer.h"

namespace perfetto {
namespace internal {

// The TracingMuxer used after Tracing::Initialize() with kNoOpBackend.
// Registrations succeed, but no backend is ever connected, so no data source
// instance is ever started and every TRACE_EVENT stays disabled. Unlike
// TracingMuxerImpl, it does not create a muxer thread.
class TracingMuxerNoOp : public TracingMuxer {
 public:
  // Installs the no-op muxer. The fake (uninitialized) muxer must be current.
  static void InitializeInstance(Platform* platform);

  // Unregisters all data sources, like TracingMuxerImpl::ResetForTesting(),
  // and restores the fake muxer. Used by Tracing::Shutdown() and
  // Tracing::ResetForTesting().
  static void ResetInstance();

  static bool IsInstalled();

  ~TracingMuxerNoOp() override;

  // TracingMuxer implementation.
  bool RegisterDataSource(const DataSourceDescriptor&,
                          DataSourceFactory,
                          DataSourceParams,
                          bool,
                          DataSourceStaticState*) override;
  void UpdateDataSourceDescriptor(const DataSourceDescriptor&,
                                  const DataSourceStaticState*) override;
  std::unique_ptr<TraceWriterBase> CreateTraceWriter(
      DataSourceStaticState*,
      uint32_t data_source_instance_index,
      DataSourceState*,
      BufferExhaustedPolicy buffer_exhausted_policy) override;
  void DestroyStoppedTraceWritersForCurrentThread() override;
  void RegisterInterceptor(const InterceptorDescriptor&,
                           InterceptorFactory,
                           InterceptorBase::TLSFactory,
                           InterceptorBase::TracePacketCallback) override;
  void ActivateTriggers(const std::vector<std::string>&, uint32_t) override;

 private:
  explicit TracingMuxerNoOp(Platform* platform) : TracingMuxer(platform) {}

  std::mutex mutex_;
  std::vector<DataSourceStaticState*> data_sources_;  // Guarded by |mutex_|.
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_INTERNAL_TRACING_MUXER_NOOP_H_
