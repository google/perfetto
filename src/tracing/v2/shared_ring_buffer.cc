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

#include "src/tracing/v2/shared_ring_buffer.h"

#include <string.h>
#include <atomic>

#include "perfetto/base/logging.h"
#include "perfetto/ext/tracing/core/basic_types.h"

namespace perfetto {

using ChunkHeader = SharedRingBuffer::ChunkHeader;

// --- SharedRingBuffer ---

SharedRingBuffer::SharedRingBuffer(void* start, size_t size)
    : start_(static_cast<uint8_t*>(start)), size_(size) {
  PERFETTO_CHECK(start != nullptr);
  PERFETTO_CHECK(size >= kRingBufferHeaderSize + kChunkSize);
  PERFETTO_CHECK(reinterpret_cast<uintptr_t>(start) % 8 == 0);

  num_chunks_ = (size - kRingBufferHeaderSize) / kChunkSize;
  PERFETTO_CHECK(num_chunks_ > 0);
}

SharedRingBuffer::Writer SharedRingBuffer::CreateWriter(WriterID writer_id) {
  return SharedRingBuffer_Writer(this, writer_id);
}

// --- SharedRingBuffer_Writer ---

SharedRingBuffer_Writer::SharedRingBuffer_Writer() = default;

SharedRingBuffer_Writer::SharedRingBuffer_Writer(SharedRingBuffer* rb,
                                                 WriterID writer_id)

    : rb_(rb),
      writer_id_(writer_id),
      cached_header_(SharedRingBuffer::ChunkHeader::Pack(writer_id, 0, 0)) {
  PERFETTO_DCHECK(rb);
}

SharedRingBuffer_Writer::SharedRingBuffer_Writer(
    SharedRingBuffer_Writer&& other) noexcept {
  PERFETTO_CHECK(!other.is_writing());
  this->rb_ = other.rb_;
  this->writer_id_ = other.writer_id_;
  if (other.last_chunk_ == other.invalid_chunk()) {
    last_chunk_ = invalid_chunk();
  } else if (other.last_chunk_ == other.garbage_chunk_) {
    last_chunk_ = garbage_chunk_;
  } else {
    last_chunk_ = other.last_chunk_;
  }
  write_off_ = other.write_off_;
  fragment_size_off_ = other.fragment_size_off_;
  cached_header_ = other.cached_header_;

  static_assert(std::is_trivially_destructible_v<SharedRingBuffer_Writer>);
  other.~SharedRingBuffer_Writer();
  new (&other) SharedRingBuffer_Writer();
}

SharedRingBuffer_Writer& SharedRingBuffer_Writer::operator=(
    SharedRingBuffer_Writer&& other) noexcept {
  if (this != &other) {
    this->~SharedRingBuffer_Writer();
    new (this) SharedRingBuffer_Writer(std::move(other));
  }
  return *this;
}

void SharedRingBuffer_Writer::BeginWriteInternal(uint8_t extra_flags) {
  PERFETTO_DCHECK(!is_writing());

  // Try to acquire (or re-acquire) a chunk.
  // - If last_chunk == invalid_chunk_: CAS fails (header is 0), get new chunk.
  // - If last_chunk == garbage_chunk_: CAS succeeds (we maintain its header).
  // - If last_chunk == real chunk: CAS succeeds if not reclaimed.

  PERFETTO_DCHECK(last_chunk_);
  auto* chunk_hdr = reinterpret_cast<std::atomic<uint32_t>*>(last_chunk_);
  const uint32_t kBitAcquiredForWriting =
      static_cast<uint32_t>(SharedRingBuffer::kFlagAcquiredForWriting) << 24;

  uint32_t expected = cached_header_;
  uint32_t desired = expected | kBitAcquiredForWriting | extra_flags;

  // TODO reason on memory order here.

  // TODO reason on the fact that if the reader sets KNeedsRewrite before we
  // acquire (can it happen??) we fail the CAS. but shouldn't happen.
  if (PERFETTO_LIKELY(chunk_hdr->compare_exchange_strong(expected, desired))) {
    // Happy case: we re-acquired the same chunk we used on the last write.
    // write_off_ is still valid since last write. It must be >0 as BeginWrite()
    // takes at least 1 byte to write the fragment size.
    PERFETTO_DCHECK(write_off_ > 0);
    cached_header_ = desired;
  } else {
    AcquireNewChunk(extra_flags);
  }

  // At this point either we (re-)acquired a valid chunk or we got redirected to
  // the garbage chunk.

  PERFETTO_DCHECK(last_chunk_ != invalid_chunk());

  // TODO check if we have any space left in write_ptr (or DCHECK and do that
  // in EndWrite).

  // Reserve 1 byte for the fragment size (patched in EndWrite).
  fragment_size_off_ = write_off_;
  *(payload_start() + write_off_) = 0;
  ++write_off_;
}

void SharedRingBuffer_Writer::EndWriteInternal(uint8_t extra_flags) {
  PERFETTO_DCHECK(is_writing());
  PERFETTO_DCHECK(last_chunk_ != invalid_chunk());

  // Patch the fragment size.
  PERFETTO_DCHECK(write_off_ > fragment_size_off_);
  uint8_t frag_size = write_off_ - fragment_size_off_ - 1;
  *(payload_start() + fragment_size_off_) = frag_size;

  for (;;) {
    // Release the chunk - update header to clear acquired_for_writing.
    // For garbage_chunk_, this just updates our local header (no contention).
    // For real chunks, this does a CAS to release.
    auto* chunk_hdr = reinterpret_cast<std::atomic<uint32_t>*>(last_chunk_);

    const uint8_t payload_size = write_off_;

    // Invalidate means that we forget about the current chunk after the release
    // so that on the next BeginWrite we try to acquire a new chunk. We do this
    // when we see that there is not enough space left in the current chunk, to
    // avoid a useless fragmentation on the next round. The 4 here is an
    // optimization, but anything >=1 is load bearing. BeginWriteInternal()
    // assumes that if we try to re-acquire the same chunk, we have space left
    // at least for the 1 byte packet header.
    bool invalidate = payload_size >= SharedRingBuffer::kChunkPayloadSize - 4;
    uint8_t flags = ChunkHeader::GetFlags(cached_header_);
    flags &= ~SharedRingBuffer::kFlagAcquiredForWriting;
    flags |= extra_flags;
    uint32_t new_hdr = ChunkHeader::Pack(writer_id_, payload_size, flags);

    // For garbage_chunk_, this CAS always succeeds (no contention) as each
    // writer has its own private garbage chunk as member field.
    // For real chunks, it may fail if reader set kFlagNeedsRewrite (below).
    uint32_t expected = cached_header_;
    if (PERFETTO_LIKELY(chunk_hdr->compare_exchange_strong(
            expected, new_hdr, std::memory_order_release,
            std::memory_order_relaxed))) {
      cached_header_ = new_hdr;
      if (invalidate) {
        last_chunk_ = invalid_chunk();
        write_off_ = 0;
        // We could set cached_header_ = 0 but it doesn't matter. The
        // `invalid_chunk_header_` will cause the CAS CAS of the next
        // BeginWrite() to fail regardless.
      }
      return;
    }

    // The CAS failed. `expected` now contains the current header.
    // This can happen in the rare case when the reader does a read pass while
    // we are writing a chunk. If that happens the reader marks the header as
    // kFlagNeedsRewrite and skips it. When we get here we and see the flag we
    // have to copy the whole chunk into a new one, and mark this as free.
    // In theory we could allow the reader to read the N-1 fragments and only
    // rewrite the newest fragments. But that adds further complexity in case of
    // fragmentation to keep the flags consistent, which are not worth it.
    // Copying a whole chunk unconditionally is likely faster than dancing
    // around extra instructions and branches to handle that complexity.

    uint8_t actual_flags = ChunkHeader::GetFlags(expected);
    uint8_t old_flags = ChunkHeader::GetFlags(cached_header_);
    if (actual_flags !=
        (cached_header_ | SharedRingBuffer::kFlagNeedsRewrite)) {
      // There is no other reason why the CAS should fail. Nobody else should
      // ever touch a chunk while we own it. If this happens, this is a bug.
      PERFETTO_FATAL("shmem buffer corrupted. old=%x actual=%x", old_flags,
                     actual_flags);
    }

    // AcquireNewChunk clobbers writer_off_ and payload_start pointing to the
    // new chunk, hence the caching here.
    uint8_t* old_payload = payload_start();
    size_t old_payload_size = write_off_;

    AcquireNewChunk(/*extra_flags=*/0);
    WriteBytesUnchecked(old_payload, old_payload_size);
    chunk_hdr->store(0, std::memory_order_release);  // Free the old chunk.

    // The next iteration of the loop will re-transact, set the header and
    // release the chunk. We would  have to be catastrophicall unlucky to hit
    // another kFlagNeedsRewrite (that can only happen if, by the time we
    // re-transact, the reader decides to do another read pass AND it happens to
    // clash again with us).
  }
}

// Acquires the next free chunk in the ring buffer. If there is no free chunk
// it falls back on the local garbage chunk and marks a data loss.
bool SharedRingBuffer_Writer::AcquireNewChunk(uint8_t extra_flags) {
  SharedRingBuffer::RingBufferHeader* rb_hdr = rb_->header();
  // TODO cache locally num_chunks to remove one pointer chasing.
  const size_t num_chunks = rb_->num_chunks();
  const uint8_t flags = SharedRingBuffer::kFlagAcquiredForWriting | extra_flags;
  const uint32_t new_hdr = ChunkHeader::Pack(writer_id_, 0, flags);
  uint32_t wr_off = rb_hdr->wr_off.load(std::memory_order_relaxed);
  uint32_t rd_off;

  for (;;) {
    // TODO think about mem ordering
    rd_off = rb_hdr->rd_off.load(std::memory_order_relaxed);
    uint32_t next_wr_off = static_cast<uint32_t>((wr_off + 1) % num_chunks);

    if (next_wr_off == rd_off) {
      // The buffer is full. Go to the epilogue and return the garbage chunk.
      break;
    }

    // TODO I think this can be mem_order_relaxed because the next one below is
    // strong.
    if (!rb_hdr->wr_off.compare_exchange_weak(wr_off, next_wr_off,
                                              std::memory_order_relaxed)) {
      // Another thread raced on incrementing the wr_off. Try again.
      // When CES fails, wr_off gets updated with the most recent value.
      continue;
    }

    // The fact that we incremented succesfully wr_off doesn't give us any
    // guarrantee that cunk[wr_off] is free. That might belong to a
    // thread descheduled while it was writing onto it. We need to be able to
    // acquire the chunk header, which is the real linearization point.

    uint8_t* chunk = rb_->chunk_at(wr_off);
    std::atomic<uint32_t>* c_hdr = rb_->chunk_header_atomic(chunk);
    uint32_t expected_free_hdr = 0;

    if (!c_hdr->compare_exchange_strong(expected_free_hdr, new_hdr))
      continue;

    // Success.
    last_chunk_ = chunk;
    cached_header_ = new_hdr;
    write_off_ = 0;
    return true;
  }  // for(;;)

  // ---
  // If we get here there are no chunks in the buffer. Redirect to the garbage
  // chunk. The data written here will be discarded, but avoids adding extra
  // branches in the code to deal with this edge case.

  // TODO mark data loss somewhere in local state. if we switch back to a normal
  // chunk we should invalidate somehow the first fragment.

  rb_->IncrementDataLosses();

  // Initialize garbage_chunk_ with a proper header.
  reinterpret_cast<std::atomic<uint32_t>*>(garbage_chunk_)
      ->store(new_hdr, std::memory_order_relaxed);

  last_chunk_ = garbage_chunk_;
  cached_header_ = new_hdr;
  write_off_ = 0;
  return false;
}

void SharedRingBuffer_Writer::WriteBytesSlow(const void* data, size_t len) {
  const uint8_t* src = static_cast<const uint8_t*>(data);
  while (len > 0) {
    size_t remaining = payload_avail();
    if (remaining == 0) {
      EndWriteInternal(SharedRingBuffer::kFlagContinuesOnNextChunk);
      BeginWriteInternal(SharedRingBuffer::kFlagContinuesFromPrevChunk);
      remaining = payload_avail();
      PERFETTO_CHECK(remaining > 0);
    }
    size_t to_write = (len < remaining) ? len : remaining;
    WriteBytesUnchecked(src, to_write);
    src += to_write;
    len -= to_write;
  }
}

// --- SharedRingBuffer_Reader ---

SharedRingBuffer_Reader::SharedRingBuffer_Reader() = default;

SharedRingBuffer_Reader::SharedRingBuffer_Reader(SharedRingBuffer* rb)
    : rb_(rb) {
  PERFETTO_DCHECK(rb);
}

SharedRingBuffer_Reader::SharedRingBuffer_Reader(
    SharedRingBuffer_Reader&& other) noexcept
    : rb_(other.rb_),
      writer_states_(std::move(other.writer_states_)),
      completed_messages_(std::move(other.completed_messages_)) {
  other.rb_ = nullptr;
}

SharedRingBuffer_Reader& SharedRingBuffer_Reader::operator=(
    SharedRingBuffer_Reader&& other) noexcept {
  if (this != &other) {
    this->~SharedRingBuffer_Reader();
    new (this) SharedRingBuffer_Reader(std::move(other));
  }
  return *this;
}

// Returns false if there are no more chunks that can be read (we hit the write
// pointer). True if some data has been read, or the chunk has been skipped.
bool SharedRingBuffer_Reader::ReadOneChunk() {
  PERFETTO_DCHECK(rb_);

  SharedRingBuffer::RingBufferHeader* rb_hdr = rb_->header();
  const size_t num_chunks = rb_->num_chunks();

  // Load rd_off and wr_off. Since we're the only reader, rd_off won't change
  // under us. wr_off might increase but that's fine.
  uint32_t rd_off = rb_hdr->rd_off.load(std::memory_order_relaxed);
  uint32_t wr_off = rb_hdr->wr_off.load(std::memory_order_acquire);

  if (rd_off == wr_off) {
    return false;  // Buffer is empty.
  }

  // When reading a chunk we can potentially overlap with writers and hit the
  // following scenarios:
  // 1. (Happy case) the chunk is idle (i.e. NOT kFlagAcquiredForWriting) when
  //    we start reading. When we finish reading and CAS the header (to mark it
  //    as free), the CAS succeeds. The chunk has not changed.
  // 2. The chunk is idle when we start reading but when we finish reading the
  //    header changed and the CAS fails. We have two subcases:
  //    2a. The chunk is idle again (i.e. by the time we did the read, the
  //        writer did acquire for writing, add data, and release it). In this
  //        case we try again the whole read (eventually we are going to
  //        converge as the chunk has a limited size).
  //    2b. The chunk is now kFlagAcquiredForWriting. This is the same of case
  //        3 below.
  // 3. The chunk is kFlagAcquiredForWriting when we start reading (or 2b, when
  //    we finish). In this case we do NOT want to stall the reader, because if
  //    we are unlucky and the writer get descheduled, the chunk could be
  //    acquired for long time. Instead we just set the kNeedsRewrite bit, so
  //    when the writer does a CAS in EndWrite(), it has to acquire a new-chunk,
  //    copy-over the current contents, and free up this one.
  //    We might fail the CAS to set the kNeedsRewrite. If this happens we have
  //    to repeat the whole process (again with a limit, because a chunk can be
  //    acquired and released by a write at most (256-4) times).

  uint8_t* chunk = rb_->chunk_at(rd_off);
  std::atomic<uint32_t>* hdr_atomic = rb_->chunk_header_atomic(chunk);
  uint32_t hdr = hdr_atomic->load(std::memory_order_acquire);

  for (;;) {
    // If chunk header is 0, it was freed (e.g., by writer after needs_rewrite).
    // We are done here, move on to the next one.
    if (PERFETTO_UNLIKELY(hdr == 0))
      break;  // Break the loop, increment rd_off and return true.

    const uint8_t payload_size = ChunkHeader::GetPayloadSize(hdr);
    uint8_t flags = ChunkHeader::GetFlags(hdr);
    if (payload_size > SharedRingBuffer::kChunkPayloadSize) {
      PERFETTO_DFATAL("Shmem ring buffer corrupted, payload_size %u too big",
                      payload_size);
      // Likely a HW bit corruption or a writer bug. We should not crash here
      // because the writer could be another untrusted process who is trying to
      // DoS us. Just clear the chunk and move on.
      // TODO signal data loss.
      hdr_atomic->store(0, std::memory_order_release);
      break;
    }

    // TODO the two CAS below should be (std::memory_order_release,
    // std::memory_order_acquire) think more.

    if (!(flags & SharedRingBuffer::kFlagAcquiredForWriting)) {
      // The chunk is idle (released) ...for now. This is our happy path.
      uint8_t payload[SharedRingBuffer::kChunkPayloadSize];
      memcpy(payload, chunk + SharedRingBuffer::kChunkHeaderSize, payload_size);
      if (PERFETTO_LIKELY(hdr_atomic->compare_exchange_strong(hdr, 0))) {
        // The chunk is still idle and its header didn't change. We are done.
        ProcessChunkPayload(payload, payload_size, hdr);
        break;  // Case 1. Break the loop, increment rd_off and return true.
      }
      // The CAS failed. `hdr` has been updated with the current header.
      // At this point either the writer acquired the chunk, or it did a full
      // acquire -> append -> release cycle and changed the payload_size.
      // In any case continue below.
      flags = ChunkHeader::GetFlags(hdr);  // Update flags for the check below.
    }

    // This is deliberately NOT an else branch. If the CAS above fails, flags
    // gets updated and might become kFlagAcquiredForWriting.
    if (flags & SharedRingBuffer::kFlagAcquiredForWriting) {
      // The chunk is acquired. We don't want to stall the reader on it. So we
      // just "invalidate" it by setting kFlagNeedsRewrite, and instructing the
      // writer to just re-iterate once it has reached its end.
      uint32_t new_hdr =
          hdr |
          (static_cast<uint32_t>(SharedRingBuffer::kFlagNeedsRewrite) << 24);
      if (PERFETTO_LIKELY(hdr_atomic->compare_exchange_strong(hdr, new_hdr))) {
        break;
      }
      // If we get here, either the writer made more progress (either finished
      // its write and released the chunk, or finished and re-acquired again).
      // In either case, re-transact by continuing the for loop below).
    }

    // If we get here, the chunk was idle when we started reading, but when we
    // finished either got acquired, or a whole new fragment was appended.
    // Also in this case, we re-transact.

    // We do not need to re-read `hdr` on every iteration, because all the code
    // flow that leads to this point imply a failed CAS, which updates `hdr`.
  }  // for(;;)

  uint32_t next_rd = static_cast<uint32_t>((rd_off + 1) % num_chunks);
  rb_hdr->rd_off.store(next_rd, std::memory_order_release);
  return true;
}

void SharedRingBuffer_Reader::ProcessChunkPayload(const uint8_t* payload,
                                                  uint8_t payload_size,
                                                  uint32_t header) {
  const WriterID writer_id = ChunkHeader::GetWriterID(header);
  const uint8_t flags = ChunkHeader::GetFlags(header);

  const bool continues_from_prev =
      (flags & SharedRingBuffer::kFlagContinuesFromPrevChunk) != 0;
  const bool continues_on_next =
      (flags & SharedRingBuffer::kFlagContinuesOnNextChunk) != 0;
  const bool data_loss = (flags & SharedRingBuffer::kFlagDataLoss) != 0;

  // Get or create writer state.
  WriterState& ws = writer_states_[writer_id];

  // If there was data loss and this chunk continues from a previous one,
  // the pending data is corrupted - discard it. Only check on first pass.
  if (data_loss && continues_from_prev) {
    ws.pending_data.clear();
  }

  for (size_t off = 0; off < payload_size;) {
    // Each fragment is: [size:1 byte][data:size bytes]
    const bool is_first_frag = (off == 0);
    uint8_t frag_size = payload[off++];

    if (off + frag_size > payload_size) {
      PERFETTO_DFATAL("Fragment size exceeds payload bounds");
      // TODO signal data loss
      break;
    }

    const uint8_t* frag_data = payload + off;
    off += frag_size;

    // Determine if this is a continuation fragment or a new fragment.
    bool is_last_frag = (off >= payload_size);

    // Case 1: First fragment and continues_from_prev - append to pending data.
    if (is_first_frag && continues_from_prev) {
      if (!data_loss) {
        ws.pending_data.append(reinterpret_cast<const char*>(frag_data),
                               frag_size);
      }
      // If this is not the last fragment, or if there's no continuation,
      // the message is complete.
      if (!is_last_frag || !continues_on_next) {
        if (!ws.pending_data.empty()) {
          completed_messages_.push_back(
              CompletedMessage{writer_id, std::move(ws.pending_data)});
          ws.pending_data.clear();
        }
      }
      continue;
    }

    // Case 2: Last fragment and continues_on_next - stash for later.
    if (is_last_frag && continues_on_next) {
      ws.pending_data.append(reinterpret_cast<const char*>(frag_data),
                             frag_size);
      continue;
    }

    // Case 3: Complete fragment within this chunk.
    completed_messages_.push_back(CompletedMessage{
        writer_id,
        std::string(reinterpret_cast<const char*>(frag_data), frag_size)});
  }
}

}  // namespace perfetto
