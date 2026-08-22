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
#include <iterator>
#include <utility>

#include "perfetto/base/logging.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace tracing_v2 {

std::shared_ptr<InProcessTracingV2Bridge> InProcessTracingV2Bridge::Create(
    base::TaskRunner* relay_task_runner,
    uint32_t num_chunks) {
  PERFETTO_CHECK(relay_task_runner);
  auto deleter = [relay_task_runner](InProcessTracingV2Bridge* bridge) {
    // The last writer's retirement, and any flush it had queued, may still be
    // waiting for the relay to reach their positions. Deleting now would drop a
    // flush callback and leave packets unforwarded, so hand over to a task that
    // drains until nothing is outstanding. Always queue it, including on the
    // relay thread, so the retirement runs before the bridge goes away.
    relay_task_runner->PostTask([bridge] { bridge->DeleteWhenQuiescent(0); });
  };
  return std::shared_ptr<InProcessTracingV2Bridge>(
      new InProcessTracingV2Bridge(relay_task_runner, num_chunks),
      std::move(deleter));
}

InProcessTracingV2Bridge::InProcessTracingV2Bridge(
    base::TaskRunner* relay_task_runner,
    uint32_t num_chunks)
    : ring_buffer_(num_chunks),
      chunk_reader_(&ring_buffer_),
      weak_runner_(relay_task_runner) {
  PERFETTO_CHECK(relay_task_runner);
  // Constructed on the muxer sequence but confined to the relay one from its
  // first drain onwards, so bind the checker there rather than here.
  PERFETTO_DETACH_FROM_THREAD(thread_checker_);
}

InProcessTracingV2Bridge::~InProcessTracingV2Bridge() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // DeleteWhenQuiescent() is the only path here and it drains first, so both
  // of these are normally empty. They are not guaranteed to be: the drain is
  // bounded so that a wedged ring cannot hang teardown.
  //
  // Extract under the lock, destroy outside it. Destroying a TraceWriter runs
  // writer and callback machinery, and a pending operation's callback owns
  // arbitrary user state; neither may run under |mutex_|. This is the same
  // rule ProcessPendingOperations() follows for retirement.
  base::FlatHashMap<WriterID, WriterState> writers;
  std::vector<PendingOperation> operations;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    writers = std::move(writers_);
    operations = std::move(pending_operations_);
  }
}

bool InProcessTracingV2Bridge::HasOutstandingWork() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  if (ring_buffer_.has_pending_data())
    return true;
  std::lock_guard<std::mutex> lock(mutex_);
  return writers_.size() != 0 || !pending_operations_.empty();
}

void InProcessTracingV2Bridge::DeleteWhenQuiescent(uint32_t attempt) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Bounded, so that a ring that cannot drain (a writer that died mid-chunk,
  // say) costs a handful of task-runner round trips instead of hanging
  // teardown forever.
  constexpr uint32_t kMaxDrainAttempts = 64;
  DrainOnTaskRunner();
  if (HasOutstandingWork() && attempt < kMaxDrainAttempts) {
    weak_runner_.task_runner()->PostTask(
        [this, attempt] { DeleteWhenQuiescent(attempt + 1); });
    return;
  }
  if (PERFETTO_UNLIKELY(HasOutstandingWork()))
    PERFETTO_DLOG("Tracing v2 relay torn down with work still outstanding");
  delete this;
}

std::unique_ptr<TraceWriter> InProcessTracingV2Bridge::CreateTraceWriter(
    std::unique_ptr<TraceWriter> downstream,
    BufferID target_buffer,
    BufferExhaustedPolicy buffer_exhausted_policy) {
  PERFETTO_CHECK(downstream);
  // The v1 writer is created first so v2 can reuse its registered WriterID
  // and routing, then retains it as the forwarding destination. The direct v2
  // path must register and route without this second writer.
  const WriterID writer_id = downstream->writer_id();
  if (writer_id == 0)
    return downstream;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto inserted = writers_.Insert(
        writer_id, WriterState{std::move(downstream), target_buffer});
    PERFETTO_CHECK(inserted.second);
  }

  TraceWriterV2::InitArgs args;
  args.ring_buffer = &ring_buffer_;
  args.delegate = shared_from_this();
  args.writer_id = writer_id;
  args.target_buffer = target_buffer;
  args.buffer_exhausted_policy = buffer_exhausted_policy;
  return std::unique_ptr<TraceWriter>(new TraceWriterV2(args));
}

uint32_t InProcessTracingV2Bridge::write_pos() const {
  return ring_buffer_.write_pos();
}

void InProcessTracingV2Bridge::DrainThrough(uint32_t watermark,
                                            std::function<void()> completion) {
  weak_runner_.PostTask([this, watermark, completion = std::move(completion)] {
    // Tested between passes, never inside one: ChunkReader advances the read
    // position as soon as it has copied a chunk out of the ring, which is
    // before that chunk has been forwarded and its writer flushed. Returning
    // from DrainOnTaskRunner() is the publication boundary.
    //
    // This terminates even with writers active: the reader consumes a position
    // whether or not it found data there, and never waits for a live writer.
    // Positions reserved after |watermark| belong to writers still running and
    // are not covered by the promise the caller is about to make.
    while (!SharedRingBuffer::IsPositionAtOrAfter(ring_buffer_.read_pos(),
                                                  watermark))
      DrainOnTaskRunner();
    completion();
  });
}

void InProcessTracingV2Bridge::OnPacketsCommitted() {
  ScheduleDrain();
}

void InProcessTracingV2Bridge::OnWriterFlush(WriterID writer_id,
                                             uint32_t pos,
                                             std::function<void()> callback) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_operations_.push_back(PendingOperation{
        PendingOperation::Type::kFlush, writer_id, pos, std::move(callback)});
  }
  ScheduleDrain();
}

void InProcessTracingV2Bridge::OnWriterDestroyed(WriterID writer_id,
                                                 uint32_t pos) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_operations_.push_back(
        PendingOperation{PendingOperation::Type::kRetire, writer_id, pos, {}});
  }
  ScheduleDrain();
}

void InProcessTracingV2Bridge::ScheduleDrain() {
  // Release, so that the chunk this writer just published is ordered before
  // the flag becomes true. The matching acquire is the exchange in
  // DrainOnTaskRunner(); see there for why a plain store would not do.
  if (drain_scheduled_.exchange(true, std::memory_order_acq_rel))
    return;
  weak_runner_.PostTask([this] { DrainOnTaskRunner(); });
}

void InProcessTracingV2Bridge::ForwardPacket(
    const ChunkReader::Packet& packet) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  TraceWriter* writer = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    WriterState* const state = writers_.Find(packet.writer_id);
    if (PERFETTO_UNLIKELY(!state)) {
      PERFETTO_DFATAL("Tracing v2 packet references retired writer %u",
                      packet.writer_id);
      return;
    }
    if (PERFETTO_UNLIKELY(state->target_buffer != packet.target_buffer)) {
      PERFETTO_DFATAL("Tracing v2 writer %u changed target buffer",
                      packet.writer_id);
      return;
    }
    writer = state->trace_writer.get();
  }

  auto trace_packet = writer->NewTracePacket();
  // The downstream v1 writer currently owns first_packet_on_sequence. Assign
  // that responsibility explicitly in the v2 service protocol before this
  // forwarding writer is removed.
  // TODO(sashwinbalaji): define and test v2 sequence-start stamping.
  // Exactly one previous_packet_dropped occurrence: the reader already hoisted
  // any the producer set and ORed it into this mask, so appending the packet
  // bytes below cannot introduce a second, overriding one.
  //
  // The downstream v1 writer stamps its own field 42 when the v1 SMB dropped,
  // which would still land before ours and lose its reasons. That is a v1-layer
  // loss on top of a v2-layer one, so rare that it is not worth plumbing in a
  // validation relay.
  // TODO(sashwinbalaji): merge downstream v1 loss before relying on these
  // production loss reporting, or delete this with the relay.
  if (packet.previous_packet_dropped != 0)
    trace_packet->set_previous_packet_dropped(packet.previous_packet_dropped);
  if (packet.size != 0)
    trace_packet->AppendRawProtoBytes(packet.data, packet.size);

  if (std::find(touched_writers_.begin(), touched_writers_.end(),
                packet.writer_id) == touched_writers_.end()) {
    touched_writers_.push_back(packet.writer_id);
  }
}

void InProcessTracingV2Bridge::FlushTouchedWriters(
    const std::vector<WriterID>& already_flushed) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // One commit per batch rather than one per packet: without this a v1 writer
  // holds its chunk until it fills, so a low-rate producer's packets would not
  // reach the service until the session flush, and the temporary hop would
  // reproduce the idle-writer chunk pinning this change exists to remove. This
  // requirement belongs to the v1 forwarding layer and goes away with it.
  //
  // Skips writers a ready flush operation already committed in this pass, so
  // one explicit TraceWriterV2::Flush() costs one downstream flush, not two.
  for (WriterID writer_id : touched_writers_) {
    if (std::find(already_flushed.begin(), already_flushed.end(), writer_id) !=
        already_flushed.end()) {
      continue;
    }
    TraceWriter* writer = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      WriterState* const state = writers_.Find(writer_id);
      if (state)
        writer = state->trace_writer.get();
    }
    if (writer)
      writer->Flush();
  }
  touched_writers_.clear();
}

bool InProcessTracingV2Bridge::ProcessPendingOperations(
    std::vector<WriterID>* flushed_writers) {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Read here, after the batch has been ungrouped and forwarded. Deliberately
  // not the ring's read cursor sampled earlier: that advances as soon as a
  // chunk has been copied out of the ring, which is before the relay has done
  // anything with it.
  const uint32_t processed_pos = ring_buffer_.read_pos();

  std::vector<PendingOperation> operations;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    operations.swap(pending_operations_);
  }

  std::vector<PendingOperation> deferred;
  for (PendingOperation& operation : operations) {
    if (!SharedRingBuffer::IsPositionAtOrAfter(processed_pos, operation.pos)) {
      deferred.emplace_back(std::move(operation));
      continue;
    }

    if (operation.type == PendingOperation::Type::kFlush) {
      TraceWriter* writer = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        WriterState* const state = writers_.Find(operation.writer_id);
        if (state)
          writer = state->trace_writer.get();
      }
      if (writer) {
        // This commits everything forwarded above, so the batch flush below
        // has nothing left to do for this writer. Several ready flushes for
        // one writer still issue one downstream flush each, which is what
        // keeps their callbacks in FIFO order.
        flushed_writers->push_back(operation.writer_id);
        writer->Flush(std::move(operation.callback));
      }
      continue;
    }

    chunk_reader_.ForgetWriter(operation.writer_id);
    std::unique_ptr<TraceWriter> retired_writer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      WriterState* const state = writers_.Find(operation.writer_id);
      if (state) {
        retired_writer = std::move(state->trace_writer);
        writers_.Erase(operation.writer_id);
      }
    }
    // Destroy outside |mutex_|: it runs the writer's final flush and its
    // callbacks. Off the arbiter's own sequence v1 queues that commit and the
    // UnregisterTraceWriter to the muxer, and relay FIFO plus the endpoint
    // fence keep both ahead of the control operation that follows.
    retired_writer.reset();
  }

  if (deferred.empty())
    return false;

  std::lock_guard<std::mutex> lock(mutex_);
  deferred.insert(deferred.end(),
                  std::make_move_iterator(pending_operations_.begin()),
                  std::make_move_iterator(pending_operations_.end()));
  pending_operations_.swap(deferred);
  return true;
}

void InProcessTracingV2Bridge::DrainOnTaskRunner() {
  PERFETTO_DCHECK_THREAD(thread_checker_);
  // Clear before draining, so a commit arriving during this pass schedules a
  // new task rather than being swallowed.
  //
  // This has to be a read-modify-write, not a store. A writer that finds the
  // flag already true posts nothing and relies on this pass to pick its chunk
  // up, so this pass must be guaranteed to see what that writer published. A
  // release store here would not give that: it has no acquire component, and
  // the ring's own atomics are not seq_cst, so the total order over this flag
  // says nothing about them. Exchanging instead reads the value the writer's
  // release-exchange wrote, which synchronizes-with it and therefore orders
  // its chunk publication before the ring loads below.
  drain_scheduled_.exchange(false, std::memory_order_acq_rel);

  const ChunkReader::DrainResult result = chunk_reader_.Drain(
      [this](const ChunkReader::Packet& packet) { ForwardPacket(packet); });
  // Operations first: a ready flush commits its writer, and the batch flush
  // then skips it instead of committing the same writer a second time.
  std::vector<WriterID> flushed_writers;
  const bool operations_pending = ProcessPendingOperations(&flushed_writers);
  FlushTouchedWriters(flushed_writers);
  if (result.has_more || operations_pending)
    ScheduleDrain();
}

}  // namespace tracing_v2
}  // namespace perfetto
