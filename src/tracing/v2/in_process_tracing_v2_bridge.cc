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

#include <algorithm>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "src/tracing/v2/proto_rewriter.h"
#include "src/tracing/v2/tracing_v2_abi.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

using ::perfetto::protos::pbzero::TracePacket;

// How many positions one drain pass resolves before it yields the relay
// sequence. A pass that stops with work left reschedules itself, so this only
// bounds how long a single task runs.
constexpr uint32_t kMaxPositionsPerPass = 256;

// A packet larger than this cannot have come from a well-formed writer: a
// packet is assembled from at most 255 fragments per chunk, and this leaves
// room for a very long chain of them. It exists so a corrupt continuation flag
// cannot make reassembly grow without bound.
// TODO(sashwinbalaji): replace with the service's aggregate per-producer budget
// when the consumer moves into traced; a per-packet cap alone does not bound
// what a producer can make the reader hold across many writers.
constexpr size_t kMaxPacketSize = 8 * 1024 * 1024;

}  // namespace

// static
std::shared_ptr<InProcessTracingV2Bridge> InProcessTracingV2Bridge::Create(
    base::TaskRunner* relay_task_runner,
    uint32_t num_chunks,
    uint32_t chunk_size) {
  PERFETTO_CHECK(relay_task_runner);
  std::unique_ptr<SharedRingBuffer> ring =
      SharedRingBuffer::Create(num_chunks, chunk_size);
  if (!ring) {
    PERFETTO_ELOG(
        "Tracing v2: could not allocate a %u x %u ring; staying on v1",
        num_chunks, chunk_size);
    return nullptr;
  }

  auto deleter = [relay_task_runner](InProcessTracingV2Bridge* bridge) {
    // Packets published just before the last reference went away are still in
    // the ring. Deleting here would drop them, so hand over to a task that
    // drains first. Always queued, including when the last reference dies on
    // the relay thread itself, so that destruction always happens on the
    // sequence every forwarding member is confined to.
    //
    // The task owns the bridge for that trip. A task runner being shut down
    // returns from its loop with immediate tasks still queued and then destroys
    // them, so a task holding a bare pointer to something nobody else owns any
    // more would leak it; destroying this callback destroys the bridge with it.
    std::shared_ptr<InProcessTracingV2Bridge> owner(bridge);
    relay_task_runner->PostTask([owner]() mutable {
      DeleteWhenQuiescent(std::move(owner), /*attempt=*/0);
    });
  };
  return std::shared_ptr<InProcessTracingV2Bridge>(
      new InProcessTracingV2Bridge(std::move(ring), relay_task_runner),
      std::move(deleter));
}

InProcessTracingV2Bridge::InProcessTracingV2Bridge(
    std::unique_ptr<SharedRingBuffer> ring,
    base::TaskRunner* relay_task_runner)
    : ring_buffer_(std::move(ring)),
      // Only stores the pointer; nothing is called back before this
      // constructor returns.
      chunk_reader_(ring_buffer_.get(), this),
      weak_runner_(relay_task_runner) {
  canonical_packet_.reserve(4096);
  // Constructed on the muxer sequence but confined to the relay one from its
  // first drain onwards, so bind the checker there rather than here.
  PERFETTO_DETACH_FROM_THREAD(thread_checker_);
}

InProcessTracingV2Bridge::~InProcessTracingV2Bridge() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // DeleteWhenQuiescent() is the only path here and it drains first. Extract
  // under the lock and destroy outside it: destroying a v1 TraceWriter runs its
  // final flush and its callbacks, and neither may run under |mutex_|.
  base::FlatHashMap<WriterID, std::unique_ptr<WriterState>> writers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    writers = std::move(writers_);
  }
}

bool InProcessTracingV2Bridge::HasPendingRingData() const {
  return PositionDistance(ring_buffer_->LoadWritePosRelaxed(),
                          chunk_reader_.read_pos()) != 0;
}

// static
void InProcessTracingV2Bridge::DeleteWhenQuiescent(
    std::shared_ptr<InProcessTracingV2Bridge> owner,
    uint32_t attempt) {
  InProcessTracingV2Bridge* const bridge = owner.get();
  PERFETTO_DCHECK_THREAD(bridge->thread_checker_);
  // Bounded, so that a ring which cannot drain - a writer that died holding a
  // chunk, say - costs a handful of task-runner round trips instead of hanging
  // teardown forever.
  constexpr uint32_t kMaxDrainAttempts = 64;
  bridge->DrainOnRelayThread();
  if (!bridge->stopped_forwarding_ && bridge->HasPendingRingData() &&
      !bridge->chunk_reader_.stopped() && attempt < kMaxDrainAttempts) {
    // Every retry carries the ownership forward, so there is never a moment
    // when the only thing pointing at the bridge is a raw pointer in a queue.
    base::TaskRunner* const task_runner = bridge->weak_runner_.task_runner();
    task_runner->PostTask([owner = std::move(owner), attempt]() mutable {
      DeleteWhenQuiescent(std::move(owner), attempt + 1);
    });
    return;
  }
  if (PERFETTO_UNLIKELY(!bridge->stopped_forwarding_ &&
                        bridge->HasPendingRingData())) {
    PERFETTO_DLOG("Tracing v2 relay torn down with data still in the ring");
  }
  // |owner| is the last reference and goes out of scope here, on the relay
  // sequence, which is where every member it destroys expects to be.
}

std::unique_ptr<TraceWriter> InProcessTracingV2Bridge::CreateTraceWriter(
    std::unique_ptr<TraceWriter> downstream,
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy) {
  // The v1 writer is created first so the v2 one can reuse its already
  // registered WriterID and routing, and is then retained as the forwarding
  // destination. A direct v2 producer must register and route without it.
  const WriterID writer_id = downstream->writer_id();
  // Zero means the arbiter ran out of WriterIDs. There is no id to put in the
  // chunk header and no key to route the packet back by, so hand the caller the
  // plain v1 writer instead of aliasing somebody else's sequence.
  if (writer_id == 0)
    return downstream;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Checked and inserted under one lock, which is what gives create-versus-
    // release a single order. A writer accepted here is in the map the release
    // moves; one refused here never enters it, and the caller keeps a plain v1
    // writer it owns and destroys itself.
    if (!accepting_writers_)
      return downstream;
    auto state = std::unique_ptr<WriterState>(new WriterState());
    state->trace_writer = std::move(downstream);
    state->target_buffer = target_buffer;
    const auto inserted = writers_.Insert(writer_id, std::move(state));
    PERFETTO_CHECK(inserted.second);
  }

  TraceWriterV2::InitArgs args;
  args.ring_buffer = ring_buffer_.get();
  args.delegate = shared_from_this();
  args.writer_id = writer_id;
  args.target_buffer = target_buffer;
  args.buffer_exhausted_policy = buffer_exhausted_policy;
  return std::unique_ptr<TraceWriter>(new TraceWriterV2(args));
}

void InProcessTracingV2Bridge::DrainToQuiescenceForTesting(
    std::function<void()> completion) {
  weak_runner_.PostTask([this, completion = std::move(completion)] {
    PERFETTO_DCHECK_THREAD(thread_checker_);
    if (stopped_forwarding_) {
      completion();
      return;
    }
    // Everything the ring holds at this instant, and nothing after it. Sampled
    // on the relay, which is the only consumer, so read_pos can only be behind
    // it.
    const uint32_t watermark = ring_buffer_->LoadWritePosRelaxed();
    uint32_t passes_without_progress = 0;
    for (;;) {
      const uint32_t remaining =
          PositionDistance(watermark, chunk_reader_.read_pos());
      if (remaining == 0)
        break;
      // At most |remaining| positions, rather than whatever the ring holds by
      // the time the pass runs. A writer that publishes while this is draining
      // - which happens for real, because forwarding runs downstream writer
      // callbacks - would otherwise be consumed here, and the completion would
      // then be reporting on data the caller never asked about. Ordinary
      // scheduled draining picks that up afterwards.
      const ChunkReader::DrainResult result =
          chunk_reader_.Drain(std::min(remaining, kMaxPositionsPerPass));
      FlushTouchedWriters();
      if (result.positions_resolved != 0) {
        passes_without_progress = 0;
        continue;
      }
      // The ring stopped, or a writer has been winning every race for long
      // enough that the reader keeps asking to be called back. Either way the
      // one thing this seam promises - that everything sampled has been
      // forwarded - is not true, and running the completion would let a test
      // assert about data that never arrived.
      constexpr uint32_t kMaxPassesWithoutProgress = 64;
      if (++passes_without_progress > kMaxPassesWithoutProgress) {
        PERFETTO_FATAL(
            "Tracing v2 relay made no progress in %u passes with %u positions "
            "still to drain; the ring is stopped or permanently contended",
            kMaxPassesWithoutProgress, remaining);
      }
    }
    completion();
  });
}

void InProcessTracingV2Bridge::ReleaseDownstreamWritersOnRelay() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  stopped_forwarding_ = true;
  base::FlatHashMap<WriterID, std::unique_ptr<WriterState>> writers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Nothing has been inserted since StartReleasingDownstreamWriters() closed
    // the set, so this takes every writer the bridge ever accepted.
    PERFETTO_DCHECK(!accepting_writers_);
    writers = std::move(writers_);
  }
  // Destroyed outside the lock: a v1 writer's destructor runs its final flush
  // and its callbacks, and neither may run under |mutex_|.
  writers = base::FlatHashMap<WriterID, std::unique_ptr<WriterState>>();
}

void InProcessTracingV2Bridge::StartReleasingDownstreamWriters() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_writers_)
      return;  // Already started. The relay task below is already queued.
    accepting_writers_ = false;
  }
  if (weak_runner_.task_runner()->RunsTasksOnCurrentThread()) {
    // Already on the relay sequence - which happens when the caller shares a
    // task runner with it, as unit tests do. Posting would only defer work the
    // caller is entitled to see finished.
    ReleaseDownstreamWritersOnRelay();
    return;
  }
  weak_runner_.PostTask([this] { ReleaseDownstreamWritersOnRelay(); });
}

bool InProcessTracingV2Bridge::holds_downstream_writers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return writers_.size() != 0;
}

void InProcessTracingV2Bridge::OnPacketsCommitted() {
  ScheduleDrain();
}

void InProcessTracingV2Bridge::ScheduleDrain() {
  // Release, so the chunk this writer just published is ordered before the flag
  // becomes true. The matching acquire is the exchange in DrainOnRelayThread();
  // see there for why a plain store would not do.
  if (drain_scheduled_.exchange(true, std::memory_order_acq_rel))
    return;
  weak_runner_.PostTask([this] { DrainOnRelayThread(); });
}

void InProcessTracingV2Bridge::DrainOnRelayThread() {
  PERFETTO_DCHECK_THREAD(thread_checker_);

  // Cleared before the ring is read, so a commit arriving during this pass
  // schedules a new task rather than being swallowed. The cost is at most one
  // redundant follow-up task per pass, which is bounded and self-limiting.
  //
  // This has to be a read-modify-write, not a store. A writer that finds the
  // flag already true posts nothing and relies on this pass to pick its chunk
  // up, so this pass must be guaranteed to see what that writer published. A
  // release store here has no acquire half, and the ring's own atomics are not
  // seq_cst, so the total order over this one flag says nothing about them.
  // Exchanging instead reads the value the writer's release-exchange wrote,
  // which synchronizes with it and orders that chunk publication before the
  // ring loads below.
  drain_scheduled_.exchange(false, std::memory_order_acq_rel);

  // Nowhere left to forward to. Whatever is still in the ring is dropped; the
  // writers that could still be publishing into it are harmless, because the
  // ring is producer-local memory this bridge owns.
  if (stopped_forwarding_)
    return;

  const ChunkReader::DrainResult result =
      chunk_reader_.Drain(kMaxPositionsPerPass);
  FlushTouchedWriters();
  if (result.work_may_remain())
    ScheduleDrain();
}

void InProcessTracingV2Bridge::OnChunkRead(
    const ChunkReader::ChunkContents& contents) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  WriterState* state = nullptr;
  {
    // The lock covers the lookup only. Everything below runs downstream writer
    // machinery, which must not happen under a mutex, and is safe without one:
    // the relay is the only sequence that touches a WriterState after its
    // insertion, and the state itself does not move when the map grows.
    std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<WriterState>* const entry =
        writers_.Find(contents.writer_id);
    if (entry)
      state = entry->get();
  }
  if (PERFETTO_UNLIKELY(!state)) {
    // Writers live until this bridge is torn down, so the only way to get here
    // is a WriterID that never had a downstream writer, i.e. a corrupt ring.
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
    // The writer told us it dropped something before this chunk. It only ever
    // does that because it could not get ring capacity.
    state->pending_data_loss |= static_cast<uint32_t>(
        TracePacket::DATA_LOSS_PRESENT | TracePacket::DATA_LOSS_SMB_FULL);
  }

  for (uint32_t i = 0; i < contents.num_fragments; ++i) {
    const ChunkReader::Fragment& fragment = contents.fragments[i];
    const bool is_first = i == 0;
    const bool is_last = i + 1 == contents.num_fragments;
    // Continuation is a chunk-level property: only the first fragment of a
    // chunk can continue the previous one, and only the last can be continued.
    const bool continues_from_prev =
        is_first && (contents.payload_flags & kFlagContinuesFromPrevChunk) != 0;
    const bool continues_on_next =
        is_last && (contents.payload_flags & kFlagContinuesOnNextChunk) != 0;

    if (!continues_from_prev) {
      if (state->expecting_continuation) {
        // The previous chunk promised a tail that never arrived.
        state->pending_data_loss |=
            static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT |
                                  TracePacket::DATA_LOSS_REASSEMBLY_GAP);
      }
      state->pending_packet.clear();
      state->pending_packet_broken = false;
    } else if (!state->expecting_continuation) {
      // The chunk holding this packet's beginning never reached us.
      state->pending_packet_broken = true;
      state->pending_data_loss |=
          static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT |
                                TracePacket::DATA_LOSS_ORPHAN_CONTINUATION);
    }

    if (!state->pending_packet_broken &&
        state->pending_packet.size() + fragment.size > kMaxPacketSize) {
      state->pending_packet_broken = true;
      state->pending_data_loss |=
          static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT |
                                TracePacket::DATA_LOSS_CHUNK_CORRUPTED);
    }
    if (!state->pending_packet_broken) {
      state->pending_packet.insert(state->pending_packet.end(), fragment.data,
                                   fragment.data + fragment.size);
    }

    state->expecting_continuation = continues_on_next;
    if (continues_on_next)
      continue;

    if (state->pending_packet_broken) {
      state->pending_packet.clear();
      state->pending_packet_broken = false;
      continue;
    }
    EmitPacket(contents.writer_id, state);
  }
}

void InProcessTracingV2Bridge::EmitPacket(WriterID writer_id,
                                          WriterState* state) {
  // A zero-byte packet is legal - a data source that opens and closes one
  // without writing a field produces exactly that - and data() on an empty
  // vector may be null, so the range is spelled out rather than computed as
  // data() and data() + 0.
  const std::vector<uint8_t>& packet_bytes = state->pending_packet;
  const uint8_t* const begin =
      packet_bytes.empty() ? nullptr : packet_bytes.data();
  const uint8_t* const end = packet_bytes.empty()
                                 ? nullptr
                                 : packet_bytes.data() + packet_bytes.size();
  if (!RewriteToLengthDelimitedProto(begin, end, kMaxPacketSize,
                                     &canonical_packet_)) {
    ++num_malformed_packets_;
    state->pending_data_loss |=
        static_cast<uint32_t>(TracePacket::DATA_LOSS_PRESENT |
                              TracePacket::DATA_LOSS_CHUNK_CORRUPTED);
    state->pending_packet.clear();
    return;
  }
  state->pending_packet.clear();

  {
    auto packet = state->trace_writer->NewTracePacket();
    if (!canonical_packet_.empty()) {
      packet->AppendRawProtoBytes(canonical_packet_.data(),
                                  canonical_packet_.size());
    }
    if (state->pending_data_loss != 0) {
      // Written after the packet's own bytes on purpose. If the producer set
      // the field too, the last occurrence is the one a decoder keeps, so our
      // report survives instead of being overwritten by a stale zero.
      //
      // The downstream v1 writer stamps its own copy when the v1 SMB drops,
      // which would land ahead of ours and lose these reasons. That is a
      // v1-layer loss on top of a v2-layer one, and it disappears with this
      // relay.
      packet->set_previous_packet_dropped(state->pending_data_loss);
      state->pending_data_loss = 0;
    }
  }  // The handle's destructor finalizes the packet.
  ++num_packets_forwarded_;

  if (std::find(touched_writers_.begin(), touched_writers_.end(), writer_id) ==
      touched_writers_.end()) {
    touched_writers_.push_back(writer_id);
  }
}

void InProcessTracingV2Bridge::FlushTouchedWriters() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // One commit per batch rather than one per packet. Without it a v1 writer
  // holds its chunk until it fills, so a low-rate producer's packets would not
  // reach the service until the session flush - reproducing, in the temporary
  // hop, exactly the idle-writer chunk pinning the v2 ring exists to remove.
  // This requirement belongs to the v1 forwarding layer and goes away with it.
  for (WriterID writer_id : touched_writers_) {
    TraceWriter* writer = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      std::unique_ptr<WriterState>* const entry = writers_.Find(writer_id);
      if (entry)
        writer = (*entry)->trace_writer.get();
    }
    // Outside the lock: Flush() runs writer machinery and may run a callback.
    if (writer)
      writer->Flush();
  }
  touched_writers_.clear();
}

}  // namespace perfetto::tracing_v2
