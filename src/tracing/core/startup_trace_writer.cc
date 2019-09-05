/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/ext/tracing/core/startup_trace_writer.h"

#include <numeric>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/tracing/core/startup_trace_writer_registry.h"
#include "perfetto/protozero/proto_utils.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/tracing/core/null_trace_writer.h"
#include "src/tracing/core/patch_list.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"

using PageHeader = perfetto::SharedMemoryABI::PageHeader;
using ChunkHeader = perfetto::SharedMemoryABI::ChunkHeader;

namespace perfetto {

namespace {

static constexpr ChunkID kFirstChunkId = 0;

SharedMemoryABI::Chunk NewChunk(SharedMemoryArbiterImpl* arbiter,
                                WriterID writer_id,
                                ChunkID chunk_id,
                                bool fragmenting_packet,
                                BufferExhaustedPolicy buffer_exhausted_policy) {
  ChunkHeader::Packets packets = {};
  if (fragmenting_packet) {
    packets.count = 1;
    packets.flags = ChunkHeader::kFirstPacketContinuesFromPrevChunk;
  }

  // The memory order of the stores below doesn't really matter. This |header|
  // is just a local temporary object. The GetNewChunk() call below will copy it
  // into the shared buffer with the proper barriers.
  ChunkHeader header = {};
  header.writer_id.store(writer_id, std::memory_order_relaxed);
  header.chunk_id.store(chunk_id, std::memory_order_relaxed);
  header.packets.store(packets, std::memory_order_relaxed);

  return arbiter->GetNewChunk(header, buffer_exhausted_policy);
}

class LocalBufferReader {
 public:
  LocalBufferReader(std::unique_ptr<protozero::ScatteredHeapBuffer> buffer)
      : buffer_(std::move(buffer)),
        buffer_slices_(buffer_->slices()),
        cur_slice_(buffer_slices_.begin()) {}

  size_t ReadBytes(SharedMemoryABI::Chunk* target_chunk,
                   size_t num_bytes,
                   size_t cur_payload_size) {
    PERFETTO_CHECK(target_chunk->payload_size() >=
                   num_bytes + cur_payload_size);
    uint8_t* target_ptr = target_chunk->payload_begin() + cur_payload_size;
    size_t bytes_read = 0;
    while (bytes_read < num_bytes) {
      if (cur_slice_ == buffer_slices_.end())
        return bytes_read;

      auto cur_slice_range = cur_slice_->GetUsedRange();

      if (cur_slice_range.size() == cur_slice_offset_) {
        cur_slice_offset_ = 0;
        cur_slice_++;
        continue;
      }

      size_t read_size = std::min(num_bytes - bytes_read,
                                  cur_slice_range.size() - cur_slice_offset_);
      memcpy(target_ptr + bytes_read, cur_slice_range.begin + cur_slice_offset_,
             read_size);
      cur_slice_offset_ += read_size;
      bytes_read += read_size;

      // Should have either read all of the chunk or completed reading now.
      PERFETTO_DCHECK(cur_slice_offset_ == cur_slice_range.size() ||
                      bytes_read == num_bytes);
    }
    return bytes_read;
  }

  size_t TotalUsedSize() const {
    size_t used_size = 0;
    for (const auto& slice : buffer_slices_) {
      used_size += slice.GetUsedRange().size();
    }
    return used_size;
  }

  bool DidReadAllData() const {
    if (cur_slice_ == buffer_slices_.end())
      return true;

    const auto next_slice = cur_slice_ + 1;
    return next_slice == buffer_slices_.end() &&
           cur_slice_->GetUsedRange().size() == cur_slice_offset_;
  }

 private:
  std::unique_ptr<protozero::ScatteredHeapBuffer> buffer_;
  const std::vector<protozero::ScatteredHeapBuffer::Slice>& buffer_slices_;

  // Iterator pointing to slice in |buffer_slices_| that we're currently reading
  // from.
  std::vector<protozero::ScatteredHeapBuffer::Slice>::const_iterator cur_slice_;
  // Read offset in the current slice in bytes.
  size_t cur_slice_offset_ = 0;
};

// Helper class that takes ownership of a LocalBufferReader its buffer and
// commits the buffer's data into the assigned SMB in batches. After writing
// each batch of data, it waits for the service to acknowledge the batch's
// commit before continuing with the remaining data.
class LocalBufferCommitter {
 public:
  LocalBufferCommitter(std::unique_ptr<LocalBufferReader> local_buffer_reader,
                       std::unique_ptr<std::vector<uint32_t>> packet_sizes,
                       base::WeakPtr<SharedMemoryArbiterImpl> arbiter,
                       WriterID writer_id,
                       BufferID target_buffer,
                       size_t chunks_per_batch,
                       BufferExhaustedPolicy buffer_exhausted_policy,
                       SharedMemoryABI::Chunk first_chunk)
      : local_buffer_reader_(std::move(local_buffer_reader)),
        packet_sizes_(std::move(packet_sizes)),
        arbiter_(arbiter),
        // TODO(eseckler): This assumes a fixed page layout of one chunk per
        // page. If we ever end up supporting dynamic page layouts, we'd have to
        // make sure that the arbiter gives us full-page chunks.
        max_payload_size_(arbiter->page_size() - sizeof(PageHeader) -
                          sizeof(ChunkHeader)),
        writer_id_(writer_id),
        target_buffer_(target_buffer),
        chunks_per_batch_(chunks_per_batch),
        buffer_exhausted_policy_(buffer_exhausted_policy),
        cur_chunk_(std::move(first_chunk)) {
    PERFETTO_DCHECK(cur_chunk_.is_valid());
    PERFETTO_DCHECK(!packet_sizes_->empty());
    remaining_packet_size_ = (*packet_sizes_)[packet_idx_];
  }

  static void CommitRemainingDataInBatches(
      std::unique_ptr<LocalBufferCommitter> committer) {
    // Give up and destroy the committer if the arbiter went away.
    if (!committer->arbiter_)
      return;

    committer->CommitNextBatch();
    if (committer->HasMoreDataToCommit()) {
      // Flush the commit request to the service and wait for its response
      // before continuing with the next batch.
      std::shared_ptr<std::unique_ptr<LocalBufferCommitter>> committer_shared(
          new std::unique_ptr<LocalBufferCommitter>(std::move(committer)));

      (*committer_shared)
          ->arbiter_->FlushPendingCommitDataRequests([committer_shared]() {
            std::unique_ptr<LocalBufferCommitter> owned_committer(
                committer_shared->release());
            CommitRemainingDataInBatches(std::move(owned_committer));
          });
      return;
    }

    // We should have read all data from the local buffer.
    PERFETTO_DCHECK(committer->local_buffer_reader_->DidReadAllData());
    // Last chunk should have completed the last packet.
    PERFETTO_DCHECK(!committer->fragmenting_packet_);

    committer->arbiter_->FlushPendingCommitDataRequests();
  }

  size_t GetTotalNumChunksRequired() {
    // We will write at least one chunk.
    size_t num_chunks = 1;

    size_t cur_payload_size = 0;
    uint16_t cur_num_packets = 0;
    for (size_t packet_idx = 0; packet_idx < packet_sizes_->size();
         packet_idx++) {
      uint32_t remaining_packet_size = (*packet_sizes_)[packet_idx];
      ++cur_num_packets;
      do {
        uint32_t fragment_size = static_cast<uint32_t>(
            std::min(static_cast<size_t>(remaining_packet_size),
                     max_payload_size_ - cur_payload_size -
                         SharedMemoryABI::kPacketHeaderSize));
        cur_payload_size += SharedMemoryABI::kPacketHeaderSize;
        cur_payload_size += fragment_size;
        remaining_packet_size -= fragment_size;

        // We need another chunk if we've filled its payload (i.e., cannot fit
        // another packet's header) or reached the maximum number of packets.
        bool next_chunk =
            cur_payload_size >=
                max_payload_size_ - SharedMemoryABI::kPacketHeaderSize ||
            cur_num_packets == ChunkHeader::Packets::kMaxCount;

        if (next_chunk) {
          num_chunks++;
          bool is_fragmenting = remaining_packet_size > 0;
          cur_num_packets = is_fragmenting ? 1 : 0;
          cur_payload_size = 0;
        }
      } while (remaining_packet_size > 0);
    }

    return num_chunks;
  }

 private:
  bool HasMoreDataToCommit() const {
    PERFETTO_DCHECK(packet_idx_ <= packet_sizes_->size());
    return packet_idx_ < packet_sizes_->size() || remaining_packet_size_ != 0;
  }

  // Reads (part of) the remaining data from |local_buffer_reader_| and writes
  // the next batch of chunks into the SMB.
  void CommitNextBatch() {
    PERFETTO_METATRACE_SCOPED(TAG_TRACE_WRITER,
                              TRACE_WRITER_COMMIT_STARTUP_WRITER_BATCH);
    for (size_t num_chunks = 0;
         (!chunks_per_batch_ || num_chunks < chunks_per_batch_) &&
         HasMoreDataToCommit();
         num_chunks++) {
      if (!CommitNextChunk()) {
        // We ran out of SMB space. Send the current batch early and retry later
        // with the next batch.
        break;
      }
    }
  }

  bool CommitNextChunk() {
    PERFETTO_DCHECK(HasMoreDataToCommit());

    // First chunk is acquired before LocalBufferCommitter is created, so we may
    // already have a valid chunk.
    if (!cur_chunk_.is_valid()) {
      cur_chunk_ = NewChunk(arbiter_.get(), writer_id_, next_chunk_id_,
                            fragmenting_packet_, buffer_exhausted_policy_);

      if (!cur_chunk_.is_valid())
        return false;

      next_chunk_id_++;
    }

    // See comment at initialization of |max_payload_size_|.
    PERFETTO_CHECK(max_payload_size_ == cur_chunk_.payload_size());

    // Iterate over remaining packets, starting at |packet_idx_|. Write as much
    // data as possible into |chunk| while not exceeding the chunk's payload
    // size and the maximum number of packets per chunk.
    size_t cur_payload_size = 0;
    uint16_t cur_num_packets = 0;
    PatchList empty_patch_list;
    PERFETTO_DCHECK(packet_idx_ < packet_sizes_->size());
    PERFETTO_DCHECK((*packet_sizes_)[packet_idx_] >= remaining_packet_size_ &&
                    (remaining_packet_size_ || !(*packet_sizes_)[packet_idx_]));
    while (HasMoreDataToCommit()) {
      ++cur_num_packets;

      // The packet may not fit completely into the chunk.
      uint32_t fragment_size = static_cast<uint32_t>(
          std::min(static_cast<size_t>(remaining_packet_size_),
                   max_payload_size_ - cur_payload_size -
                       SharedMemoryABI::kPacketHeaderSize));

      // Write packet header, i.e. the fragment size.
      protozero::proto_utils::WriteRedundantVarInt(
          fragment_size, cur_chunk_.payload_begin() + cur_payload_size);
      cur_payload_size += SharedMemoryABI::kPacketHeaderSize;

      // Copy packet content into the chunk.
      size_t bytes_read = local_buffer_reader_->ReadBytes(
          &cur_chunk_, fragment_size, cur_payload_size);
      PERFETTO_DCHECK(bytes_read == fragment_size);

      cur_payload_size += fragment_size;
      remaining_packet_size_ -= fragment_size;

      fragmenting_packet_ = remaining_packet_size_ > 0;
      if (!fragmenting_packet_) {
        ++packet_idx_;
        if (packet_idx_ < packet_sizes_->size()) {
          remaining_packet_size_ = (*packet_sizes_)[packet_idx_];
        }
      }

      // We should return the current chunk if we've filled its payload, reached
      // the maximum number of packets, or wrote everything we wanted to.
      bool return_chunk =
          cur_payload_size >=
              max_payload_size_ - SharedMemoryABI::kPacketHeaderSize ||
          cur_num_packets == ChunkHeader::Packets::kMaxCount ||
          !HasMoreDataToCommit();

      if (return_chunk)
        break;
    }

    auto new_packet_count = cur_chunk_.IncreasePacketCountTo(cur_num_packets);
    PERFETTO_DCHECK(new_packet_count == cur_num_packets);

    if (fragmenting_packet_) {
      PERFETTO_DCHECK(cur_payload_size == max_payload_size_);
      cur_chunk_.SetFlag(ChunkHeader::kLastPacketContinuesOnNextChunk);
    }

    arbiter_->ReturnCompletedChunk(std::move(cur_chunk_), target_buffer_,
                                   &empty_patch_list);
    return true;
  }

  std::unique_ptr<LocalBufferReader> local_buffer_reader_;
  std::unique_ptr<std::vector<uint32_t>> packet_sizes_;
  base::WeakPtr<SharedMemoryArbiterImpl> arbiter_;
  const size_t max_payload_size_;
  const WriterID writer_id_;
  const BufferID target_buffer_;
  const size_t chunks_per_batch_;
  BufferExhaustedPolicy buffer_exhausted_policy_;
  SharedMemoryABI::Chunk cur_chunk_;
  // We receive the first chunk in the constructor, thus the next chunk will be
  // the second one.
  ChunkID next_chunk_id_ = kFirstChunkId + 1;
  size_t packet_idx_ = 0;
  uint32_t remaining_packet_size_ = 0;
  bool fragmenting_packet_ = false;
};

}  // namespace

StartupTraceWriter::StartupTraceWriter(
    std::shared_ptr<StartupTraceWriterRegistryHandle> registry_handle,
    BufferExhaustedPolicy buffer_exhausted_policy,
    size_t max_buffer_size_bytes)
    : registry_handle_(std::move(registry_handle)),
      buffer_exhausted_policy_(buffer_exhausted_policy),
      max_buffer_size_bytes_(max_buffer_size_bytes),
      memory_buffer_(new protozero::ScatteredHeapBuffer()),
      memory_stream_writer_(
          new protozero::ScatteredStreamWriter(memory_buffer_.get())),
      packet_sizes_(new std::vector<uint32_t>()) {
  memory_buffer_->set_writer(memory_stream_writer_.get());
  PERFETTO_DETACH_FROM_THREAD(writer_thread_checker_);
}

StartupTraceWriter::StartupTraceWriter(
    std::unique_ptr<TraceWriter> trace_writer)
    : was_bound_(true), trace_writer_(std::move(trace_writer)) {}

StartupTraceWriter::~StartupTraceWriter() {
  // Should have been returned to the registry before destruction.
  PERFETTO_DCHECK(!registry_handle_);
}

// static
void StartupTraceWriter::ReturnToRegistry(
    std::unique_ptr<StartupTraceWriter> writer) {
  auto registry_handle = std::move(writer->registry_handle_);
  if (registry_handle) {
    // May destroy |writer|.
    registry_handle->ReturnWriterToRegistry(std::move(writer));
  }
}

bool StartupTraceWriter::BindToArbiter(SharedMemoryArbiterImpl* arbiter,
                                       BufferID target_buffer,
                                       size_t chunks_per_batch) {
  // LocalBufferCommitter requires a WeakPtr to the arbiter, and thus needs to
  // execute on the arbiter's task runner.
  PERFETTO_DCHECK(arbiter->task_runner()->RunsTasksOnCurrentThread());

  // Create and destroy trace writer without holding lock, since this will post
  // a task and task posting may trigger a trace event, which would cause a
  // deadlock. This may create a few more trace writers than necessary in cases
  // where a concurrent write is in progress (other than causing some
  // computational overhead, this is not problematic).
  auto trace_writer =
      arbiter->CreateTraceWriter(target_buffer, buffer_exhausted_policy_);

  {
    std::lock_guard<std::mutex> lock(lock_);

    PERFETTO_DCHECK(!trace_writer_);

    // Can't bind while the writer thread is writing.
    if (write_in_progress_)
      return false;

    // If there's a pending trace packet, it should have been completed by the
    // writer thread before write_in_progress_ is reset.
    if (cur_packet_) {
      PERFETTO_DCHECK(cur_packet_->is_finalized());
      cur_packet_.reset();
    }

    // Successfully bind if we don't have any data or no valid trace writer.
    if (packet_sizes_->empty() || !trace_writer->writer_id()) {
      trace_writer_ = std::move(trace_writer);
      memory_buffer_.reset();
      packet_sizes_.reset();
      memory_stream_writer_.reset();
      return true;
    }

    // We need to ensure that we commit at least one chunk now, otherwise the
    // service might receive and erroneously start reading from a future chunk
    // committed by the underlying trace writer. Thus, we attempt to acquire the
    // first chunk and bail out if we fail (we'll retry later).
    SharedMemoryABI::Chunk first_chunk =
        NewChunk(arbiter, trace_writer->writer_id(), kFirstChunkId,
                 /*fragmenting_packet=*/false, buffer_exhausted_policy_);
    if (!first_chunk.is_valid())
      return false;

    trace_writer_ = std::move(trace_writer);
    ChunkID next_chunk_id = CommitLocalBufferChunks(
        arbiter, trace_writer_->writer_id(), target_buffer, chunks_per_batch,
        std::move(first_chunk));

    // The real TraceWriter should start writing at the subsequent chunk ID.
    bool success = trace_writer_->SetFirstChunkId(next_chunk_id);
    PERFETTO_DCHECK(success);
  }

  return true;
}

TraceWriter::TracePacketHandle StartupTraceWriter::NewTracePacket() {
  PERFETTO_DCHECK_THREAD(writer_thread_checker_);

  // Check if we are already bound without grabbing the lock. This is an
  // optimization to avoid any locking in the common case where the proxy was
  // bound some time ago.
  if (PERFETTO_LIKELY(was_bound_)) {
    PERFETTO_DCHECK(!cur_packet_);
    PERFETTO_DCHECK(trace_writer_);
    return trace_writer_->NewTracePacket();
  }

  // Now grab the lock and safely check whether we are still unbound.
  {
    std::unique_lock<std::mutex> lock(lock_);
    if (trace_writer_) {
      PERFETTO_DCHECK(!cur_packet_);
      // Set the |was_bound_| flag to avoid locking in future calls to
      // NewTracePacket().
      was_bound_ = true;
      // Don't hold the lock while calling NewTracePacket() on |trace_writer_|.
      // This is safe because |trace_writer_| remains valid once set. It also
      // avoids deadlocks that may be caused by holding the lock while waiting
      // for a new SMB chunk in |trace_writer_|.
      lock.unlock();
      return trace_writer_->NewTracePacket();
    }

    // Check if we already exceeded the maximum size of the local buffer, and if
    // so, write into nowhere.
    if (null_trace_writer_ ||
        memory_buffer_->GetTotalSize() >= max_buffer_size_bytes_) {
      if (!null_trace_writer_) {
        null_trace_writer_.reset(new NullTraceWriter());

        // Write a packet that marks data loss.
        std::unique_ptr<protos::pbzero::TracePacket> packet(
            new protos::pbzero::TracePacket());
        packet->Reset(memory_stream_writer_.get());
        {
          TraceWriter::TracePacketHandle handle(packet.get());
          handle->set_previous_packet_dropped(true);
        }
        uint32_t packet_size = packet->Finalize();
        packet_sizes_->push_back(packet_size);
      }
      return null_trace_writer_->NewTracePacket();
    }

    // Not bound. Make sure it stays this way until the TracePacketHandle goes
    // out of scope by setting |write_in_progress_|.
    PERFETTO_DCHECK(!write_in_progress_);
    write_in_progress_ = true;
  }

  // Write to the local buffer.
  if (cur_packet_) {
    // If we hit this, the caller is calling NewTracePacket() without having
    // finalized the previous packet.
    PERFETTO_DCHECK(cur_packet_->is_finalized());
  } else {
    cur_packet_.reset(new protos::pbzero::TracePacket());
  }

  cur_packet_->Reset(memory_stream_writer_.get());
  TraceWriter::TracePacketHandle handle(cur_packet_.get());
  // |this| outlives the packet handle.
  handle.set_finalization_listener(this);
  return handle;
}

void StartupTraceWriter::Flush(std::function<void()> callback) {
  PERFETTO_DCHECK_THREAD(writer_thread_checker_);
  // It's fine to check |was_bound_| instead of acquiring the lock because
  // |trace_writer_| will only need flushing after the first trace packet was
  // written to it and |was_bound_| is set.
  if (PERFETTO_LIKELY(was_bound_)) {
    PERFETTO_DCHECK(trace_writer_);
    return trace_writer_->Flush(std::move(callback));
  }

  // Can't flush while unbound.
  if (callback)
    callback();
}

WriterID StartupTraceWriter::writer_id() const {
  PERFETTO_DCHECK_THREAD(writer_thread_checker_);
  // We can't acquire the lock because this is a const method. So we'll only
  // proxy to |trace_writer_| once we have written the first packet to it
  // instead.
  if (PERFETTO_LIKELY(was_bound_)) {
    PERFETTO_DCHECK(trace_writer_);
    return trace_writer_->writer_id();
  }
  return 0;
}

uint64_t StartupTraceWriter::written() const {
  PERFETTO_DCHECK_THREAD(writer_thread_checker_);
  // We can't acquire the lock because this is a const method. So we'll only
  // proxy to |trace_writer_| once we have written the first packet to it
  // instead.
  if (PERFETTO_LIKELY(was_bound_)) {
    PERFETTO_DCHECK(trace_writer_);
    return trace_writer_->written();
  }
  return 0;
}

size_t StartupTraceWriter::used_buffer_size() {
  PERFETTO_DCHECK_THREAD(writer_thread_checker_);
  if (PERFETTO_LIKELY(was_bound_))
    return 0;

  std::lock_guard<std::mutex> lock(lock_);
  if (trace_writer_)
    return 0;

  size_t used_size = 0;
  memory_buffer_->AdjustUsedSizeOfCurrentSlice();
  for (const auto& slice : memory_buffer_->slices()) {
    used_size += slice.GetUsedRange().size();
  }
  return used_size;
}

void StartupTraceWriter::OnMessageFinalized(protozero::Message* message) {
  PERFETTO_DCHECK(cur_packet_.get() == message);
  PERFETTO_DCHECK(cur_packet_->is_finalized());
  // Finalize() is a no-op because the packet is already finalized.
  uint32_t packet_size = cur_packet_->Finalize();
  packet_sizes_->push_back(packet_size);

  // Write is complete, reset the flag to allow binding.
  std::lock_guard<std::mutex> lock(lock_);
  PERFETTO_DCHECK(write_in_progress_);
  write_in_progress_ = false;
}

ChunkID StartupTraceWriter::CommitLocalBufferChunks(
    SharedMemoryArbiterImpl* arbiter,
    WriterID writer_id,
    BufferID target_buffer,
    size_t chunks_per_batch,
    SharedMemoryABI::Chunk first_chunk) {
  PERFETTO_DCHECK(!packet_sizes_->empty());
  PERFETTO_DCHECK(writer_id);

  memory_buffer_->AdjustUsedSizeOfCurrentSlice();
  memory_stream_writer_.reset();

  std::unique_ptr<LocalBufferReader> local_buffer_reader(
      new LocalBufferReader(std::move(memory_buffer_)));

  PERFETTO_DCHECK(local_buffer_reader->TotalUsedSize() ==
                  std::accumulate(packet_sizes_->begin(), packet_sizes_->end(),
                                  static_cast<size_t>(0u)));

  std::unique_ptr<LocalBufferCommitter> committer(new LocalBufferCommitter(
      std::move(local_buffer_reader), std::move(packet_sizes_),
      arbiter->GetWeakPtr(), writer_id, target_buffer, chunks_per_batch,
      buffer_exhausted_policy_, std::move(first_chunk)));

  ChunkID next_chunk_id =
      kFirstChunkId +
      static_cast<ChunkID>(committer->GetTotalNumChunksRequired());

  // Write the chunks to the SMB in smaller batches to avoid large bursts that
  // could fill up the SMB completely and lead to stalls or data loss. We'll
  // continue writing the chunks asynchronously. We need to ensure that we write
  // at least one chunk now, otherwise the service might receive and erroneously
  // start reading from a future chunk committed by the underlying trace writer.
  LocalBufferCommitter::CommitRemainingDataInBatches(std::move(committer));

  return next_chunk_id;
}

}  // namespace perfetto
