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

#include "perfetto/tracing/core/startup_trace_writer.h"

#include <numeric>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/trace/trace_packet.pbzero.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/startup_trace_writer_registry.h"
#include "src/tracing/core/patch_list.h"
#include "src/tracing/core/shared_memory_arbiter_impl.h"

using ChunkHeader = perfetto::SharedMemoryABI::ChunkHeader;

namespace perfetto {

namespace {

SharedMemoryABI::Chunk NewChunk(SharedMemoryArbiterImpl* arbiter,
                                WriterID writer_id,
                                ChunkID chunk_id,
                                bool fragmenting_packet) {
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

  return arbiter->GetNewChunk(header);
}

class LocalBufferReader {
 public:
  LocalBufferReader(protozero::ScatteredHeapBuffer* buffer)
      : buffer_slices_(buffer->slices()), cur_slice_(buffer_slices_.begin()) {}

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
  const std::vector<protozero::ScatteredHeapBuffer::Slice>& buffer_slices_;

  // Iterator pointing to slice in |buffer_slices_| that we're currently reading
  // from.
  std::vector<protozero::ScatteredHeapBuffer::Slice>::const_iterator cur_slice_;
  // Read offset in the current slice in bytes.
  size_t cur_slice_offset_ = 0;
};

}  // namespace

StartupTraceWriter::StartupTraceWriter(
    std::shared_ptr<StartupTraceWriterRegistryHandle> registry_handle)
    : registry_handle_(std::move(registry_handle)),
      memory_buffer_(new protozero::ScatteredHeapBuffer()),
      memory_stream_writer_(
          new protozero::ScatteredStreamWriter(memory_buffer_.get())) {
  memory_buffer_->set_writer(memory_stream_writer_.get());
  PERFETTO_DETACH_FROM_THREAD(writer_thread_checker_);
}

StartupTraceWriter::StartupTraceWriter(
    std::unique_ptr<TraceWriter> trace_writer)
    : was_bound_(true), trace_writer_(std::move(trace_writer)) {}

StartupTraceWriter::~StartupTraceWriter() {
  if (registry_handle_)
    registry_handle_->OnWriterDestroyed(this);
}

bool StartupTraceWriter::BindToArbiter(SharedMemoryArbiterImpl* arbiter,
                                       BufferID target_buffer) {
  // Create and destroy trace writer without holding lock, since this will post
  // a task and task posting may trigger a trace event, which would cause a
  // deadlock. This may create a few more trace writers than necessary in cases
  // where a concurrent write is in progress (other than causing some
  // computational overhead, this is not problematic).
  auto trace_writer = arbiter->CreateTraceWriter(target_buffer);

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

    trace_writer_ = std::move(trace_writer);
    ChunkID next_chunk_id = CommitLocalBufferChunks(
        arbiter, trace_writer_->writer_id(), target_buffer);

    // The real TraceWriter should start writing at the subsequent chunk ID.
    bool success = trace_writer_->SetFirstChunkId(next_chunk_id);
    PERFETTO_DCHECK(success);

    memory_stream_writer_.reset();
    memory_buffer_.reset();
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
    std::lock_guard<std::mutex> lock(lock_);
    if (trace_writer_) {
      PERFETTO_DCHECK(!cur_packet_);
      // Set the |was_bound_| flag to avoid locking in future calls to
      // NewTracePacket().
      was_bound_ = true;
      return trace_writer_->NewTracePacket();
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
  packet_sizes_.push_back(packet_size);

  // Write is complete, reset the flag to allow binding.
  std::lock_guard<std::mutex> lock(lock_);
  PERFETTO_DCHECK(write_in_progress_);
  write_in_progress_ = false;
}

ChunkID StartupTraceWriter::CommitLocalBufferChunks(
    SharedMemoryArbiterImpl* arbiter,
    WriterID writer_id,
    BufferID target_buffer) {
  // TODO(eseckler): Write and commit these chunks asynchronously. This would
  // require that the service is informed of the missing initial chunks, e.g. by
  // committing our first chunk here before the new trace writer has a chance to
  // commit its first chunk. Otherwise the service wouldn't know to wait for our
  // chunks.

  if (packet_sizes_.empty() || !writer_id)
    return 0;

  memory_buffer_->AdjustUsedSizeOfCurrentSlice();
  LocalBufferReader local_buffer_reader(memory_buffer_.get());

  PERFETTO_DCHECK(local_buffer_reader.TotalUsedSize() ==
                  std::accumulate(packet_sizes_.begin(), packet_sizes_.end(),
                                  static_cast<size_t>(0u)));

  ChunkID next_chunk_id = 0;
  SharedMemoryABI::Chunk cur_chunk =
      NewChunk(arbiter, writer_id, next_chunk_id++, false);

  size_t max_payload_size = cur_chunk.payload_size();
  size_t cur_payload_size = 0;
  uint16_t cur_num_packets = 0;
  size_t total_num_packets = packet_sizes_.size();
  PatchList empty_patch_list;
  for (size_t packet_idx = 0; packet_idx < total_num_packets; packet_idx++) {
    uint32_t packet_size = packet_sizes_[packet_idx];
    uint32_t remaining_packet_size = packet_size;
    ++cur_num_packets;
    do {
      uint32_t fragment_size = static_cast<uint32_t>(
          std::min(static_cast<size_t>(remaining_packet_size),
                   max_payload_size - cur_payload_size -
                       SharedMemoryABI::kPacketHeaderSize));
      // Write packet header, i.e. the fragment size.
      protozero::proto_utils::WriteRedundantVarInt(
          fragment_size, cur_chunk.payload_begin() + cur_payload_size);
      cur_payload_size += SharedMemoryABI::kPacketHeaderSize;

      // Copy packet content into the chunk.
      size_t bytes_read = local_buffer_reader.ReadBytes(
          &cur_chunk, fragment_size, cur_payload_size);
      PERFETTO_DCHECK(bytes_read == fragment_size);

      cur_payload_size += fragment_size;
      remaining_packet_size -= fragment_size;

      bool last_write =
          packet_idx == total_num_packets - 1 && remaining_packet_size == 0;

      // We should return the current chunk if we've filled its payload, reached
      // the maximum number of packets, or wrote everything we wanted to.
      bool return_chunk =
          cur_payload_size >=
              max_payload_size - SharedMemoryABI::kPacketHeaderSize ||
          cur_num_packets == ChunkHeader::Packets::kMaxCount || last_write;

      if (return_chunk) {
        auto new_packet_count =
            cur_chunk.IncreasePacketCountTo(cur_num_packets);
        PERFETTO_DCHECK(new_packet_count == cur_num_packets);

        bool is_fragmenting = remaining_packet_size > 0;
        if (is_fragmenting) {
          PERFETTO_DCHECK(cur_payload_size == max_payload_size);
          cur_chunk.SetFlag(ChunkHeader::kLastPacketContinuesOnNextChunk);
        }

        arbiter->ReturnCompletedChunk(std::move(cur_chunk), target_buffer,
                                      &empty_patch_list);

        // Avoid creating a new chunk after the last write.
        if (!last_write) {
          cur_chunk =
              NewChunk(arbiter, writer_id, next_chunk_id++, is_fragmenting);
          max_payload_size = cur_chunk.payload_size();
          cur_payload_size = 0;
          cur_num_packets = is_fragmenting ? 1 : 0;
        } else {
          PERFETTO_DCHECK(!is_fragmenting);
        }
      }
    } while (remaining_packet_size > 0);
  }

  // The last chunk should have been returned.
  PERFETTO_DCHECK(!cur_chunk.is_valid());
  // We should have read all data from the local buffer.
  PERFETTO_DCHECK(local_buffer_reader.DidReadAllData());

  return next_chunk_id;
}

}  // namespace perfetto
