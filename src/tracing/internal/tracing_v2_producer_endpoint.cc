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

#include "src/tracing/internal/tracing_v2_producer_endpoint.h"

#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/tracing/internal/tracing_v2_endpoint_functions.h"
#include "src/tracing/v2/in_process_tracing_v2_bridge.h"
#include "src/tracing/v2/shared_ring_buffer.h"

namespace perfetto {
namespace internal {

namespace {

bool IsSupported() {
  return tracing_v2::SharedRingBuffer::SupportsWriterWait();
}

std::unique_ptr<ProducerEndpoint> CreateProducerEndpoint(
    std::unique_ptr<ProducerEndpoint> v1_endpoint,
    base::TaskRunner* muxer_task_runner,
    base::TaskRunner* relay_task_runner) {
  return std::make_unique<TracingV2ProducerEndpoint>(
      std::move(v1_endpoint), muxer_task_runner, relay_task_runner);
}

void StartReleasingV1Writers(ProducerEndpoint* endpoint) {
  static_cast<TracingV2ProducerEndpoint*>(endpoint)->StartReleasingV1Writers();
}

bool HasRetainedV1Writers(const ProducerEndpoint* endpoint) {
  return static_cast<const TracingV2ProducerEndpoint*>(endpoint)
      ->HasRetainedV1Writers();
}

const TracingV2EndpointFunctions kTracingV2EndpointFunctions{
    /*is_supported=*/&IsSupported,
    /*create_endpoint=*/&CreateProducerEndpoint,
    /*start_releasing_v1_writers=*/&StartReleasingV1Writers,
    /*has_retained_v1_writers=*/&HasRetainedV1Writers,
};

}  // namespace

const TracingV2EndpointFunctions* GetTracingV2EndpointFunctions() {
  return &kTracingV2EndpointFunctions;
}

TracingV2ProducerEndpoint::TracingV2ProducerEndpoint(
    std::unique_ptr<ProducerEndpoint> v1_endpoint,
    base::TaskRunner* muxer_task_runner,
    base::TaskRunner* relay_task_runner)
    : v1_endpoint_(std::move(v1_endpoint)),
      muxer_task_runner_(muxer_task_runner),
      in_process_bridge_(
          tracing_v2::InProcessTracingV2Bridge::Create(relay_task_runner)),
      weak_ptr_factory_(this) {
  PERFETTO_CHECK(v1_endpoint_);
  PERFETTO_CHECK(muxer_task_runner_);
}

TracingV2ProducerEndpoint::~TracingV2ProducerEndpoint() {
  // The bridge can outlive this wrapper, but its retained writers cannot
  // outlive |v1_endpoint_| and its arbiter.
  PERFETTO_DCHECK(!HasRetainedV1Writers());
  StartReleasingV1Writers();
}

void TracingV2ProducerEndpoint::StartReleasingV1Writers() {
  in_process_bridge_->StartReleasingV1Writers();
}

bool TracingV2ProducerEndpoint::HasRetainedV1Writers() const {
  return in_process_bridge_->HasRetainedV1Writers();
}

void TracingV2ProducerEndpoint::Disconnect() {
  v1_endpoint_->Disconnect();
}

void TracingV2ProducerEndpoint::RegisterDataSource(
    const DataSourceDescriptor& descriptor) {
  v1_endpoint_->RegisterDataSource(DescriptorForService(descriptor));
}

void TracingV2ProducerEndpoint::UpdateDataSource(
    const DataSourceDescriptor& descriptor) {
  v1_endpoint_->UpdateDataSource(DescriptorForService(descriptor));
}

DataSourceDescriptor TracingV2ProducerEndpoint::DescriptorForService(
    const DataSourceDescriptor& descriptor) const {
  if (!descriptor.no_flush())
    return descriptor;
  // The service normally scrapes no_flush data sources directly, but it cannot
  // see the v2 ring. Clear only its copy to force a ring-draining flush; the
  // muxer retains no_flush and does not call the data source's OnFlush().
  DataSourceDescriptor for_service = descriptor;
  for_service.set_no_flush(false);
  return for_service;
}

void TracingV2ProducerEndpoint::UnregisterDataSource(const std::string& name) {
  v1_endpoint_->UnregisterDataSource(name);
}

void TracingV2ProducerEndpoint::RegisterTraceWriter(uint32_t writer_id,
                                                    uint32_t target_buffer) {
  v1_endpoint_->RegisterTraceWriter(writer_id, target_buffer);
}

void TracingV2ProducerEndpoint::UnregisterTraceWriter(uint32_t writer_id) {
  v1_endpoint_->UnregisterTraceWriter(writer_id);
}

void TracingV2ProducerEndpoint::CommitData(const CommitDataRequest& request,
                                           CommitDataCallback callback) {
  v1_endpoint_->CommitData(request, std::move(callback));
}

SharedMemory* TracingV2ProducerEndpoint::shared_memory() const {
  return v1_endpoint_->shared_memory();
}

size_t TracingV2ProducerEndpoint::shared_buffer_page_size_kb() const {
  return v1_endpoint_->shared_buffer_page_size_kb();
}

std::unique_ptr<TraceWriter> TracingV2ProducerEndpoint::CreateTraceWriter(
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy) {
  // The retained v1 writer supplies the WriterID and forwarding destination.
  std::unique_ptr<TraceWriter> v1_writer =
      v1_endpoint_->CreateTraceWriter(target_buffer, buffer_exhausted_policy);
  return in_process_bridge_->CreateTraceWriter(
      std::move(v1_writer), target_buffer, buffer_exhausted_policy);
}

SharedMemoryArbiter* TracingV2ProducerEndpoint::MaybeSharedMemoryArbiter() {
  // Startup writers are created directly by this arbiter and remain on v1.
  // TODO(sashwinbalaji): migrate them with the v2 producer/service lifecycle.
  return v1_endpoint_->MaybeSharedMemoryArbiter();
}

bool TracingV2ProducerEndpoint::IsShmemProvidedByProducer() const {
  return v1_endpoint_->IsShmemProvidedByProducer();
}

void TracingV2ProducerEndpoint::NotifyFlushComplete(FlushRequestID id) {
  ForwardAfterPendingRingData([id](ProducerEndpoint* endpoint) {
    if (endpoint)
      endpoint->NotifyFlushComplete(id);
  });
}

void TracingV2ProducerEndpoint::NotifyDataSourceStarted(
    DataSourceInstanceID id) {
  v1_endpoint_->NotifyDataSourceStarted(id);
}

void TracingV2ProducerEndpoint::NotifyDataSourceStopped(
    DataSourceInstanceID id) {
  ForwardAfterPendingRingData([id](ProducerEndpoint* endpoint) {
    if (endpoint)
      endpoint->NotifyDataSourceStopped(id);
  });
}

void TracingV2ProducerEndpoint::ActivateTriggers(
    const std::vector<std::string>& triggers) {
  v1_endpoint_->ActivateTriggers(triggers);
}

void TracingV2ProducerEndpoint::Sync(std::function<void()> callback) {
  ForwardAfterPendingRingData(
      [callback = std::move(callback)](ProducerEndpoint* endpoint) mutable {
        // A notification can be dropped once the connection is gone; a
        // Sync() caller is still waiting, so complete it here.
        if (endpoint) {
          endpoint->Sync(std::move(callback));
        } else if (callback) {
          callback();
        }
      });
}

void TracingV2ProducerEndpoint::ForwardAfterPendingRingData(
    std::function<void(ProducerEndpoint*)> request) {
  base::TaskRunner* const muxer_task_runner = muxer_task_runner_;
  auto weak_this = weak_ptr_factory_.GetWeakPtr();
  in_process_bridge_->DrainPendingData(
      [muxer_task_runner, weak_this = std::move(weak_this),
       request = std::move(request)]() mutable {
        // Inspect the WeakPtr only on its owning thread.
        muxer_task_runner->PostTask([weak_this = std::move(weak_this),
                                     request = std::move(request)]() mutable {
          request(weak_this ? weak_this->v1_endpoint_.get() : nullptr);
        });
      });
}

}  // namespace internal
}  // namespace perfetto
