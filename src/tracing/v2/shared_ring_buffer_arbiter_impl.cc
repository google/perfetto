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

#include "src/tracing/v2/shared_ring_buffer_arbiter_impl.h"

#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "src/tracing/core/null_trace_writer.h"
#include "src/tracing/v2/trace_writer_v2_impl.h"

namespace perfetto::tracing_v2 {

SharedRingBufferArbiterImpl::SharedRingBufferArbiterImpl(
    base::TaskRunner* task_runner,
    ProducerEndpoint* endpoint,
    SharedMemoryArbiter* shared_memory_arbiter)
    : task_runner_(task_runner),
      endpoint_(endpoint),
      shared_memory_arbiter_(shared_memory_arbiter) {}

SharedRingBufferArbiterImpl::~SharedRingBufferArbiterImpl() {
  Disconnect();
}

bool SharedRingBufferArbiterImpl::OfferRingBuffer(
    std::unique_ptr<SharedMemory> memory,
    uint32_t chunk_size) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (state() != State::kIdle)
    return false;
  if (!memory || !memory->start() ||
      memory->size() > TracingService::kMaxShmSize ||
      reinterpret_cast<uintptr_t>(memory->start()) %
          alignof(RingBufferHeader) ||
      !IsSupportedChunkSize(chunk_size) ||
      !NumChunksForRingLayout(memory->size(), chunk_size)) {
    Disconnect();
    return false;
  }
  memory_ = std::move(memory);
  ring_ = std::make_unique<SharedRingBuffer>(
      static_cast<uint8_t*>(memory_->start()), memory_->size(), chunk_size);
  SetState(State::kOffered);
  return true;
}

std::unique_ptr<TraceWriter> SharedRingBufferArbiterImpl::CreateTraceWriter(
    BufferID buffer,
    BufferExhaustedPolicy policy) {
  const State current = state();
  if (current != State::kOffered && current != State::kAccepted)
    return nullptr;
  WriterID writer_id = shared_memory_arbiter_->AllocateExternalWriterID();
  if (!writer_id)
    return std::make_unique<NullTraceWriter>();
  return std::make_unique<TraceWriterV2Impl>(this, ring_.get(), writer_id,
                                             buffer, policy);
}

void SharedRingBufferArbiterImpl::OnWriterDestroyed(WriterID id) {
  // This release can allow the endpoint to die. Do not access members after it.
  shared_memory_arbiter_->ReleaseExternalWriterID(id);
}

SharedRingBufferArbiterImpl::State SharedRingBufferArbiterImpl::state() const {
  return state_.load(std::memory_order_acquire);
}

bool SharedRingBufferArbiterImpl::CanAcquireChunks() const {
  const State current = state();
  return current == State::kOffered || current == State::kAccepted;
}

bool SharedRingBufferArbiterImpl::ShouldWaitForReader() const {
  return state() == State::kAccepted;
}

void SharedRingBufferArbiterImpl::SetState(State next) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  const State current = state();
  PERFETTO_CHECK(next == State::kDisconnected ||
                 (current == State::kIdle && next == State::kOffered) ||
                 (current == State::kOffered && next == State::kAccepted));
  state_.store(next, std::memory_order_release);
}

void SharedRingBufferArbiterImpl::SetAccepted() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (state() != State::kOffered)
    return;
  SetState(State::kAccepted);
  NotifyReader();
  auto callbacks = std::move(pending_flushes_);
  pending_flushes_.clear();
  for (auto& callback : callbacks)
    Flush(std::move(callback));
}

void SharedRingBufferArbiterImpl::Disconnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  SetState(State::kDisconnected);
  auto callbacks = std::move(pending_flushes_);
  pending_flushes_.clear();
  for (auto& callback : callbacks)
    task_runner_->PostTask(std::move(callback));
}

void SharedRingBufferArbiterImpl::NotifyReader() {
  if (notification_pending_.load(std::memory_order_relaxed) ||
      state() != State::kAccepted ||
      notification_pending_.exchange(true, std::memory_order_acq_rel))
    return;
  auto weak = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak] {
    if (weak)
      weak->SendNotification();
  });
}

void SharedRingBufferArbiterImpl::SendNotification() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  notification_pending_.store(false, std::memory_order_release);
  if (state() == State::kAccepted)
    endpoint_->DrainRingBuffer();
}

void SharedRingBufferArbiterImpl::TryMakeReaderProgress() {
  if (state() != State::kAccepted)
    return;
  if (task_runner_->RunsTasksOnCurrentThread()) {
    DrainForStall();
    return;
  }
  auto weak = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak] {
    if (weak)
      weak->DrainForStall();
  });
}

void SharedRingBufferArbiterImpl::DrainForStall() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (state() != State::kAccepted)
    return;
  // DrainRingBuffer has no reply. A reader protocol error can no longer
  // disconnect the arbiter from this path. A stalled writer times out on
  // its own stall deadline.
  endpoint_->DrainRingBuffer();
}

void SharedRingBufferArbiterImpl::Flush(std::function<void()> callback) {
  if (!callback) {
    NotifyReader();
    return;
  }
  auto weak = weak_factory_.GetWeakPtr();
  task_runner_->PostTask([weak, callback = std::move(callback)]() mutable {
    if (weak)
      weak->FlushOnEndpoint(std::move(callback));
    else
      callback();
  });
}

void SharedRingBufferArbiterImpl::FlushOnEndpoint(
    std::function<void()> callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  switch (state()) {
    case State::kIdle:
    case State::kDisconnected:
      callback();
      return;
    case State::kOffered:
      pending_flushes_.push_back(std::move(callback));
      return;
    case State::kAccepted:
      break;
  }
  // DrainRingBuffer has no reply. FlushPendingCommitDataRequests sends a
  // CommitData with a flush id. IPC frames on one connection are ordered,
  // so the service processes the drain before the commit. The commit
  // callback then fires the caller callback after both complete.
  endpoint_->DrainRingBuffer();
  shared_memory_arbiter_->FlushPendingCommitDataRequests(std::move(callback));
}

}  // namespace perfetto::tracing_v2
