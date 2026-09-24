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

#include "src/tracing/v2/producer_ring_buffer_endpoint.h"

#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "src/tracing/core/null_trace_writer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/trace_writer_v2_impl.h"

namespace perfetto::tracing_v2 {

// static
std::unique_ptr<ProducerRingBufferEndpoint> ProducerRingBufferEndpoint::Create(
    base::TaskRunner* task_runner,
    ProducerEndpoint* endpoint,
    SharedMemoryArbiter* shared_memory_arbiter,
    std::shared_ptr<SharedMemory> ring_buffer_memory,
    uint32_t chunk_size) {
  if (!ring_buffer_memory) {
    PERFETTO_ELOG("tracing v2: no ring buffer memory");
    return nullptr;
  }
  if (ring_buffer_memory->size() > TracingService::kMaxShmSize) {
    PERFETTO_ELOG("tracing v2: ring buffer size %zu is above the limit %zu",
                  ring_buffer_memory->size(), TracingService::kMaxShmSize);
    return nullptr;
  }
  base::StatusOr<uint32_t> num_chunks = NumChunksForRingBufferLayout(
      ring_buffer_memory->start(), ring_buffer_memory->size(), chunk_size);
  if (!num_chunks.ok()) {
    PERFETTO_ELOG("tracing v2: %s", num_chunks.status().c_message());
    return nullptr;
  }
  return std::unique_ptr<ProducerRingBufferEndpoint>(
      new ProducerRingBufferEndpoint(
          task_runner, endpoint, shared_memory_arbiter,
          std::move(ring_buffer_memory), chunk_size));
}

ProducerRingBufferEndpoint::ProducerRingBufferEndpoint(
    base::TaskRunner* task_runner,
    ProducerEndpoint* endpoint,
    SharedMemoryArbiter* shared_memory_arbiter,
    std::shared_ptr<SharedMemory> ring_buffer_memory,
    uint32_t chunk_size)
    : task_runner_(task_runner),
      endpoint_(endpoint),
      shared_memory_arbiter_(shared_memory_arbiter),
      memory_(std::move(ring_buffer_memory)),
      ring_buffer_(std::make_unique<SharedRingBuffer>(
          static_cast<uint8_t*>(memory_->start()),
          memory_->size(),
          chunk_size)) {}

ProducerRingBufferEndpoint::~ProducerRingBufferEndpoint() {
  Disconnect();
}

void ProducerRingBufferEndpoint::OnReaderAttached() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // The accept reply can arrive after Disconnect().
  if (reader_state_.load() == ReaderState::kDetached)
    return;
  SetReaderState(ReaderState::kAttached);
}

void ProducerRingBufferEndpoint::Disconnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  SetReaderState(ReaderState::kDetached);
}

std::unique_ptr<TraceWriter> ProducerRingBufferEndpoint::CreateTraceWriter(
    BufferID target_buffer,
    BufferExhaustedPolicy policy) {
  if (PERFETTO_UNLIKELY(reader_state_.load() == ReaderState::kDetached))
    return std::make_unique<NullTraceWriter>();
  const WriterID writer_id =
      shared_memory_arbiter_->AllocateTracingV2WriterID();
  if (PERFETTO_UNLIKELY(!writer_id))
    return std::make_unique<NullTraceWriter>();
  return std::make_unique<TraceWriterV2Impl>(this, writer_id, target_buffer,
                                             policy);
}

void ProducerRingBufferEndpoint::Flush(std::function<void()> callback) {
  // Post the callback only after a drain task.
  // - Use force=true. Without it, this call can merge into another
  //   writer's pending drain task.
  // - That task can run late: its writer can be suspended between setting
  //   the flag and calling PostTask(). The callback would then run first.
  PostDrainTask(/*force=*/true);
  if (callback)
    task_runner_->PostTask(std::move(callback));
}

void ProducerRingBufferEndpoint::OnWriterDestroyed(WriterID id) {
  // TODO(sashwinbalaji): Verify the order of WriterID reuse between the ring
  // buffer and the SMB. The problem:
  // - The SMB arbiter can give this ID to a v1 writer at once.
  // - The last ring buffer chunk of this writer can still wait for a drain.
  // - The v1 writer can then commit to the same (producer, writer) sequence
  //   before the service reads that chunk.

  // After this release, the endpoint can destroy this object. Do not access
  // members after it.
  shared_memory_arbiter_->ReleaseTracingV2WriterID(id);
}

void ProducerRingBufferEndpoint::NotifyReader(NotifyReason reason) {
  // A posted task cannot run while a writer on the endpoint thread is
  // stalled. So send the request directly.
  if (reason == NotifyReason::kWriterStalled &&
      task_runner_->RunsTasksOnCurrentThread()) {
    endpoint_->DrainV2RingBuffer();
    return;
  }
  PostDrainTask(/*force=*/false);
}

bool ProducerRingBufferEndpoint::IsReaderAttached() const {
  return reader_state_.load() == ReaderState::kAttached;
}

void ProducerRingBufferEndpoint::SetReaderState(ReaderState next) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  const ReaderState current = reader_state_.load();
  PERFETTO_CHECK(
      next == ReaderState::kDetached ||
      (current == ReaderState::kPending && next == ReaderState::kAttached));
  reader_state_.store(next);
}

void ProducerRingBufferEndpoint::PostDrainTask(bool force) {
  // Writers share one pending drain task.
  //
  // |drain_task_pending_| is set from the moment a writer claims the task
  // until the task runs. Other writers then post nothing. The pending task
  // still drains their chunks:
  //
  //   writer thread               endpoint thread (the pending task)
  //   -------------               ----------------------------------
  //   W1. publish the chunk       E1. clear the flag
  //   W2. set the flag            E2. send DrainV2RingBuffer
  //       - was set: post nothing
  //       - was clear: post a task
  //
  // If W2 sees the flag set, E1 did not run yet. So E2 runs after W1, and
  // the service reads the chunk.
  //
  // Flush() uses force=true. See Flush() for why.
  if (!force && drain_task_pending_.fetch_or(1))
    return;
  task_runner_->PostTask([weak_this = weak_factory_.GetWeakPtr()] {
    if (weak_this)
      weak_this->SendDrainRequest();
  });
}

void ProducerRingBufferEndpoint::SendDrainRequest() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Clear the flag before the drain request.
  drain_task_pending_.store(0);
  endpoint_->DrainV2RingBuffer();
}

}  // namespace perfetto::tracing_v2
