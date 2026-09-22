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
    std::unique_ptr<SharedMemory> ring_buffer_memory,
    uint32_t chunk_size) {
  if (!ring_buffer_memory ||
      ring_buffer_memory->size() > TracingService::kMaxShmSize ||
      !NumChunksForRingBufferLayout(ring_buffer_memory->start(),
                                    ring_buffer_memory->size(), chunk_size)) {
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
    std::unique_ptr<SharedMemory> ring_buffer_memory,
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
  // In kPending, writers published without drain requests. One drain covers
  // all data published so far.
  endpoint_->DrainRingBuffer();
}

void ProducerRingBufferEndpoint::Disconnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  SetReaderState(ReaderState::kDetached);
}

std::unique_ptr<TraceWriter> ProducerRingBufferEndpoint::CreateTraceWriter(
    BufferID target_buffer,
    BufferExhaustedPolicy policy) {
  if (reader_state_.load() == ReaderState::kDetached)
    return std::make_unique<NullTraceWriter>();
  const WriterID writer_id =
      shared_memory_arbiter_->AllocateTracingV2WriterID();
  if (!writer_id)
    return std::make_unique<NullTraceWriter>();
  return std::make_unique<TraceWriterV2Impl>(this, ring_buffer_.get(),
                                             writer_id, target_buffer, policy);
}

void ProducerRingBufferEndpoint::Flush(std::function<void()> callback) {
  if (!callback) {
    PostDrainTask();
    return;
  }
  // Send the drain request in the same task as the callback. The pending
  // drain task can come later than the callback. See PostDrainTask().
  auto weak = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak, callback = std::move(callback)] {
    if (weak)
      weak->SendDrainRequest();
    callback();
  });
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
      task_runner_->RunsTasksOnCurrentThread() &&
      reader_state_.load() == ReaderState::kAttached) {
    endpoint_->DrainRingBuffer();
    return;
  }
  PostDrainTask();
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

void ProducerRingBufferEndpoint::PostDrainTask() {
  // All writers share one pending drain task.
  //
  // |drain_task_pending_| is true from the moment a writer claims the task
  // until the task runs. Other writers then post nothing. The pending task
  // still drains their chunks:
  //
  //   writer thread               endpoint thread (the pending task)
  //   -------------               ----------------------------------
  //   W1. publish the chunk       E1. clear the flag
  //   W2. read the flag           E2. send DrainRingBuffer
  //       - set: post nothing
  //       - clear: post a task
  //
  // If W2 sees the flag set, E1 did not run yet. So E2 runs after W1, and
  // the service reads the chunk.
  //
  // The pending task can run late:
  // - A writer sets the flag first. It posts the task after that.
  // - Between these two steps, the task is not in the queue yet.
  // - A task that another caller posts at that time runs first.
  // So Flush() does not wait for the pending task. It sends its own drain.
  //
  // |reader_state_| works the same way. A writer that sees kPending posts
  // nothing. OnReaderAttached() drains its chunk later.
  if (drain_task_pending_.load() ||
      reader_state_.load() != ReaderState::kAttached ||
      drain_task_pending_.exchange(true)) {
    return;
  }
  auto weak = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak] {
    if (weak)
      weak->SendDrainRequest();
  });
}

void ProducerRingBufferEndpoint::SendDrainRequest() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Clear the flag before the drain request.
  drain_task_pending_.store(false);
  if (reader_state_.load() == ReaderState::kAttached)
    endpoint_->DrainRingBuffer();
}

}  // namespace perfetto::tracing_v2
