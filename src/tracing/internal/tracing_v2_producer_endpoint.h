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

#ifndef SRC_TRACING_INTERNAL_TRACING_V2_PRODUCER_ENDPOINT_H_
#define SRC_TRACING_INTERNAL_TRACING_V2_PRODUCER_ENDPOINT_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/tracing/core/data_source_descriptor.h"

namespace perfetto {

namespace tracing_v2 {
class InProcessTracingV2Bridge;
}

namespace internal {

// Temporary wrapper that routes ordinary writers through the v2 ring while
// keeping the existing ProducerEndpoint and service path:
//
//   ProducerImpl::service_ -> TracingV2ProducerEndpoint
//     +-- v1_endpoint_ (real service endpoint)
//     +-- bridge
//          +-- v2 ring and reader
//          +-- retained v1 writers -> v1 SMB -> service
//
// It intercepts writer creation and orders Flush/Stop/Sync after pending ring
// data. Other ProducerEndpoint calls are forwarded to |v1_endpoint_|. Startup
// writers bypass this class and remain on v1.
//
// Writer creation may run on SDK threads; endpoint calls run on the muxer
// thread; the bridge drains on a separate relay thread. The separation avoids
// deadlock when a kStall v1 writer needs the muxer to commit data.
class TracingV2ProducerEndpoint : public ProducerEndpoint {
 public:
  TracingV2ProducerEndpoint(std::unique_ptr<ProducerEndpoint> v1_endpoint,
                            base::TaskRunner* muxer_task_runner,
                            base::TaskRunner* relay_task_runner);
  ~TracingV2ProducerEndpoint() override;

  TracingV2ProducerEndpoint(const TracingV2ProducerEndpoint&) = delete;
  TracingV2ProducerEndpoint& operator=(const TracingV2ProducerEndpoint&) =
      delete;
  TracingV2ProducerEndpoint(TracingV2ProducerEndpoint&&) = delete;
  TracingV2ProducerEndpoint& operator=(TracingV2ProducerEndpoint&&) = delete;

  void Disconnect() override;
  void RegisterDataSource(const DataSourceDescriptor&) override;
  void UpdateDataSource(const DataSourceDescriptor&) override;
  void UnregisterDataSource(const std::string&) override;
  void RegisterTraceWriter(uint32_t writer_id, uint32_t target_buffer) override;
  void UnregisterTraceWriter(uint32_t writer_id) override;
  void CommitData(const CommitDataRequest&,
                  CommitDataCallback callback = {}) override;
  SharedMemory* shared_memory() const override;
  size_t shared_buffer_page_size_kb() const override;
  std::unique_ptr<TraceWriter> CreateTraceWriter(
      BufferID target_buffer,
      BufferExhaustedPolicy) override;
  SharedMemoryArbiter* MaybeSharedMemoryArbiter() override;
  bool IsShmemProvidedByProducer() const override;
  void NotifyFlushComplete(FlushRequestID) override;
  void NotifyDataSourceStarted(DataSourceInstanceID) override;
  void NotifyDataSourceStopped(DataSourceInstanceID) override;
  void ActivateTriggers(const std::vector<std::string>&) override;
  void Sync(std::function<void()> callback) override;

  // Idempotently starts returning retained writers to the v1 arbiter.
  void StartReleasingV1Writers();

  // Returns whether the bridge still retains any v1 TraceWriter.
  bool HasRetainedV1Writers() const;

 private:
  // Clears no_flush because the service cannot scrape the v2 ring.
  DataSourceDescriptor DescriptorForService(const DataSourceDescriptor&) const;

  // Drains preceding ring data before running |request| on the muxer thread.
  // A null endpoint means this wrapper was destroyed meanwhile.
  void ForwardAfterPendingRingData(
      std::function<void(ProducerEndpoint*)> request);

  const std::unique_ptr<ProducerEndpoint> v1_endpoint_;
  base::TaskRunner* const muxer_task_runner_;
  const std::shared_ptr<tracing_v2::InProcessTracingV2Bridge>
      in_process_bridge_;

  // Last, so queued muxer callbacks are invalidated before other members.
  base::WeakPtrFactory<TracingV2ProducerEndpoint> weak_ptr_factory_;
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_INTERNAL_TRACING_V2_PRODUCER_ENDPOINT_H_
