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

#include "src/tracing/v2/producer_ring.h"

#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "src/tracing/core/null_trace_writer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto {
namespace tracing_v2 {

ProducerRing::ServiceChannel::~ServiceChannel() = default;

// static
size_t ProducerRing::LogicalSize(uint32_t num_chunks, uint32_t chunk_size) {
  // The producer builds the ring within the service's size budget, so the
  // 64-bit extent always fits size_t here.
  return static_cast<size_t>(RingLogicalSize(num_chunks, chunk_size));
}

// static
std::shared_ptr<ProducerRing> ProducerRing::Create(
    std::shared_ptr<SharedMemory> memory,
    uint32_t num_chunks,
    uint32_t chunk_size,
    ServiceChannel* channel) {
  // Validate geometry before the constructor's fatal checks. Return null for
  // invalid geometry, so callers can handle a producer/service mismatch.
  if (!IsValidRingGeometry(num_chunks, chunk_size))
    return nullptr;
  if (!memory ||
      uint64_t(memory->size()) < RingLogicalSize(num_chunks, chunk_size))
    return nullptr;
  return std::shared_ptr<ProducerRing>(
      new ProducerRing(std::move(memory), num_chunks, chunk_size, channel));
}

ProducerRing::ProducerRing(std::shared_ptr<SharedMemory> memory,
                           uint32_t num_chunks,
                           uint32_t chunk_size,
                           ServiceChannel* channel)
    : memory_(std::move(memory)),
      num_chunks_(num_chunks),
      chunk_size_(chunk_size),
      // The ring is built over the exact logical extent, not the (page-rounded)
      // mapping size, so its geometry equation holds regardless of OS rounding.
      ring_(static_cast<uint8_t*>(memory_->start()),
            LogicalSize(num_chunks, chunk_size),
            chunk_size),
      channel_(channel),
      was_attached_(channel != nullptr) {}

ProducerRing::~ProducerRing() = default;

std::unique_ptr<TraceWriter> ProducerRing::CreateTraceWriter(
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy) {
  WriterID writer_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    writer_id = writer_ids_.Allocate();
    if (!writer_id) {
      // Pending retirements still reserve their IDs. Keep existing writers
      // running and report exhaustion once for this connection.
      if (writer_id_exhausted_count_++ == 0) {
        PERFETTO_ELOG(
            "tracing v2: writer-id space exhausted on this connection; new "
            "writers get a NullTraceWriter until an ID retires");
      }
      return std::unique_ptr<TraceWriter>(new NullTraceWriter());
    }
  }
  TraceWriterV2Impl::InitArgs args;
  args.delegate = shared_from_this();
  args.writer_id = writer_id;
  args.target_buffer = target_buffer;
  args.buffer_exhausted_policy = buffer_exhausted_policy;
  args.ring_buffer = &ring_;
  return std::unique_ptr<TraceWriter>(new TraceWriterV2Impl(args));
}

void ProducerRing::DetachFromService() {
  std::lock_guard<std::mutex> lock(mutex_);
  channel_ = nullptr;
  was_attached_ = true;
}

bool ProducerRing::AttachToService(ServiceChannel* channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!channel || was_attached_)
    return false;
  channel_ = channel;
  was_attached_ = true;
  channel_->NotifyRingData({});
  for (WriterID id : retired_before_attach_)
    RetireWriterLocked(id);
  retired_before_attach_.clear();
  return true;
}

void ProducerRing::NotifyReader() {
  // This writer needs space after a failed claim or a continuation fragment.
  // Request a drain every time. In process, NotifyRingData() can drain inline
  // on the service sequence before the writer waits on the ring futex.
  // A large OnFlush() write can fill the ring repeatedly on that sequence.
  // Each fill needs a separate drain to avoid deadlock.
  // NotifyReaderBatched() coalesces only notifications for complete packets.
  //
  // Hold the lock across the call to serialize with DetachFromService().
  // This prevents channel destruction during notification. After detach clears
  // |channel_|, no further notification can occur. An inline drain reads the
  // ring and stores fragments without reacquiring this lock.
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel_)
    channel_->NotifyRingData({});
}

void ProducerRing::NotifyReaderBatched() {
  // A complete packet needs no immediate space, so coalesce notifications.
  // |batched_notify_dirty_| records packets that arrive during an outstanding
  // drain. OnBatchedNotifyDrained() checks that flag to request another drain.
  // NotifyReader() bypasses this check when a writer needs space immediately.
  std::lock_guard<std::mutex> lock(mutex_);
  if (!channel_)
    return;
  batched_notify_dirty_ = true;
  if (batched_notify_in_flight_)
    return;
  SendBatchedNotifyLocked();
}

void ProducerRing::SendBatchedNotifyLocked() {
  batched_notify_in_flight_ = true;
  batched_notify_dirty_ = false;
  std::weak_ptr<ProducerRing> weak = weak_from_this();
  channel_->NotifyRingData([weak] {
    if (auto self = weak.lock())
      self->OnBatchedNotifyDrained();
  });
}

void ProducerRing::OnBatchedNotifyDrained() {
  std::lock_guard<std::mutex> lock(mutex_);
  batched_notify_in_flight_ = false;
  // If packets finalized during the drain, request another notification.
  // Detach clears |channel_| to prevent further requests.
  if (batched_notify_dirty_ && channel_)
    SendBatchedNotifyLocked();
}

bool ProducerRing::DrainRunsOnCurrentThread() {
  std::lock_guard<std::mutex> lock(mutex_);
  return !channel_ || channel_->DrainRunsOnCurrentThread();
}

void ProducerRing::Flush(WriterID, std::function<void()> callback) {
  // The writer has already published its data. Ask the service to drain, then
  // run the flush callback once it has.
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel_)
    channel_->NotifyRingData(std::move(callback));
  // Without a channel there is no service acknowledgement. Drop the callback,
  // as TraceWriter::Flush permits when the connection is unavailable.
}

void ProducerRing::OnWriterDestroyed(WriterID writer_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!channel_) {
    if (!was_attached_)
      retired_before_attach_.push_back(writer_id);
    return;
  }
  RetireWriterLocked(writer_id);
}

void ProducerRing::RetireWriterLocked(WriterID writer_id) {
  std::weak_ptr<ProducerRing> weak = weak_from_this();
  channel_->RetireWriter(writer_id, [weak, writer_id](bool success) {
    if (!success)
      return;
    if (auto self = weak.lock()) {
      std::lock_guard<std::mutex> guard(self->mutex_);
      self->writer_ids_.Free(writer_id);
    }
  });
}

}  // namespace tracing_v2
}  // namespace perfetto
