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

#include "src/tracing/v2/in_process_tracing_v2_bridge.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/tracing/v2/proto_rewriter.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

using ::perfetto::protos::pbzero::TracePacket;

// Bound each relay task so a busy bridge cannot starve the others.
constexpr uint32_t kMaxPositionsPerPass = 256;

// Reject oversized packets before malformed input can grow this without bound.
// TODO(sashwinbalaji): use a total per-producer budget in traced.
constexpr size_t kMaxPacketSize = protozero::proto_utils::kMaxMessageLength;

// Match the maximum v1 SMB size.
constexpr uint64_t kMaxChunkStorage = 32 * 1024 * 1024;

void AddDataLoss(uint32_t* pending_data_loss, uint32_t reason = 0) {
  // Accumulate loss reasons until a packet reaches v1.
  *pending_data_loss |= TracePacket::DATA_LOSS_PRESENT | reason;
}

}  // namespace

// static
uint32_t InProcessTracingV2Bridge::NumChunksForCapacity(size_t capacity_bytes,
                                                        uint32_t chunk_size) {
  PERFETTO_CHECK(chunk_size > 0);
  const uint64_t chunks_that_fit = uint64_t{capacity_bytes} / chunk_size;
  if (chunks_that_fit == 0)
    return 0;

  // Limit the ring buffer to SharedRingBuffer's maximum chunk count.
  const uint64_t capped =
      std::min<uint64_t>(chunks_that_fit, kMaxChunksPerRing);

  // Chunk lookup uses a bit mask, so choose the largest power of two that fits.
  uint32_t num_chunks =
      base::RoundUpToPowerOfTwo(static_cast<uint32_t>(capped));
  if (num_chunks > capped)
    num_chunks /= 2;
  return num_chunks;
}

// static
std::shared_ptr<InProcessTracingV2Bridge> InProcessTracingV2Bridge::Create(
    std::shared_ptr<RelaySequence> relay,
    uint32_t num_chunks,
    uint32_t chunk_size) {
  PERFETTO_CHECK(relay);
  // SharedRingBuffer validates the remaining layout constraints.
  PERFETTO_CHECK(num_chunks > 0);
  const uint64_t chunk_storage = uint64_t{num_chunks} * uint64_t{chunk_size};
  // This also keeps the size_t cast below safe on 32-bit.
  PERFETTO_CHECK(chunk_storage <= kMaxChunkStorage);
  const uint64_t ring_size = uint64_t{sizeof(RingBufferHeader)} + chunk_storage;
  // Zero-filled memory is a valid empty ring buffer: positions at 0, all chunks
  // in Free(0).
  base::PagedMemory ring_memory =
      base::PagedMemory::Allocate(static_cast<size_t>(ring_size));
  return std::shared_ptr<InProcessTracingV2Bridge>(new InProcessTracingV2Bridge(
      std::move(ring_memory), chunk_size, std::move(relay)));
}

InProcessTracingV2Bridge::InProcessTracingV2Bridge(
    base::PagedMemory ring_memory,
    uint32_t chunk_size,
    std::shared_ptr<RelaySequence> relay)
    : ring_memory_(std::move(ring_memory)),
      ring_buffer_(static_cast<uint8_t*>(ring_memory_.Get()),
                   ring_memory_.size(),
                   chunk_size),
      ring_buffer_reader_(&ring_buffer_, this),
      relay_(std::move(relay)) {
  PERFETTO_DETACH_FROM_THREAD(thread_checker_);
}

// May run on any thread. Relay tasks hold a reference to the bridge, so none
// can still be running when destruction begins.
InProcessTracingV2Bridge::~InProcessTracingV2Bridge() = default;

std::unique_ptr<TraceWriter> InProcessTracingV2Bridge::CreateTraceWriter(
    std::unique_ptr<TraceWriter> v1_writer,
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy) {
  const WriterID writer_id = v1_writer->writer_id();
  // WriterID 0 is an unusable writer returned after downstream allocation
  // fails.
  if (writer_id == 0)
    return v1_writer;

  // Keep the v1 writer for the relay, indexed by its existing WriterID.
  auto state = std::unique_ptr<WriterState>(new WriterState());
  state->v1_writer = std::move(v1_writer);
  state->target_buffer = target_buffer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto inserted = writers_.Insert(writer_id, std::move(state));
    PERFETTO_CHECK(inserted.second);
  }

  // Publish into the ring buffer with the same WriterID.
  TraceWriterV2::InitArgs args{};
  args.delegate = shared_from_this();
  args.writer_id = writer_id;
  args.target_buffer = target_buffer;
  args.buffer_exhausted_policy = buffer_exhausted_policy;
  return std::unique_ptr<TraceWriter>(new TraceWriterV2(args));
}

// --- TraceWriterV2::Delegate: SDK writer threads. ---

SharedRingBuffer& InProcessTracingV2Bridge::ring_buffer() {
  return ring_buffer_;
}

void InProcessTracingV2Bridge::NotifyReader() {
  // Coalesce notifications into one relay task. The exchanges make each
  // writer's ring buffer updates visible to the drain. The task clears the flag
  // before draining so later writes can schedule another pass. A failed post
  // leaves the flag set.
  if (drain_scheduled_.exchange(true, std::memory_order_acq_rel))
    return;
  relay_->PostTask([self = shared_from_this()] {
    PERFETTO_DCHECK_THREAD(self->thread_checker_);
    self->drain_scheduled_.exchange(false, std::memory_order_acq_rel);
    const auto result = self->ring_buffer_reader_.Drain(kMaxPositionsPerPass);
    if (result.needs_another_drain())
      self->NotifyReader();
  });
}

// --- SharedRingBufferReader::Delegate: relay sequence. ---

// Continuation joins chunks from the same WriterID. Other writers may reserve
// positions between them:
//
//   writer A, earlier chunk: [ frag a | frag b... ]  ContinuesOnNext
//   writer B:                [ another packet ]
//   writer A, next chunk:    [ ...frag b | frag c ]  ContinuesFromPrev
void InProcessTracingV2Bridge::OnChunkRead(
    const SharedRingBufferReader::ChunkContents& contents) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  WriterState* state = FindWriterState(contents.writer_id);
  if (PERFETTO_UNLIKELY(!state)) {
    PERFETTO_DFATAL("No downstream v1 writer for tracing v2 WriterID %u",
                    contents.writer_id);
    return;
  }

  if (PERFETTO_UNLIKELY(state->target_buffer != contents.target_buffer)) {
    DiscardCurrentPacket(state);
    PERFETTO_DFATAL("Tracing v2 writer %u changed target buffer",
                    contents.writer_id);
    return;
  }

  if ((contents.payload_flags & kFlagDataLoss) != 0) {
    // The v2 ring buffer is shared-memory buffering too, so reuse the existing
    // reason.
    AddDataLoss(&state->pending_data_loss, TracePacket::DATA_LOSS_SMB_FULL);
  }

  for (uint32_t i = 0; i < contents.num_fragments; ++i) {
    const SharedRingBufferReader::Fragment& fragment = contents.fragments[i];
    const bool is_first = i == 0;
    const bool is_last = i + 1 == contents.num_fragments;
    const bool continues_from_prev =
        is_first && (contents.payload_flags & kFlagContinuesFromPrevChunk) != 0;
    const bool continues_on_next =
        is_last && (contents.payload_flags & kFlagContinuesOnNextChunk) != 0;

    if (!continues_from_prev) {
      // A promised continuation did not arrive.
      if (state->expecting_continuation) {
        AddDataLoss(&state->pending_data_loss,
                    TracePacket::DATA_LOSS_REASSEMBLY_GAP);
      }
      state->partial_packet.clear();
      state->discarding_packet = false;
    } else if (!state->expecting_continuation) {
      // The packet's first fragment did not arrive.
      state->discarding_packet = true;
      AddDataLoss(&state->pending_data_loss,
                  TracePacket::DATA_LOSS_ORPHAN_CONTINUATION);
    }

    PERFETTO_DCHECK(state->partial_packet.size() <= kMaxPacketSize);
    if (!state->discarding_packet &&
        fragment.size > kMaxPacketSize - state->partial_packet.size()) {
      state->discarding_packet = true;
      AddDataLoss(&state->pending_data_loss);
    }
    state->expecting_continuation = continues_on_next;
    if (state->discarding_packet) {
      state->partial_packet.clear();
      state->discarding_packet = continues_on_next;
      continue;
    }

    state->partial_packet.insert(state->partial_packet.end(), fragment.data,
                                 fragment.data + fragment.size);
    if (!continues_on_next)
      ForwardPacket(state);
  }
}

void InProcessTracingV2Bridge::OnDataLoss(WriterID writer_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  WriterState* state = FindWriterState(writer_id);
  if (!state)
    return;

  DiscardCurrentPacket(state);
}

InProcessTracingV2Bridge::WriterState*
InProcessTracingV2Bridge::FindWriterState(WriterID writer_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  std::lock_guard<std::mutex> lock(mutex_);
  auto* entry = writers_.Find(writer_id);
  return entry ? entry->get() : nullptr;
}

void InProcessTracingV2Bridge::DiscardCurrentPacket(WriterState* state) {
  state->partial_packet.clear();
  // We don't know where the rejected chunk's packet ended. Keep discarding
  // until a fragment that starts a new packet shows up.
  state->expecting_continuation = false;
  state->discarding_packet = true;
  AddDataLoss(&state->pending_data_loss,
              TracePacket::DATA_LOSS_CHUNK_CORRUPTED);
}

void InProcessTracingV2Bridge::ForwardPacket(WriterState* state) {
  // Avoid pointer arithmetic on a null data() for an empty packet.
  const std::vector<uint8_t>& packet_bytes = state->partial_packet;
  const uint8_t* const begin =
      packet_bytes.empty() ? nullptr : packet_bytes.data();
  const uint8_t* const end = begin ? begin + packet_bytes.size() : nullptr;
  const RewriteResult rewrite_result = RewriteProtoGroupToLengthDelimited(
      begin, end, &rewritten_packet_, kMaxPacketSize);

  state->partial_packet.clear();
  switch (rewrite_result) {
    case RewriteResult::kSuccess:
      break;
    case RewriteResult::kMalformedInput:
      AddDataLoss(&state->pending_data_loss,
                  TracePacket::DATA_LOSS_CHUNK_CORRUPTED);
      return;
    case RewriteResult::kOutputTooLarge:
      AddDataLoss(&state->pending_data_loss);
      return;
  }

  {
    auto packet = state->v1_writer->NewTracePacket();
    if (!rewritten_packet_.empty()) {
      packet->AppendRawProtoBytes(rewritten_packet_.data(),
                                  rewritten_packet_.size());
    }
    if (state->pending_data_loss != 0) {
      // Appended after the payload: with proto last-value-wins this overrides
      // any previous_packet_dropped the writer itself set.
      packet->set_previous_packet_dropped(state->pending_data_loss);
      state->pending_data_loss = 0;
    }
  }  // The handle's destructor finalizes the packet.
  state->has_unflushed_v1_data = true;
}

// --- Barrier submission: muxer and SDK writer threads. ---

void InProcessTracingV2Bridge::DrainPendingData(
    std::function<void()> completion) {
  // Copy the completion callback into the barrier. If posting fails, invoke
  // the original callback here.
  const bool queued = EnqueueBarrier(BarrierType::kFlushDirtyWriters,
                                     /*writer_id=*/0, completion);
  if (!queued && completion) {
    completion();
  }
}

void InProcessTracingV2Bridge::Flush(WriterID writer_id,
                                     std::function<void()> callback) {
  // Disconnect may discard a writer flush, but cannot acknowledge its data.
  EnqueueBarrier(BarrierType::kFlushWriter, writer_id, std::move(callback));
}

void InProcessTracingV2Bridge::OnWriterDestroyed(WriterID writer_id) {
  // The queued task keeps the bridge and WriterID alive until the drain. If
  // admission is closed, the retained writer dies with the bridge instead.
  EnqueueBarrier(BarrierType::kDestroyWriter, writer_id, {});
}

bool InProcessTracingV2Bridge::EnqueueBarrier(
    BarrierType type,
    WriterID writer_id,
    std::function<void()> completion) {
  Barrier barrier;
  // The caller has synchronized with the writers whose preceding data this
  // flush/stop covers, so this snapshot includes at least their reservations.
  // Seeing a newer position only adds work to the barrier. Chunk state, rather
  // than this position, determines which reservations have published payload.
  barrier.drain_target_pos = ring_buffer_.LoadWritePos();
  barrier.type = type;
  barrier.writer_id = writer_id;
  barrier.completion = std::move(completion);
  return relay_->PostTask(
      [self = shared_from_this(), barrier = std::move(barrier)]() mutable {
        PERFETTO_DCHECK_THREAD(self->thread_checker_);
        self->pending_control_barriers_.push_back(std::move(barrier));
        // The front barrier drives the queue.
        if (self->pending_control_barriers_.size() == 1)
          self->RunFrontBarrier();
      });
}

// --- Barrier execution: relay sequence. ---

bool InProcessTracingV2Bridge::DrainBarrierBatch(uint32_t target_pos) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // An ordinary drain may already have passed the sampled target.
  const int32_t positions_to_target =
      static_cast<int32_t>(target_pos - ring_buffer_reader_.read_pos());
  if (positions_to_target <= 0)
    return true;

  const uint32_t remaining = static_cast<uint32_t>(positions_to_target);
  const uint32_t batch_size = std::min(remaining, kMaxPositionsPerPass);
  const auto result = ring_buffer_reader_.Drain(batch_size);
  if (result.positions_consumed == remaining)
    return true;
  if (result.needs_another_drain())
    return false;

  // The reader already logged a latched protocol error. Abandon this barrier
  // without repeating that diagnostic for every later flush.
  if (result.last_result ==
      SharedRingBufferReader::ConsumeResult::kProtocolError) {
    return true;
  }
  PERFETTO_DFATAL("Tracing v2 barrier target is ahead of write_pos");
  return true;
}

void InProcessTracingV2Bridge::RunFrontBarrier() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  PERFETTO_DCHECK(!pending_control_barriers_.empty());
  if (!DrainBarrierBatch(pending_control_barriers_.front().drain_target_pos)) {
    // Yield between batches so one bridge cannot monopolize the relay.
    relay_->PostTask([self = shared_from_this()] { self->RunFrontBarrier(); });
    return;
  }

  // Finish queue bookkeeping before invoking any writer or completion callback.
  Barrier barrier = std::move(pending_control_barriers_.front());
  pending_control_barriers_.pop_front();
  switch (barrier.type) {
    case BarrierType::kFlushWriter: {
      // Registration precedes every writer request. The queued destruction
      // removes the entry after its earlier requests. Even a clean writer must
      // request an ACK.
      WriterState* state = FindWriterState(barrier.writer_id);
      PERFETTO_CHECK(state);
      state->v1_writer->Flush(std::move(barrier.completion));
      state->has_unflushed_v1_data = false;
      break;
    }
    case BarrierType::kFlushDirtyWriters: {
      // Snapshot under the registration lock, but call writers outside it.
      // Only this sequence removes entries, so their pointers remain valid.
      std::vector<WriterState*> writers;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = writers_.GetIterator(); it; ++it) {
          if (it.value()->has_unflushed_v1_data)
            writers.push_back(it.value().get());
        }
      }
      for (WriterState* state : writers) {
        state->v1_writer->Flush();
        state->has_unflushed_v1_data = false;
      }

      // Startup tracing cannot share this connection, so the bound arbiter
      // posts CommitData before Flush() returns. The completion's muxer task
      // lands behind those commits without waiting for their service ACKs.
      if (barrier.completion)
        barrier.completion();
      break;
    }
    case BarrierType::kDestroyWriter: {
      std::unique_ptr<WriterState> state;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* entry = writers_.Find(barrier.writer_id);
        PERFETTO_CHECK(entry);
        state = std::move(*entry);
        writers_.Erase(barrier.writer_id);
      }
      // The drain covered this writer's final chunk. Flush and destroy outside
      // the lock. The v1 writer retains its WriterID until destruction.
      if (state->has_unflushed_v1_data)
        state->v1_writer->Flush();
      break;
    }
  }

  if (pending_control_barriers_.empty())
    return;

  // Yield between batches and barriers. Closing the relay drops this task and
  // leaves the remaining barriers incomplete.
  // Only this method removes the front and posts at most one continuation.
  // Enqueue tasks can append work but cannot advance a non-empty queue.
  relay_->PostTask([self = shared_from_this()] { self->RunFrontBarrier(); });
}

}  // namespace perfetto::tracing_v2
