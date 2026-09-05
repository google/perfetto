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
#include "perfetto/protozero/proto_utils.h"
#include "src/tracing/v2/proto_rewriter.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

using ::perfetto::protos::pbzero::TracePacket;

// Prevent one busy ring from monopolizing the shared relay thread.
constexpr uint32_t kMaxPositionsPerPass = 256;

// Prevent malformed packets from growing relay buffers without bound.
// TODO(sashwinbalaji): use a total per-producer budget in traced.
constexpr size_t kMaxPacketSize = protozero::proto_utils::kMaxMessageLength;

// Same allocation cap as the v1 SMB.
constexpr uint64_t kMaxRingSize = 32 * 1024 * 1024;

}  // namespace

// static
std::shared_ptr<InProcessTracingV2Bridge> InProcessTracingV2Bridge::Create(
    base::TaskRunner* relay_task_runner,
    uint32_t num_chunks,
    uint32_t chunk_size) {
  PERFETTO_CHECK(relay_task_runner);
  // The cap also makes the size_t cast safe on 32-bit builds.
  const uint64_t ring_size = uint64_t{sizeof(RingBufferHeader)} +
                             uint64_t{num_chunks} * uint64_t{chunk_size};
  PERFETTO_CHECK(ring_size <= kMaxRingSize);
  // Zero-filled memory is an empty ring: positions and Free(0) are zero.
  base::PagedMemory ring_memory =
      base::PagedMemory::Allocate(static_cast<size_t>(ring_size));

  auto deleter = [relay_task_runner](InProcessTracingV2Bridge* bridge) {
    // Destruction is relay-only, but the last reference can drop on any SDK
    // thread. Task copies share one owner slot; the invoked copy moves that
    // reference onto the relay.
    auto owner =
        std::make_shared<std::shared_ptr<InProcessTracingV2Bridge>>(bridge);
    relay_task_runner->PostTask(
        [owner] { DeleteWhenQuiescent(std::move(*owner)); });
  };
  return std::shared_ptr<InProcessTracingV2Bridge>(
      new InProcessTracingV2Bridge(std::move(ring_memory), chunk_size,
                                   relay_task_runner),
      std::move(deleter));
}

InProcessTracingV2Bridge::InProcessTracingV2Bridge(
    base::PagedMemory ring_memory,
    uint32_t chunk_size,
    base::TaskRunner* relay_task_runner)
    : ring_memory_(std::move(ring_memory)),
      ring_buffer_(static_cast<uint8_t*>(ring_memory_.Get()),
                   ring_memory_.size(),
                   chunk_size),
      ring_buffer_reader_(&ring_buffer_, this),
      weak_runner_(relay_task_runner) {
  PERFETTO_DETACH_FROM_THREAD(thread_checker_);
}

InProcessTracingV2Bridge::~InProcessTracingV2Bridge() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
}

std::unique_ptr<TraceWriter> InProcessTracingV2Bridge::CreateTraceWriter(
    std::unique_ptr<TraceWriter> v1_writer,
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy) {
  PERFETTO_CHECK(v1_writer);
  const WriterID writer_id = v1_writer->writer_id();
  // Zero means the v1 arbiter could not allocate an id.
  if (writer_id == 0)
    return v1_writer;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_writers_)
      return v1_writer;
    auto state = std::unique_ptr<WriterState>(new WriterState());
    state->v1_writer = std::move(v1_writer);
    state->target_buffer = target_buffer;
    const auto inserted = writers_.Insert(writer_id, std::move(state));
    PERFETTO_CHECK(inserted.second);
  }

  TraceWriterV2::InitArgs args{};
  args.ring_buffer = &ring_buffer_;
  args.delegate = shared_from_this();
  args.writer_id = writer_id;
  args.target_buffer = target_buffer;
  args.buffer_exhausted_policy = buffer_exhausted_policy;
  return std::unique_ptr<TraceWriter>(new TraceWriterV2(args));
}

void InProcessTracingV2Bridge::NotifyReader() {
  ScheduleDrain();
}

void InProcessTracingV2Bridge::ScheduleDrain() {
  // Publish ring writes before advertising pending work.
  if (drain_scheduled_.exchange(true, std::memory_order_acq_rel))
    return;
  weak_runner_.PostTask([this] { DrainOnRelayThread(); });
}

void InProcessTracingV2Bridge::DrainOnRelayThread() {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  // Clear before draining so concurrent writes schedule another pass. The
  // acquire observes publications whose notifier found the flag already set.
  drain_scheduled_.exchange(false, std::memory_order_acq_rel);

  const SharedRingBufferReader::DrainResult result =
      ring_buffer_reader_.Drain(kMaxPositionsPerPass);
  if (result.needs_another_drain())
    ScheduleDrain();
}

bool InProcessTracingV2Bridge::DrainUpTo(uint32_t target_pos) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  for (;;) {
    // An ordinary drain may already have passed the sampled target.
    const int32_t positions_to_target =
        static_cast<int32_t>(target_pos - ring_buffer_reader_.read_pos());
    if (positions_to_target <= 0)
      return true;

    const SharedRingBufferReader::DrainResult result =
        ring_buffer_reader_.Drain(std::min(
            static_cast<uint32_t>(positions_to_target), kMaxPositionsPerPass));
    if (result.positions_resolved != 0)
      continue;

    if (result.last_result !=
        SharedRingBufferReader::ResolveResult::kRetryLater) {
      // Do not strand control behind a malformed ring position.
      PERFETTO_DLOG("Tracing v2 relay stopped at a malformed ring position");
      return true;
    }
    return false;
  }
}

// Continuation flags join the last fragment of one chunk to the first of the
// next:
//
//   chunk k   : [ frag a | frag b | frag c... ]  flags: ContinuesOnNext
//   chunk k+1 : [ ...frag c | frag d ]           flags: ContinuesFromPrev
void InProcessTracingV2Bridge::OnChunkRead(
    const SharedRingBufferReader::ChunkContents& contents) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Old SDK writers can outlive the endpoint; keep freeing ring capacity.
  if (stopped_forwarding_)
    return;

  WriterState* state = nullptr;
  {
    // Do not run v1 writer code under |mutex_|.
    std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<WriterState>* const entry =
        writers_.Find(contents.writer_id);
    if (entry)
      state = entry->get();
  }
  if (PERFETTO_UNLIKELY(!state)) {
    // Retirement drains every position that can name this WriterID.
    PERFETTO_DFATAL("Tracing v2 packet references unknown writer %u",
                    contents.writer_id);
    return;
  }
  if (PERFETTO_UNLIKELY(state->target_buffer != contents.target_buffer)) {
    PERFETTO_DFATAL("Tracing v2 writer %u changed target buffer",
                    contents.writer_id);
    return;
  }

  if ((contents.payload_flags & kFlagDataLoss) != 0) {
    state->pending_data_loss |= static_cast<uint32_t>(
        TracePacket::DATA_LOSS_PRESENT | TracePacket::DATA_LOSS_SMB_FULL);
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
        state->pending_data_loss |=
            static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT |
                                  TracePacket::DATA_LOSS_REASSEMBLY_GAP);
      }
      state->partial_packet.clear();
      state->discarding_packet = false;
    } else if (!state->expecting_continuation) {
      // The packet's first fragment did not arrive.
      state->discarding_packet = true;
      state->pending_data_loss |=
          static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT |
                                TracePacket::DATA_LOSS_ORPHAN_CONTINUATION);
    }

    if (!state->discarding_packet &&
        fragment.size > kMaxPacketSize - state->partial_packet.size()) {
      state->discarding_packet = true;
      state->pending_data_loss |=
          static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT);
    }
    if (!state->discarding_packet) {
      state->partial_packet.insert(state->partial_packet.end(), fragment.data,
                                   fragment.data + fragment.size);
    }

    state->expecting_continuation = continues_on_next;
    if (continues_on_next)
      continue;

    if (state->discarding_packet) {
      state->partial_packet.clear();
      state->discarding_packet = false;
      continue;
    }
    ForwardPacket(state);
  }
}

void InProcessTracingV2Bridge::OnDataLoss(WriterID writer_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (stopped_forwarding_)
    return;

  WriterState* state = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<WriterState>* const entry = writers_.Find(writer_id);
    if (entry)
      state = entry->get();
  }
  if (!state)
    return;

  state->pending_data_loss |= static_cast<uint32_t>(
      TracePacket::DATA_LOSS_PRESENT | TracePacket::DATA_LOSS_CHUNK_CORRUPTED);
}

void InProcessTracingV2Bridge::ForwardPacket(WriterState* state) {
  // Avoid pointer arithmetic on a null data() for an empty packet.
  const std::vector<uint8_t>& packet_bytes = state->partial_packet;
  const uint8_t* const begin =
      packet_bytes.empty() ? nullptr : packet_bytes.data();
  const uint8_t* const end = packet_bytes.empty()
                                 ? nullptr
                                 : packet_bytes.data() + packet_bytes.size();
  const RewriteResult rewrite_result = RewriteProtoGroupToLengthDelimited(
      begin, end, &rewritten_packet_, kMaxPacketSize);
  state->partial_packet.clear();
  switch (rewrite_result) {
    case RewriteResult::kSuccess:
      break;
    case RewriteResult::kMalformedInput:
      state->pending_data_loss |=
          static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT |
                                TracePacket::DATA_LOSS_CHUNK_CORRUPTED);
      return;
    case RewriteResult::kOutputTooLarge:
      state->pending_data_loss |=
          static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT);
      return;
  }

  {
    auto packet = state->v1_writer->NewTracePacket();
    if (!rewritten_packet_.empty()) {
      packet->AppendRawProtoBytes(rewritten_packet_.data(),
                                  rewritten_packet_.size());
    }
    if (state->pending_data_loss != 0) {
      // Last-value-wins keeps the bridge's loss reason authoritative.
      packet->set_previous_packet_dropped(state->pending_data_loss);
      state->pending_data_loss = 0;
    }
  }  // The handle's destructor finalizes the packet.
  state->has_unflushed_v1_data = true;
}

void InProcessTracingV2Bridge::DrainPendingData(
    std::function<void()> completion) {
  EnqueueBarrier(/*writer_id=*/0, std::move(completion));
}

void InProcessTracingV2Bridge::Flush(WriterID writer_id,
                                     std::function<void()> callback) {
  EnqueueBarrier(writer_id, std::move(callback));
}

void InProcessTracingV2Bridge::OnWriterDestroyed(WriterID writer_id) {
  // Drain and flush before returning the WriterID to v1.
  EnqueueBarrier(writer_id, [this, writer_id] { RetireV1Writer(writer_id); });
}

void InProcessTracingV2Bridge::EnqueueBarrier(
    WriterID writer_id,
    std::function<void()> completion) {
  // Sample before posting so the barrier covers preceding writes.
  const uint32_t drain_target_pos = ring_buffer_.LoadWritePos();
  weak_runner_.PostTask([this, drain_target_pos, writer_id,
                         completion = std::move(completion)]() mutable {
    PERFETTO_DCHECK_THREAD(thread_checker_);
    pending_control_barriers_.emplace_back();
    Barrier& barrier = pending_control_barriers_.back();
    barrier.id = next_barrier_id_++;
    barrier.drain_target_pos = drain_target_pos;
    barrier.writer_id = writer_id;
    barrier.completion = std::move(completion);
    // Only the front barrier runs.
    if (pending_control_barriers_.size() == 1)
      RunFrontBarrier();
  });
}

void InProcessTracingV2Bridge::RunFrontBarrier() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  while (!pending_control_barriers_.empty()) {
    Barrier& barrier = pending_control_barriers_.front();
    if (barrier.pending_flush_acks != 0)
      return;
    if (stopped_forwarding_) {
      CompleteFrontBarrier();
      continue;
    }

    const uint64_t barrier_id = barrier.id;
    if (!DrainUpTo(barrier.drain_target_pos)) {
      // Teardown may complete this barrier before the retry runs.
      weak_runner_.PostTask([this, barrier_id] {
        if (!pending_control_barriers_.empty() &&
            pending_control_barriers_.front().id == barrier_id)
          RunFrontBarrier();
      });
      return;
    }

    // Writer barriers flush one writer; endpoint barriers flush all dirty ones.
    std::vector<WriterID> writers;
    if (barrier.writer_id != 0) {
      writers.push_back(barrier.writer_id);
    } else {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = writers_.GetIterator(); it; ++it) {
        if (it.value()->has_unflushed_v1_data)
          writers.push_back(it.key());
      }
    }
    if (writers.empty()) {
      CompleteFrontBarrier();
      continue;
    }

    // Keep the bridge alive until every asynchronous acknowledgement arrives.
    barrier.pending_flush_acks = writers.size();
    std::shared_ptr<InProcessTracingV2Bridge> owner = shared_from_this();
    for (WriterID writer_id : writers) {
      FlushV1Writer(writer_id, [owner, barrier_id] {
        owner->weak_runner_.PostTask([owner, barrier_id] {
          owner->OnFrontBarrierFlushAcknowledged(barrier_id);
        });
      });
    }
    return;
  }
}

void InProcessTracingV2Bridge::OnFrontBarrierFlushAcknowledged(
    uint64_t barrier_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Ignore acknowledgements for barriers completed during teardown.
  if (pending_control_barriers_.empty() ||
      pending_control_barriers_.front().id != barrier_id)
    return;
  PERFETTO_DCHECK(pending_control_barriers_.front().pending_flush_acks > 0);
  if (--pending_control_barriers_.front().pending_flush_acks != 0)
    return;
  CompleteFrontBarrier();
  RunFrontBarrier();
}

void InProcessTracingV2Bridge::CompleteFrontBarrier() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  std::function<void()> completion =
      std::move(pending_control_barriers_.front().completion);
  pending_control_barriers_.pop_front();
  if (completion)
    completion();
}

void InProcessTracingV2Bridge::FlushV1Writer(WriterID writer_id,
                                             std::function<void()> callback) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  TraceWriter* v1_writer = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<WriterState>* const entry = writers_.Find(writer_id);
    if (entry) {
      v1_writer = (*entry)->v1_writer.get();
      (*entry)->has_unflushed_v1_data = false;
    }
  }
  // A released writer has no v1 connection left to acknowledge the flush.
  if (v1_writer) {
    v1_writer->Flush(std::move(callback));
  } else if (callback) {
    callback();
  }
}

void InProcessTracingV2Bridge::StartReleasingV1Writers() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_writers_)
      return;
    accepting_writers_ = false;
  }
  weak_runner_.PostTask([this] { ReleaseAllV1Writers(); });
}

bool InProcessTracingV2Bridge::HasRetainedV1Writers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return writers_.size() != 0;
}

void InProcessTracingV2Bridge::RetireV1Writer(WriterID writer_id) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  std::unique_ptr<WriterState> writer_to_destroy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<WriterState>* const entry = writers_.Find(writer_id);
    if (!entry)
      return;
    writer_to_destroy = std::move(*entry);
    writers_.Erase(writer_id);
  }
  // Destroy outside |mutex_| because the writer can run callbacks.
}

void InProcessTracingV2Bridge::ReleaseAllV1Writers() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  stopped_forwarding_ = true;
  base::FlatHashMap<WriterID, std::unique_ptr<WriterState>> writers_to_destroy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    PERFETTO_DCHECK(!accepting_writers_);
    writers_to_destroy = std::move(writers_);
  }
  // Released writers may never acknowledge pending flushes.
  while (!pending_control_barriers_.empty())
    CompleteFrontBarrier();
  // |writers_to_destroy| runs destructors outside |mutex_|.
}

bool InProcessTracingV2Bridge::HasPendingRingData() const {
  return NumOutstandingPositions(ring_buffer_.LoadWritePos(),
                                 ring_buffer_reader_.read_pos()) != 0;
}

// static
void InProcessTracingV2Bridge::DeleteWhenQuiescent(
    std::shared_ptr<InProcessTracingV2Bridge> owner) {
  InProcessTracingV2Bridge* const bridge = owner.get();
  PERFETTO_DCHECK_THREAD(bridge->thread_checker_);
  bridge->DrainOnRelayThread();
  if (!bridge->stopped_forwarding_ && bridge->HasPendingRingData() &&
      !bridge->ring_buffer_reader_.has_protocol_error()) {
    base::TaskRunner* const task_runner = bridge->weak_runner_.task_runner();
    auto holder = std::make_shared<std::shared_ptr<InProcessTracingV2Bridge>>(
        std::move(owner));
    task_runner->PostTask(
        [holder] { DeleteWhenQuiescent(std::move(*holder)); });
    return;
  }
  if (PERFETTO_UNLIKELY(!bridge->stopped_forwarding_ &&
                        bridge->HasPendingRingData())) {
    PERFETTO_DLOG("Tracing v2 relay torn down with data still in the ring");
  }
}

}  // namespace perfetto::tracing_v2
