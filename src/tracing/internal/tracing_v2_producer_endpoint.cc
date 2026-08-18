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
#include "perfetto/ext/base/waitable_event.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "src/tracing/v2/in_process_tracing_v2_bridge.h"
#include "src/tracing/v2/shared_ring_buffer.h"

namespace perfetto {
namespace internal {

namespace {

std::atomic<bool> g_force_tracing_v2_in_process_for_testing{false};
std::atomic<uint64_t> g_tracing_v2_writers_created{0};

}  // namespace

bool UseTracingV2InProcess() {
  // A writer that fills the ring blocks on a futex, so a platform without one
  // cannot host this path at all. Checked before anything else, so neither the
  // aconfig flag nor the test override can switch it on there.
  if (!tracing_v2::kHasFutex)
    return false;
#if PERFETTO_TRACING_V2_HAS_FUTEX()
  // Default-off aconfig flag on Android, a build-time false everywhere else,
  // which is why tests need the override to reach the path at all.
  // TODO(sashwinbalaji): combine this global permission with an explicit
  // process and backend opt-in before enabling the Android flag. The override
  // is required to be set before initialization, so it publishes no associated
  // state and needs atomicity only.
  return PERFETTO_FLAGS(TRACING_V2_IN_PROCESS) ||
         g_force_tracing_v2_in_process_for_testing.load(
             std::memory_order_relaxed);
#endif
}

void SetTracingV2InProcessForTesting(bool enabled) {
  g_force_tracing_v2_in_process_for_testing.store(enabled,
                                                  std::memory_order_relaxed);
}

uint64_t GetTracingV2WritersCreatedForTesting() {
  return g_tracing_v2_writers_created.load(std::memory_order_relaxed);
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

TracingV2ProducerEndpoint::~TracingV2ProducerEndpoint() {
  // |endpoint_|, and with it the arbiter, is destroyed as this returns, so
  // every writer that hands a WriterID back to it must already be gone. Both
  // teardown paths guarantee that without anybody blocking: the muxer starts
  // the release when it disposes the connection or shuts down, and until the
  // relay has run it the arbiter has outstanding WriterIDs and refuses to shut
  // down, so nothing drops the last reference to this endpoint.
  PERFETTO_DCHECK(!relay_retains_writers());
  // For a caller that got here without asking - a unit test that simply drops
  // the endpoint, say. There is nothing left to release when the DCHECK above
  // holds; this only makes sure the bridge stops accepting writers.
  StartReleasingRelayWriters();
}

void TracingV2ProducerEndpoint::StartReleasingRelayWriters() {
  if (!bridge_)
    return;
  // The bridge retains one downstream v1 writer per v2 WriterID, and those
  // writers hand their ids back to |endpoint_|'s arbiter when they are
  // destroyed. The bridge can outlive this decorator - SDK threads still hold
  // v2 writers, which hold a reference to it - so the downstream writers have
  // to go before the arbiter does.
  //
  // Whatever is still in the ring at this point is dropped. That is a real
  // Step-1 limitation: a producer whose connection goes away loses its ring
  // tail, and a caller that needs the data has to Flush() before the session
  // ends.
  // TODO(sashwinbalaji): it disappears with this relay. Once traced reads the
  // mapping there is no second writer to release and nothing to drop here.
  bridge_->StartReleasingDownstreamWriters();
}

bool TracingV2ProducerEndpoint::relay_retains_writers() const {
  return bridge_ && bridge_->holds_downstream_writers();
}

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
  std::unique_ptr<TraceWriter> downstream =
      endpoint_->CreateTraceWriter(target_buffer, buffer_exhausted_policy);
  if (!bridge_)
    return downstream;

  // Both hops get the caller's policy. Dropping at the second one would lose
  // packets the v2 writer had already accepted under kStall, which is the one
  // thing kStall exists to prevent.
  //
  // The cost is that a stalling downstream writer blocks the relay, and the
  // relay is shared by every writer in the process, so unrelated data sources
  // stall behind it. v1 has no equivalent: there each writer stalls on its own
  // thread. This is a property of the temporary second hop only; once traced
  // reads the ring directly there is nothing left to stall on.
  // TODO(sashwinbalaji): revisit if it shows up in the relay measurements.
  TraceWriter* const downstream_raw = downstream.get();
  std::unique_ptr<TraceWriter> writer = bridge_->CreateTraceWriter(
      std::move(downstream), target_buffer, buffer_exhausted_policy);
  // The bridge hands the plain v1 writer back when it cannot use the WriterID.
  if (writer.get() != downstream_raw)
    g_tracing_v2_writers_created.fetch_add(1, std::memory_order_relaxed);
  return writer;
}

SharedMemoryArbiter* TracingV2ProducerEndpoint::MaybeSharedMemoryArbiter() {
  // Startup writers are created directly by this arbiter, bypassing
  // CreateTraceWriter() and therefore the v2 ring. That exclusion is
  // intentional for now, not a permanent split between writer types.
  // TODO(sashwinbalaji): migrate startup writers once the direct v2
  // producer/service lifecycle is defined.
  return endpoint_->MaybeSharedMemoryArbiter();
}

bool TracingV2ProducerEndpoint::IsShmemProvidedByProducer() const {
  return endpoint_->IsShmemProvidedByProducer();
}

void TracingV2ProducerEndpoint::NotifyFlushComplete(FlushRequestID id) {
  endpoint_->NotifyFlushComplete(id);
}

void TracingV2ProducerEndpoint::NotifyDataSourceStarted(
    DataSourceInstanceID id) {
  endpoint_->NotifyDataSourceStarted(id);
}

void TracingV2ProducerEndpoint::NotifyDataSourceStopped(
    DataSourceInstanceID id) {
  endpoint_->NotifyDataSourceStopped(id);
}

void TracingV2ProducerEndpoint::ActivateTriggers(
    const std::vector<std::string>& triggers) {
  endpoint_->ActivateTriggers(triggers);
}

void TracingV2ProducerEndpoint::Sync(std::function<void()> callback) {
  endpoint_->Sync(std::move(callback));
}

void TracingV2ProducerEndpoint::FlushPendingCommitDataRequests() {
  PERFETTO_DCHECK(muxer_task_runner_->RunsTasksOnCurrentThread());
  if (SharedMemoryArbiter* arbiter = endpoint_->MaybeSharedMemoryArbiter())
    arbiter->FlushPendingCommitDataRequests();
}

void TracingV2ProducerEndpoint::WaitForRelayQuiescenceForTesting() {
  PERFETTO_CHECK(!muxer_task_runner_->RunsTasksOnCurrentThread());
  if (!bridge_)
    return;

  base::WaitableEvent done;
  base::TaskRunner* const muxer_task_runner = muxer_task_runner_;
  // Capturing |this| is safe because the caller is required to keep the
  // endpoint alive for the whole call; that is the contract in the header.
  TracingV2ProducerEndpoint* const self = this;
  // Three hops, each on the sequence that owns the state it touches:
  //   1. the relay drains everything the ring holds and commits the downstream
  //      writers it touched;
  //   2. the muxer pushes the commits the arbiter is still batching - only the
  //      muxer sequence may - and then asks the service to Sync();
  //   3. the service's acknowledgement releases this thread.
  // Deadlock-free because the caller is neither sequence, and the muxer keeps
  // pumping while this thread waits, which is what lets a relay blocked on a
  // full downstream buffer make progress.
  bridge_->DrainToQuiescenceForTesting([muxer_task_runner, self, &done] {
    muxer_task_runner->PostTask([self, &done] {
      self->FlushPendingCommitDataRequests();
      self->endpoint_->Sync([&done] { done.Notify(); });
    });
  });
  done.Wait();
}

}  // namespace internal
}  // namespace perfetto
