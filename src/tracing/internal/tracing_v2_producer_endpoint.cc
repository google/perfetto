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

#include <atomic>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flags.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "src/tracing/v2/in_process_tracing_v2_bridge.h"

namespace perfetto {
namespace internal {

namespace {

std::atomic<bool> g_force_tracing_v2_in_process_for_testing{false};

}  // namespace

bool UseTracingV2InProcess() {
  // The data path blocks on a futex when the ring fills, so a platform without
  // one cannot host it at all. Checked before anything else so neither the
  // aconfig flag nor the test override can switch it on there.
  if (!tracing_v2::kHasFutex)
    return false;
  // Default-off aconfig flag on Android; a build-time false everywhere else,
  // which is why tests need the override below to reach the path at all.
  // TODO(sashwinbalaji): combine this global permission with an explicit
  // process and system-backend opt-in before enabling the Android flag.
  // The test override is required to be set before initialization, so it
  // publishes no associated state and needs atomicity only.
  return PERFETTO_FLAGS(TRACING_V2_IN_PROCESS) ||
         g_force_tracing_v2_in_process_for_testing.load(
             std::memory_order_relaxed);
}

void SetTracingV2InProcessForTesting(bool enabled) {
  g_force_tracing_v2_in_process_for_testing.store(enabled,
                                                  std::memory_order_relaxed);
}

TracingV2ProducerEndpoint::TracingV2ProducerEndpoint(
    std::unique_ptr<ProducerEndpoint> endpoint,
    base::TaskRunner* muxer_task_runner,
    base::TaskRunner* relay_task_runner)
    : endpoint_(std::move(endpoint)),
      muxer_task_runner_(muxer_task_runner),
      bridge_(tracing_v2::InProcessTracingV2Bridge::Create(relay_task_runner)) {
  PERFETTO_CHECK(endpoint_);
  PERFETTO_CHECK(muxer_task_runner_);
}

TracingV2ProducerEndpoint::~TracingV2ProducerEndpoint() = default;

void TracingV2ProducerEndpoint::Disconnect() {
  endpoint_->Disconnect();
}

void TracingV2ProducerEndpoint::RegisterDataSource(
    const DataSourceDescriptor& descriptor) {
  endpoint_->RegisterDataSource(descriptor);
}

void TracingV2ProducerEndpoint::UpdateDataSource(
    const DataSourceDescriptor& descriptor) {
  endpoint_->UpdateDataSource(descriptor);
}

void TracingV2ProducerEndpoint::UnregisterDataSource(const std::string& name) {
  endpoint_->UnregisterDataSource(name);
}

void TracingV2ProducerEndpoint::RegisterTraceWriter(uint32_t writer_id,
                                                    uint32_t target_buffer) {
  endpoint_->RegisterTraceWriter(writer_id, target_buffer);
}

void TracingV2ProducerEndpoint::UnregisterTraceWriter(uint32_t writer_id) {
  endpoint_->UnregisterTraceWriter(writer_id);
}

void TracingV2ProducerEndpoint::CommitData(const CommitDataRequest& request,
                                           CommitDataCallback callback) {
  endpoint_->CommitData(request, std::move(callback));
}

SharedMemory* TracingV2ProducerEndpoint::shared_memory() const {
  return endpoint_->shared_memory();
}

size_t TracingV2ProducerEndpoint::shared_buffer_page_size_kb() const {
  return endpoint_->shared_buffer_page_size_kb();
}

std::unique_ptr<TraceWriter> TracingV2ProducerEndpoint::CreateTraceWriter(
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy) {
  // Both hops get the caller's policy. Dropping at the second one would lose
  // packets the v2 writer had already accepted under kStall, which is the one
  // thing kStall exists to prevent.
  //
  // The cost is that a stalling downstream writer blocks the relay, and the
  // relay is shared by every writer in the process, so unrelated data sources
  // stall behind it. v1 has no equivalent: there each writer stalls on its own
  // thread. This is a property of the temporary second hop only: once traced
  // reads the ring directly there is nothing left to stall on.
  // TODO(sashwinbalaji): revisit if it shows up in the relay measurements.
  std::unique_ptr<TraceWriter> downstream =
      endpoint_->CreateTraceWriter(target_buffer, buffer_exhausted_policy);
  return bridge_->CreateTraceWriter(std::move(downstream), target_buffer,
                                    buffer_exhausted_policy);
}

SharedMemoryArbiter* TracingV2ProducerEndpoint::MaybeSharedMemoryArbiter() {
  // Startup writers are created directly by this arbiter, bypassing
  // CreateTraceWriter() and therefore the v2 ring. That exclusion is
  // intentional for now, not a permanent split between writer types.
  // TODO(sashwinbalaji): migrate startup writers after the direct v2
  // producer/service lifecycle is defined.
  return endpoint_->MaybeSharedMemoryArbiter();
}

bool TracingV2ProducerEndpoint::IsShmemProvidedByProducer() const {
  return endpoint_->IsShmemProvidedByProducer();
}

void TracingV2ProducerEndpoint::RunAfterRelayDrained(
    RelayContinuation continuation) {
  PERFETTO_DCHECK(muxer_task_runner_->RunsTasksOnCurrentThread());
  const uint32_t watermark = bridge_->write_pos();
  base::TaskRunner* const muxer_task_runner = muxer_task_runner_;
  base::WeakPtr<TracingV2ProducerEndpoint> weak_this =
      weak_ptr_factory_.GetWeakPtr();
  bridge_->DrainThrough(
      watermark, [muxer_task_runner, weak_this,
                  continuation = std::move(continuation)]() mutable {
        // On the relay sequence. A weak pointer may be copied from any thread
        // but only tested on the sequence that owns its factory, so hand it
        // straight back to the muxer without touching it.
        muxer_task_runner->PostTask(
            [weak_this, continuation = std::move(continuation)] {
              if (weak_this)
                continuation(weak_this.get());
            });
      });
}

void TracingV2ProducerEndpoint::FlushPendingCommitDataRequests() {
  if (SharedMemoryArbiter* arbiter = endpoint_->MaybeSharedMemoryArbiter())
    arbiter->FlushPendingCommitDataRequests();
}

void TracingV2ProducerEndpoint::NotifyFlushComplete(FlushRequestID id) {
  RunAfterRelayDrained([id](TracingV2ProducerEndpoint* self) {
    self->FlushPendingCommitDataRequests();
    self->endpoint_->NotifyFlushComplete(id);
  });
}

void TracingV2ProducerEndpoint::NotifyDataSourceStarted(
    DataSourceInstanceID id) {
  endpoint_->NotifyDataSourceStarted(id);
}

void TracingV2ProducerEndpoint::NotifyDataSourceStopped(
    DataSourceInstanceID id) {
  RunAfterRelayDrained([id](TracingV2ProducerEndpoint* self) {
    self->FlushPendingCommitDataRequests();
    self->endpoint_->NotifyDataSourceStopped(id);
  });
}

void TracingV2ProducerEndpoint::ActivateTriggers(
    const std::vector<std::string>& triggers) {
  endpoint_->ActivateTriggers(triggers);
}

void TracingV2ProducerEndpoint::Sync(std::function<void()> callback) {
  // Sync() promises the service has processed everything this producer sent
  // before it. Passing it straight through would let it overtake trace data
  // still sitting in the ring, which no CommitData represents yet. The
  // callback is dropped if the endpoint goes away first: it could not be
  // delivered through a disconnected endpoint anyway.
  RunAfterRelayDrained([callback = std::move(callback)](
                           TracingV2ProducerEndpoint* self) mutable {
    self->FlushPendingCommitDataRequests();
    self->endpoint_->Sync(std::move(callback));
  });
}

}  // namespace internal
}  // namespace perfetto
