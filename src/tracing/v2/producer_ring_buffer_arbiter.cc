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

#include "src/tracing/v2/producer_ring_buffer_arbiter.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/ext/tracing/core/shared_memory_arbiter.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/tracing/core/null_trace_writer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/trace_writer_v2_impl.h"

namespace perfetto::tracing_v2 {

ProducerRingBufferArbiter::ProducerRingBufferArbiter(
    base::TaskRunner* task_runner,
    ProducerEndpoint* endpoint,
    AllocateMemoryFn allocate_memory)
    : task_runner_(task_runner),
      endpoint_(endpoint),
      allocate_memory_(std::move(allocate_memory)) {}

ProducerRingBufferArbiter::~ProducerRingBufferArbiter() {
  Disconnect();
}

// -----------------------------------------------------------------------------
// Instances and the ring buffer (endpoint thread)
// -----------------------------------------------------------------------------

void ProducerRingBufferArbiter::SetupInstance(
    DataSourceInstanceID id,
    const DataSourceConfig& config,
    const base::FlatSet<ProtocolAbiVersion>& protocol_abi_versions,
    SharedMemoryArbiter* arbiter,
    size_t size_budget) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Both endpoints create the SMB arbiter before they set up data sources.
  PERFETTO_CHECK(arbiter);

  // Decide once per instance, at setup. The service already rejects a
  // probability above 100, and a drain percent outside [-1, 100].
  const auto& experiment = config.experimental_tracing_v2();
  const uint32_t probability = experiment.use_v2_probability_percent();
  const bool use_ring_buffer =
      protocol_abi_versions.count(ProtocolAbiVersion::kV2) &&
      config.supports_tracing_v2() && probability > 0 &&
      (probability >= 100 ||
       static_cast<uint64_t>(base::Uuidv4().lsb()) % 100 < probability);
  if (!use_ring_buffer) {
    if (!protocol_abi_versions.count(ProtocolAbiVersion::kV1)) {
      PERFETTO_ELOG(
          "Data source \"%s\" has no permitted transport: "
          "v2 was not selected and v1 is not in the common set",
          config.name().c_str());
    }
    return;
  }
  {
    std::lock_guard<base::MaybeRtMutex> lock(mutex_);
    ring_buffer_instances_[id] = experiment.drain_occupancy_percent();
  }

  // Later instances reuse the ring buffer. After a failed allocation, try
  // again.
  if (state_.load() == State::kNoRingBuffer)
    CreateAndAttachRingBuffer(config, arbiter, size_budget);
}

void ProducerRingBufferArbiter::CreateAndAttachRingBuffer(
    const DataSourceConfig& config,
    SharedMemoryArbiter* arbiter,
    size_t size_budget) {
  // The service rejects an invalid chunk size, and maps at most kMaxShmSize.
  const auto& experiment = config.experimental_tracing_v2();
  const uint32_t chunk_size = experiment.has_chunk_size_bytes()
                                  ? experiment.chunk_size_bytes()
                                  : kMinChunkSize;
  const std::optional<size_t> size = RingBufferSizeForBudget(
      std::min(size_budget ? size_budget : kDefaultSizeBudget,
               size_t{TracingService::kMaxShmSize}),
      chunk_size);
  if (!size) {
    PERFETTO_ELOG(
        "Tracing v2: no ring buffer of %u-byte chunks fits the budget",
        chunk_size);
    return;
  }
  std::shared_ptr<SharedMemory> memory = allocate_memory_(*size);
  if (!memory) {
    PERFETTO_ELOG("Failed to allocate tracing v2 shared memory");
    return;
  }
  // A local bug: the size and the chunk size give a valid layout.
  PERFETTO_CHECK(
      NumChunksForRingBufferLayout(memory->start(), memory->size(), chunk_size)
          .ok());

  // Set the ring buffer before the state leaves kNoRingBuffer. Writers exist
  // only after that.
  shared_memory_arbiter_ = arbiter;
  memory_ = memory;
  ring_buffer_ = std::make_unique<SharedRingBuffer>(
      static_cast<uint8_t*>(memory_->start()), memory_->size(), chunk_size);
  SetState(State::kPending);

  // In-process, the endpoint replies inline.
  endpoint_->AttachV2RingBuffer(
      std::move(memory), chunk_size,
      [weak_this = weak_factory_.GetWeakPtr()](bool accepted) {
        if (weak_this)
          weak_this->OnAttachReply(accepted);
      });
}

void ProducerRingBufferArbiter::OnAttachReply(bool accepted) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // The reply can arrive after Disconnect().
  if (state_.load() == State::kDetached)
    return;
  if (accepted) {
    SetState(State::kAttached);
    return;
  }
  // Also on an ordinary disconnect: the pending reply is rejected.
  PERFETTO_DLOG("Tracing v2 ring buffer not accepted");
  SetState(State::kDetached);
}

void ProducerRingBufferArbiter::OnInstanceStopped(DataSourceInstanceID id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  std::lock_guard<base::MaybeRtMutex> lock(mutex_);
  ring_buffer_instances_.erase(id);
}

void ProducerRingBufferArbiter::Disconnect() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  SetState(State::kDetached);
}

void ProducerRingBufferArbiter::SetState(State next) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  const State current = state_.load();
  PERFETTO_CHECK(next == State::kDetached ||
                 (current == State::kNoRingBuffer && next == State::kPending) ||
                 (current == State::kPending && next == State::kAttached));
  state_.store(next);
}

// -----------------------------------------------------------------------------
// Writers (any thread)
// -----------------------------------------------------------------------------

std::unique_ptr<TraceWriter> ProducerRingBufferArbiter::MaybeCreateTraceWriter(
    BufferID target_buffer,
    BufferExhaustedPolicy policy,
    DataSourceInstanceID id) {
  int32_t drain_occupancy_percent;
  {
    std::lock_guard<base::MaybeRtMutex> lock(mutex_);
    auto it = ring_buffer_instances_.find(id);
    if (it == ring_buffer_instances_.end())
      return nullptr;
    drain_occupancy_percent = it->second;
  }

  // No v1 fallback for a v2 instance.
  const State state = state_.load();
  if (PERFETTO_UNLIKELY(state == State::kNoRingBuffer ||
                        state == State::kDetached)) {
    return std::make_unique<NullTraceWriter>();
  }

  const WriterID writer_id =
      shared_memory_arbiter_->AllocateTracingV2WriterID();
  if (PERFETTO_UNLIKELY(!writer_id))
    return std::make_unique<NullTraceWriter>();

  return std::make_unique<TraceWriterV2Impl>(
      this, writer_id, target_buffer, policy,
      SharedRingBufferWriter::DrainThresholdForPercent(
          ring_buffer_->num_chunks(), drain_occupancy_percent));
}

void ProducerRingBufferArbiter::Flush(std::function<void()> callback) {
  // Without a ring buffer, there is nothing to drain. Writers exist only
  // with one, so this is an endpoint call.
  if (!ring_buffer_) {
    if (callback)
      callback();
    return;
  }

  // Set force=true to queue our own drain task before the callback. This
  // ensures the drain request is sent before the callback runs.
  //
  // Sharing another writer's drain task would leave a race:
  // 1. Another writer sets the flag, then pauses before PostTask().
  // 2. Flush() sees the flag and queues only its callback.
  // 3. The callback runs before the other writer queues the drain task.
  PostDrainTask(/*force=*/true);

  if (callback)
    task_runner_->PostTask(std::move(callback));
}

void ProducerRingBufferArbiter::OnWriterDestroyed(WriterID id) {
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

void ProducerRingBufferArbiter::NotifyReader(NotifyReason reason) {
  // A posted task cannot run while a writer on the endpoint thread is
  // stalled. So send the request directly.
  if (reason == NotifyReason::kWriterStalled &&
      task_runner_->RunsTasksOnCurrentThread()) {
    endpoint_->DrainV2RingBuffer();
    return;
  }
  PostDrainTask(/*force=*/false);
}

bool ProducerRingBufferArbiter::IsReaderAttached() const {
  return state_.load() == State::kAttached;
}

// -----------------------------------------------------------------------------
// Drain requests
// -----------------------------------------------------------------------------

void ProducerRingBufferArbiter::PostDrainTask(bool force) {
  // When |force| is false, notifications share a pending drain task.
  // Setting |drain_task_pending_| after publishing ensures that task covers
  // the new data. The atomic operations below act on |drain_task_pending_|:
  //
  //   writer thread               endpoint thread (the pending task)
  //   -------------               ----------------------------------
  //   W1. publish the chunk       E1. store(0)
  //   W2. fetch_or(1)             E2. send DrainV2RingBuffer
  //       - was set: post nothing
  //       - was clear: post a task
  //
  // If W2 finds |drain_task_pending_| already set, a drain is still due after
  // W1. That request covers this publication too, so no extra task is needed.
  //
  // Flush() sets |force| to true to bypass merging and queue its own drain
  // task ahead of its callback.
  if (!force && drain_task_pending_.fetch_or(1))
    return;

  task_runner_->PostTask([weak_this = weak_factory_.GetWeakPtr()] {
    if (weak_this)
      weak_this->SendDrainRequest();
  });
}

void ProducerRingBufferArbiter::SendDrainRequest() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Let later publications queue another task before we send this request.
  drain_task_pending_.store(0);
  endpoint_->DrainV2RingBuffer();
}

}  // namespace perfetto::tracing_v2
