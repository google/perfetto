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
  // Validate geometry before constructing the ring, whose ctor CHECKs the same
  // layout: a producer/service disagreement here is a programming error, not
  // untrusted input, but returning null keeps callers non-fatal.
  if (!IsValidRingGeometry(num_chunks, chunk_size))
    return nullptr;
  if (!memory || memory->size() < LogicalSize(num_chunks, chunk_size))
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
      channel_(channel) {
  PERFETTO_DCHECK(channel_);
}

ProducerRing::~ProducerRing() = default;

std::unique_ptr<TraceWriter> ProducerRing::CreateTraceWriter(
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy) {
  WriterID writer_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_writer_id_ > kMaxWriterID) {
      // Writer-id space exhausted. Ids are monotonic and not reused within a
      // connection, so a new writer can never splice onto a retired writer's
      // data still retained in the trace buffer. Do not stop tracing silently:
      // count the refusal and log it once. Existing writers keep working, and a
      // reconnect starts a fresh id space.
      if (writer_id_exhausted_count_++ == 0) {
        PERFETTO_ELOG(
            "tracing v2: writer-id space exhausted on this connection; new "
            "writers get a NullTraceWriter until the producer reconnects");
      }
      return std::unique_ptr<TraceWriter>(new NullTraceWriter());
    }
    writer_id = next_writer_id_++;
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
}

void ProducerRing::NotifyReader() {
  // Progress-critical. A writer could not claim a chunk and is about to park on
  // the ring futex, or it published a continuation that filled the ring mid
  // packet. Ask the service to drain every time -- this path is never
  // coalesced. When the service reads on this same sequence (in process),
  // NotifyRingData() drains synchronously and frees space before the writer
  // parks. Each fill must get its own drain, so a coalescing gate here would
  // strand a writer that fills the ring several times in a row (e.g. a large
  // write from OnFlush on the service sequence). Steady-state per-packet
  // notifications go through NotifyReaderBatched(), which coalesces.
  //
  // The lock is held across the call so it serializes with DetachFromService():
  // once |channel_| is null no further notification is sent, and the channel
  // cannot be destroyed mid-call. An in-process channel may drain inline here;
  // that only reads the ring and admits into the trace buffer, never
  // re-entering this lock.
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel_)
    channel_->NotifyRingData({});
}

void ProducerRing::NotifyReaderBatched() {
  // Steady-state: a packet finished without needing more space, so the reader
  // can drain when convenient. Coalesce a burst into one outstanding
  // notification. |batched_notify_dirty_| records that more packets arrived
  // while a drain was in flight; OnBatchedNotifyDrained() rechecks it so the
  // last packet is never stranded. This never gates the progress-critical
  // NotifyReader() path above.
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
  // Recheck: if packets finalized while the drain was in flight, send one more
  // notification so their data is drained. Detach clears |channel_|, which
  // stops the chain.
  if (batched_notify_dirty_ && channel_)
    SendBatchedNotifyLocked();
}

bool ProducerRing::DrainRunsOnCurrentThread() {
  std::lock_guard<std::mutex> lock(mutex_);
  return channel_ && channel_->DrainRunsOnCurrentThread();
}

void ProducerRing::Flush(WriterID, std::function<void()> callback) {
  // The writer has already published its data. Ask the service to drain, then
  // run the flush callback once it has.
  std::unique_lock<std::mutex> lock(mutex_);
  if (channel_) {
    channel_->NotifyRingData(std::move(callback));
    return;
  }
  // Detached: the service can no longer complete the flush. TraceWriter::Flush
  // allows discarding the callback on disconnect; run it so callers do not
  // hang.
  lock.unlock();
  if (callback)
    callback();
}

void ProducerRing::OnWriterDestroyed(WriterID) {
  // Ids are monotonic and not reused, so there is nothing to retire here. Nudge
  // the service so the writer's final published data is drained promptly.
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel_)
    channel_->NotifyRingData({});
}

}  // namespace tracing_v2
}  // namespace perfetto
