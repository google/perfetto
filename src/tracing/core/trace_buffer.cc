/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/tracing/core/trace_buffer.h"

#include <limits>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/tracing/core/shared_memory_abi.h"
#include "perfetto/tracing/core/trace_packet.h"

#define TRACE_BUFFER_VERBOSE_LOGGING() 0  // Set to 1 when debugging unittests.
#if TRACE_BUFFER_VERBOSE_LOGGING()
#define TRACE_BUFFER_DLOG PERFETTO_DLOG
namespace {
std::string HexDump(const uint8_t* src, size_t size) {
  std::string buf;
  buf.reserve(4096 * 4);
  char line[64];
  char* c = line;
  for (size_t i = 0; i < size; i++) {
    c += sprintf(c, "%02x ", src[i]);
    if (i % 16 == 15) {
      buf.append("\n");
      buf.append(line);
      c = line;
    }
  }
  return buf;
}
}  // namespace
#else
#define TRACE_BUFFER_DLOG(...) void()
#endif

namespace perfetto {

namespace {
constexpr uint8_t kFirstPacketContinuesFromPrevChunk =
    SharedMemoryABI::ChunkHeader::kFirstPacketContinuesFromPrevChunk;
constexpr uint8_t kLastPacketContinuesOnNextChunk =
    SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk;
constexpr uint8_t kChunkNeedsPatching =
    SharedMemoryABI::ChunkHeader::kChunkNeedsPatching;
}  // namespace.

constexpr size_t TraceBuffer::ChunkRecord::kMaxSize;
constexpr size_t TraceBuffer::InlineChunkHeaderSize = sizeof(ChunkRecord);

// static
std::unique_ptr<TraceBuffer> TraceBuffer::Create(size_t size_in_bytes) {
  std::unique_ptr<TraceBuffer> trace_buffer(new TraceBuffer());
  if (!trace_buffer->Initialize(size_in_bytes))
    return nullptr;
  return trace_buffer;
}

TraceBuffer::TraceBuffer() {
  // See comments in ChunkRecord for the rationale of this.
  static_assert(sizeof(ChunkRecord) == sizeof(SharedMemoryABI::PageHeader) +
                                           sizeof(SharedMemoryABI::ChunkHeader),
                "ChunkRecord out of sync with the layout of SharedMemoryABI");
}

TraceBuffer::~TraceBuffer() = default;

bool TraceBuffer::Initialize(size_t size) {
  static_assert(
      base::kPageSize % sizeof(ChunkRecord) == 0,
      "sizeof(ChunkRecord) must be an integer divider of a page size");
  PERFETTO_CHECK(size % base::kPageSize == 0);
  data_ = base::PageAllocator::AllocateMayFail(size);
  if (!data_) {
    PERFETTO_ELOG("Trace buffer allocation failed (size: %zu)", size);
    return false;
  }
  size_ = size;
  max_chunk_size_ = std::min(size, ChunkRecord::kMaxSize);
  wptr_ = begin();
  index_.clear();
  last_chunk_id_.clear();
  read_iter_ = GetReadIterForSequence(index_.end());
  return true;
}

// Note: |src| points to a shmem region that is shared with the producer. Assume
// that the producer is malicious and will change the content of |src|
// while we execute here. Don't do any processing on it other than memcpy().
void TraceBuffer::CopyChunkUntrusted(ProducerID producer_id_trusted,
                                     uid_t producer_uid_trusted,
                                     WriterID writer_id,
                                     ChunkID chunk_id,
                                     uint16_t num_fragments,
                                     uint8_t chunk_flags,
                                     const uint8_t* src,
                                     size_t size) {
  // |record_size| = |size| + sizeof(ChunkRecord), rounded up to avoid to end
  // up in a fragmented state where size_to_end() < sizeof(ChunkRecord).
  const size_t record_size =
      base::AlignUp<sizeof(ChunkRecord)>(size + sizeof(ChunkRecord));
  if (PERFETTO_UNLIKELY(record_size > max_chunk_size_)) {
    stats_.abi_violations++;
    PERFETTO_DCHECK(suppress_sanity_dchecks_for_testing_);
    return;
  }

  TRACE_BUFFER_DLOG("CopyChunk @ %lu, size=%zu", wptr_ - begin(), record_size);

#if PERFETTO_DCHECK_IS_ON()
  changed_since_last_read_ = true;
#endif

  // If there isn't enough room from the given write position. Write a padding
  // record to clear the end of the buffer and wrap back.
  const size_t cached_size_to_end = size_to_end();
  if (PERFETTO_UNLIKELY(record_size > cached_size_to_end)) {
    size_t res = DeleteNextChunksFor(cached_size_to_end);
    PERFETTO_DCHECK(res <= cached_size_to_end);
    AddPaddingRecord(cached_size_to_end);
    wptr_ = begin();
    stats_.write_wrap_count++;
    PERFETTO_DCHECK(size_to_end() >= record_size);
  }

  ChunkRecord record(record_size);
  record.producer_id = producer_id_trusted;
  record.chunk_id = chunk_id;
  record.writer_id = writer_id;
  record.num_fragments = num_fragments;
  record.flags = chunk_flags;

  // At this point either |wptr_| points to an untouched part of the buffer
  // (i.e. *wptr_ == 0) or we are about to overwrite one or more ChunkRecord(s).
  // In the latter case we need to first figure out where the next valid
  // ChunkRecord is (if it exists) and add padding between the new record.
  // Example ((w) == write cursor):
  //
  // Initial state (wtpr_ == 0):
  // |0 (w)    |10               |30                  |50
  // +---------+-----------------+--------------------+--------------------+
  // | Chunk 1 | Chunk 2         | Chunk 3            | Chunk 4            |
  // +---------+-----------------+--------------------+--------------------+
  //
  // Let's assume we now want now write a 5th Chunk of size == 35. The final
  // state should look like this:
  // |0                                |35 (w)         |50
  // +---------------------------------+---------------+--------------------+
  // | Chunk 5                         | Padding Chunk | Chunk 4            |
  // +---------------------------------+---------------+--------------------+

  // Deletes all chunks from |wptr_| to |wptr_| + |record_size|.
  size_t padding_size = DeleteNextChunksFor(record_size);

  // Now first insert the new chunk. At the end, if necessary, add the padding.
  ChunkMeta::Key key(record);
  stats_.chunks_written++;
  stats_.bytes_written += size;
  auto it_and_inserted =
      index_.emplace(key, ChunkMeta(GetChunkRecordAt(wptr_), num_fragments,
                                    chunk_flags, producer_uid_trusted));
  if (PERFETTO_UNLIKELY(!it_and_inserted.second)) {
    // More likely a producer bug, but could also be a malicious producer.
    stats_.abi_violations++;
    PERFETTO_DCHECK(suppress_sanity_dchecks_for_testing_);
    index_.erase(it_and_inserted.first);
    index_.emplace(key, ChunkMeta(GetChunkRecordAt(wptr_), num_fragments,
                                  chunk_flags, producer_uid_trusted));
  }
  TRACE_BUFFER_DLOG("  copying @ [%lu - %lu] %zu", wptr_ - begin(),
                    wptr_ - begin() + record_size, record_size);
  WriteChunkRecord(record, src, size);
  TRACE_BUFFER_DLOG("Chunk raw: %s", HexDump(wptr_, record_size).c_str());
  wptr_ += record_size;
  if (wptr_ >= end()) {
    PERFETTO_DCHECK(padding_size == 0);
    wptr_ = begin();
    stats_.write_wrap_count++;
  }
  DcheckIsAlignedAndWithinBounds(wptr_);

  last_chunk_id_[std::make_pair(producer_id_trusted, writer_id)] = chunk_id;

  if (padding_size)
    AddPaddingRecord(padding_size);
}

size_t TraceBuffer::DeleteNextChunksFor(size_t bytes_to_clear) {
  // Find the position of the first chunk which begins at or after
  // (|wptr_| + |bytes|). Note that such a chunk might not exist and we might
  // either reach the end of the buffer or a zeroed region of the buffer.
  uint8_t* next_chunk_ptr = wptr_;
  uint8_t* search_end = wptr_ + bytes_to_clear;
  TRACE_BUFFER_DLOG("Delete [%zu %zu]", wptr_ - begin(), search_end - begin());
  DcheckIsAlignedAndWithinBounds(wptr_);
  PERFETTO_DCHECK(search_end <= end());
  while (next_chunk_ptr < search_end) {
    const ChunkRecord& next_chunk = *GetChunkRecordAt(next_chunk_ptr);
    TRACE_BUFFER_DLOG(
        "  scanning chunk [%zu %zu] (valid=%d)", next_chunk_ptr - begin(),
        next_chunk_ptr - begin() + next_chunk.size, next_chunk.is_valid());

    // We just reached the untouched part of the buffer, it's going to be all
    // zeroes from here to end().
    // Optimization: if during Initialize() we fill the buffer with padding
    // records we could get rid of this branch.
    if (PERFETTO_UNLIKELY(!next_chunk.is_valid())) {
      // This should happen only at the first iteration. The zeroed area can
      // only begin precisely at the |wptr_|, not after. Otherwise it means that
      // we wrapped but screwed up the ChunkRecord chain.
      PERFETTO_DCHECK(next_chunk_ptr == wptr_);
      return 0;
    }

    // Remove |next_chunk| from the index, unless it's a padding record (padding
    // records are not part of the index).
    if (PERFETTO_LIKELY(!next_chunk.is_padding)) {
      ChunkMeta::Key key(next_chunk);
      auto it = index_.find(key);
      bool removed = false;
      if (PERFETTO_LIKELY(it != index_.end())) {
        const ChunkMeta& meta = it->second;
        if (PERFETTO_UNLIKELY(meta.num_fragments_read < meta.num_fragments))
          stats_.chunks_overwritten++;
        index_.erase(it);
        removed = true;
      }
      TRACE_BUFFER_DLOG("  del index {%" PRIu32 ",%" PRIu32
                        ",%u} @ [%lu - %lu] %zu",
                        key.producer_id, key.writer_id, key.chunk_id,
                        next_chunk_ptr - begin(),
                        next_chunk_ptr - begin() + next_chunk.size, removed);
      PERFETTO_DCHECK(removed);
    }

    next_chunk_ptr += next_chunk.size;

    // We should never hit this, unless we managed to screw up while writing
    // to the buffer and breaking the ChunkRecord(s) chain.
    // TODO(primiano): Write more meaningful logging with the status of the
    // buffer, to get more actionable bugs in case we hit this.
    PERFETTO_CHECK(next_chunk_ptr <= end());
  }
  PERFETTO_DCHECK(next_chunk_ptr >= search_end && next_chunk_ptr <= end());
  return static_cast<size_t>(next_chunk_ptr - search_end);
}

void TraceBuffer::AddPaddingRecord(size_t size) {
  PERFETTO_DCHECK(size >= sizeof(ChunkRecord) && size <= ChunkRecord::kMaxSize);
  ChunkRecord record(size);
  record.is_padding = 1;
  TRACE_BUFFER_DLOG("AddPaddingRecord @ [%lu - %lu] %zu", wptr_ - begin(),
                    wptr_ - begin() + size, size);
  WriteChunkRecord(record, nullptr, size - sizeof(ChunkRecord));
  // |wptr_| is deliberately not advanced when writing a padding record.
}

bool TraceBuffer::TryPatchChunkContents(ProducerID producer_id,
                                        WriterID writer_id,
                                        ChunkID chunk_id,
                                        const Patch* patches,
                                        size_t patches_size,
                                        bool other_patches_pending) {
  ChunkMeta::Key key(producer_id, writer_id, chunk_id);
  auto it = index_.find(key);
  if (it == index_.end()) {
    stats_.patches_failed++;
    return false;
  }
  ChunkMeta& chunk_meta = it->second;

  // Check that the index is consistent with the actual ProducerID/WriterID
  // stored in the ChunkRecord.
  PERFETTO_DCHECK(ChunkMeta::Key(*chunk_meta.chunk_record) == key);
  uint8_t* chunk_begin = reinterpret_cast<uint8_t*>(chunk_meta.chunk_record);
  PERFETTO_DCHECK(chunk_begin >= begin());
  uint8_t* chunk_end = chunk_begin + chunk_meta.chunk_record->size;
  PERFETTO_DCHECK(chunk_end <= end());

  static_assert(Patch::kSize == SharedMemoryABI::kPacketHeaderSize,
                "Patch::kSize out of sync with SharedMemoryABI");

  for (size_t i = 0; i < patches_size; i++) {
    uint8_t* ptr =
        chunk_begin + sizeof(ChunkRecord) + patches[i].offset_untrusted;
    TRACE_BUFFER_DLOG("PatchChunk {%" PRIu32 ",%" PRIu32
                      ",%u} size=%zu @ %zu with {%02x %02x %02x %02x} cur "
                      "{%02x %02x %02x %02x}",
                      producer_id, writer_id, chunk_id, chunk_end - chunk_begin,
                      patches[i].offset_untrusted, patches[i].data[0],
                      patches[i].data[1], patches[i].data[2],
                      patches[i].data[3], ptr[0], ptr[1], ptr[2], ptr[3]);
    if (ptr < chunk_begin + sizeof(ChunkRecord) ||
        ptr > chunk_end - Patch::kSize) {
      // Either the IPC was so slow and in the meantime the writer managed to
      // wrap over |chunk_id| or the producer sent a malicious IPC.
      stats_.patches_failed++;
      return false;
    }

    // DCHECK that we are writing into a zero-filled size field and not into
    // valid data. It relies on ScatteredStreamWriter::ReserveBytes() to
    // zero-fill reservations in debug builds.
    char zero[Patch::kSize]{};
    PERFETTO_DCHECK(memcmp(ptr, &zero, Patch::kSize) == 0);

    memcpy(ptr, &patches[i].data[0], Patch::kSize);
  }
  TRACE_BUFFER_DLOG(
      "Chunk raw (after patch): %s",
      HexDump(chunk_begin, chunk_meta.chunk_record->size).c_str());

  stats_.patches_succeeded += patches_size;
  if (!other_patches_pending) {
    chunk_meta.flags &= ~kChunkNeedsPatching;
    chunk_meta.chunk_record->flags = chunk_meta.flags;
  }
  return true;
}

void TraceBuffer::BeginRead() {
  read_iter_ = GetReadIterForSequence(index_.begin());
#if PERFETTO_DCHECK_IS_ON()
  changed_since_last_read_ = false;
#endif
}

TraceBuffer::SequenceIterator TraceBuffer::GetReadIterForSequence(
    ChunkMap::iterator seq_begin) {
  SequenceIterator iter;
  iter.seq_begin = seq_begin;
  if (seq_begin == index_.end()) {
    iter.cur = iter.seq_end = index_.end();
    return iter;
  }

#if PERFETTO_DCHECK_IS_ON()
  // Either |seq_begin| is == index_.begin() or the item immediately before must
  // belong to a different {ProducerID, WriterID} sequence.
  if (seq_begin != index_.begin() && seq_begin != index_.end()) {
    auto prev_it = seq_begin;
    prev_it--;
    PERFETTO_DCHECK(
        seq_begin == index_.begin() ||
        std::tie(prev_it->first.producer_id, prev_it->first.writer_id) <
            std::tie(seq_begin->first.producer_id, seq_begin->first.writer_id));
  }
#endif

  // Find the first entry that has a greater {ProducerID, WriterID} (or just
  // index_.end() if we reached the end).
  ChunkMeta::Key key = seq_begin->first;  // Deliberate copy.
  key.chunk_id = kMaxChunkID;
  iter.seq_end = index_.upper_bound(key);
  PERFETTO_DCHECK(iter.seq_begin != iter.seq_end);

  // Now find the first entry between [seq_begin, seq_end) that is
  // > last_chunk_id_. This is where we the sequence will start (see notes about
  // wrapping in the header).
  auto producer_and_writer_id = std::make_pair(key.producer_id, key.writer_id);
  PERFETTO_DCHECK(last_chunk_id_.count(producer_and_writer_id));
  iter.wrapping_id = last_chunk_id_[producer_and_writer_id];
  key.chunk_id = iter.wrapping_id;
  iter.cur = index_.upper_bound(key);
  if (iter.cur == iter.seq_end)
    iter.cur = iter.seq_begin;
  return iter;
}

void TraceBuffer::SequenceIterator::MoveNext() {
  // Note: |seq_begin| might be == |seq_end|.
  if (cur == seq_end || cur->first.chunk_id == wrapping_id) {
    cur = seq_end;
    return;
  }
  if (++cur == seq_end)
    cur = seq_begin;
}

bool TraceBuffer::ReadNextTracePacket(TracePacket* packet,
                                      uid_t* producer_uid) {
  // Note: MoveNext() moves only within the next chunk within the same
  // {ProducerID, WriterID} sequence. Here we want to:
  // - return the next patched+complete packet in the current sequence, if any.
  // - return the first patched+complete packet in the next sequence, if any.
  // - return false if none of the above is found.
  TRACE_BUFFER_DLOG("ReadNextTracePacket()");

  // Just in case we forget to initialize it below.
  *producer_uid = kInvalidUid;

#if PERFETTO_DCHECK_IS_ON()
  PERFETTO_DCHECK(!changed_since_last_read_);
#endif
  for (;; read_iter_.MoveNext()) {
    if (PERFETTO_UNLIKELY(!read_iter_.is_valid())) {
      // We ran out of chunks in the current {ProducerID, WriterID} sequence or
      // we just reached the index_.end().

      if (PERFETTO_UNLIKELY(read_iter_.seq_end == index_.end()))
        return false;

      // We reached the end of sequence, move to the next one.
      // Note: ++read_iter_.seq_end might become index_.end(), but
      // GetReadIterForSequence() knows how to deal with that.
      read_iter_ = GetReadIterForSequence(read_iter_.seq_end);
      PERFETTO_DCHECK(read_iter_.is_valid() && read_iter_.cur != index_.end());
    }

    ChunkMeta* chunk_meta = &*read_iter_;

    // If the chunk has holes that are awaiting to be patched out-of-band,
    // skip the current sequence and move to the next one.
    if (chunk_meta->flags & kChunkNeedsPatching) {
      read_iter_.MoveToEnd();
      continue;
    }

    const uid_t trusted_uid = chunk_meta->trusted_uid;

    // At this point we have a chunk in |chunk_meta| that has not been fully
    // read. We don't know yet whether we have enough data to read the full
    // packet (in the case it's fragmented over several chunks) and we are about
    // to find that out. Specifically:
    // A) If the first fragment is unread and is a fragment continuing from a
    //    previous chunk, it means we have missed the previous ChunkID. In
    //    fact, if this wasn't the case, a previous call to ReadNext() shouldn't
    //    have moved the cursor to this chunk.
    // B) Any fragment > 0 && < last is always readable. By definition an inner
    //    packet is never fragmented and hence doesn't require neither stitching
    //    nor any out-of-band patching. The same applies to the last packet
    //    iff it doesn't continue on the next chunk.
    // C) If the last packet (which might be also the only packet in the chunk)
    //    is a fragment and continues on the next chunk, we peek at the next
    //    chunks and, if we have all of them, mark as read and move the cursor.
    //
    // +---------------+   +-------------------+  +---------------+
    // | ChunkID: 1    |   | ChunkID: 2        |  | ChunkID: 3    |
    // |---------------+   +-------------------+  +---------------+
    // | Packet 1      |   |                   |  | ... Packet 3  |
    // | Packet 2      |   | ... Packet 3  ... |  | Packet 4      |
    // | Packet 3  ... |   |                   |  | Packet 5 ...  |
    // +---------------+   +-------------------+  +---------------+

    PERFETTO_DCHECK(chunk_meta->num_fragments_read <=
                    chunk_meta->num_fragments);
    while (chunk_meta->num_fragments_read < chunk_meta->num_fragments) {
      enum { kSkip = 0, kReadOnePacket, kTryReadAhead } action;
      if (chunk_meta->num_fragments_read == 0) {
        if (chunk_meta->flags & kFirstPacketContinuesFromPrevChunk) {
          action = kSkip;  // Case A.
        } else if (chunk_meta->num_fragments == 1 &&
                   (chunk_meta->flags & kLastPacketContinuesOnNextChunk)) {
          action = kTryReadAhead;  // Case C.
        } else {
          action = kReadOnePacket;  // Case B.
        }
      } else if (chunk_meta->num_fragments_read <
                     chunk_meta->num_fragments - 1 ||
                 !(chunk_meta->flags & kLastPacketContinuesOnNextChunk)) {
        action = kReadOnePacket;  // Case B.
      } else {
        action = kTryReadAhead;  // Case C.
      }

      TRACE_BUFFER_DLOG("  chunk %u, packet %hu of %hu, action=%d",
                        read_iter_.chunk_id(), chunk_meta->num_fragments_read,
                        chunk_meta->num_fragments, action);

      if (action == kSkip) {
        // This fragment will be skipped forever, not just in this ReadPacket()
        // iteration. This happens by virtue of ReadNextPacketInChunk()
        // incrementing the |num_fragments_read| and marking the fragment as
        // read even if we didn't really.
        ReadNextPacketInChunk(chunk_meta, nullptr);
        continue;
      }

      if (action == kReadOnePacket) {
        // The easy peasy case B.
        if (PERFETTO_LIKELY(ReadNextPacketInChunk(chunk_meta, packet))) {
          *producer_uid = trusted_uid;
          return true;
        }

        // In extremely rare cases (producer bugged / malicious) the chunk might
        // contain an invalid fragment. In such case we don't want to stall the
        // sequence but just skip the chunk and move on.
        stats_.abi_violations++;
        PERFETTO_DCHECK(suppress_sanity_dchecks_for_testing_);
        break;
      }

      PERFETTO_DCHECK(action == kTryReadAhead);
      ReadAheadResult ra_res = ReadAhead(packet);
      if (ra_res == ReadAheadResult::kSucceededReturnSlices) {
        stats_.readaheads_succeeded++;
        *producer_uid = trusted_uid;
        return true;
      }

      if (ra_res == ReadAheadResult::kFailedMoveToNextSequence) {
        // readahead didn't find a contigous packet sequence. We'll try again
        // on the next ReadPacket() call.
        stats_.readaheads_failed++;

        // TODO(primiano): optimization: this MoveToEnd() is the reason why
        // MoveNext() (that is called in the outer for(;;MoveNext)) needs to
        // deal gracefully with the case of |cur|==|seq_end|. Maybe we can do
        // something to avoid that check by reshuffling the code here?
        read_iter_.MoveToEnd();

        // This break will go back to beginning of the for(;;MoveNext()). That
        // will move to the next sequence because we set the read iterator to
        // its end.
        break;
      }

      PERFETTO_DCHECK(ra_res == ReadAheadResult::kFailedStayOnSameSequence);

      // In this case ReadAhead() might advance |read_iter_|, so we need to
      // re-cache the |chunk_meta| pointer to point to the current chunk.
      chunk_meta = &*read_iter_;
    }  // while(...)  [iterate over packet fragments for the current chunk].
  }    // for(;;MoveNext()) [iterate over chunks].
}

TraceBuffer::ReadAheadResult TraceBuffer::ReadAhead(TracePacket* packet) {
  static_assert(static_cast<ChunkID>(kMaxChunkID + 1) == 0,
                "relying on kMaxChunkID to wrap naturally");
  TRACE_BUFFER_DLOG(" readahead start @ chunk %u", read_iter_.chunk_id());
  ChunkID next_chunk_id = read_iter_.chunk_id() + 1;
  SequenceIterator it = read_iter_;
  for (it.MoveNext(); it.is_valid(); it.MoveNext(), next_chunk_id++) {
    // We should stay within the same sequence while iterating here.
    PERFETTO_DCHECK(it.producer_id() == read_iter_.producer_id() &&
                    it.writer_id() == read_iter_.writer_id());

    TRACE_BUFFER_DLOG("   expected chunk ID: %u, actual ID: %u", next_chunk_id,
                      it.chunk_id());

    if (PERFETTO_UNLIKELY((*it).num_fragments == 0))
      continue;

    // If we miss the next chunk, stop looking in the current sequence and
    // try another sequence. This chunk might come in the near future.
    // The second condition is the edge case of a buggy/malicious
    // producer. The ChunkID is contiguous but its flags don't make sense.
    if (it.chunk_id() != next_chunk_id ||
        PERFETTO_UNLIKELY(
            !((*it).flags & kFirstPacketContinuesFromPrevChunk))) {
      return ReadAheadResult::kFailedMoveToNextSequence;
    }

    // If the chunk is contiguous but has not been patched yet move to the next
    // sequence and try coming back here on the next ReadNextTracePacket() call.
    // TODO(primiano): add a test to cover this, it's a subtle case.
    if ((*it).flags & kChunkNeedsPatching)
      return ReadAheadResult::kFailedMoveToNextSequence;

    // This is the case of an intermediate chunk which contains only one
    // fragment which continues on the next chunk. This is the case for large
    // packets, e.g.: [Packet0, Packet1(0)] [Packet1(1)] [Packet1(2), ...]
    // (Packet1(X) := fragment X of Packet1).
    if ((*it).num_fragments == 1 &&
        ((*it).flags & kLastPacketContinuesOnNextChunk)) {
      continue;
    }

    // We made it! We got all fragments for the packet without holes.
    TRACE_BUFFER_DLOG("  readahead success @ chunk %u", it.chunk_id());
    PERFETTO_DCHECK(((*it).num_fragments == 1 &&
                     !((*it).flags & kLastPacketContinuesOnNextChunk)) ||
                    (*it).num_fragments > 1);

    // Now let's re-iterate over the [read_iter_, it] sequence and mark
    // all the fragments as read.
    bool packet_corruption = false;
    for (;;) {
      PERFETTO_DCHECK(read_iter_.is_valid());
      TRACE_BUFFER_DLOG("    commit chunk %u", read_iter_.chunk_id());
      if (PERFETTO_LIKELY((*read_iter_).num_fragments > 0)) {
        // In the unlikely case of a corrupted packet, invalidate the all
        // stitching and move on to the next chunk in the same sequence,
        // if any.
        packet_corruption |= !ReadNextPacketInChunk(&*read_iter_, packet);
      }
      if (read_iter_.cur == it.cur)
        break;
      read_iter_.MoveNext();
    }  // for(;;)
    PERFETTO_DCHECK(read_iter_.cur == it.cur);

    if (PERFETTO_UNLIKELY(packet_corruption)) {
      stats_.abi_violations++;
      PERFETTO_DCHECK(suppress_sanity_dchecks_for_testing_);
      *packet = TracePacket();  // clear.
      return ReadAheadResult::kFailedStayOnSameSequence;
    }

    return ReadAheadResult::kSucceededReturnSlices;
  }  // for(it...)  [readahead loop]
  return ReadAheadResult::kFailedMoveToNextSequence;
}

bool TraceBuffer::ReadNextPacketInChunk(ChunkMeta* chunk_meta,
                                        TracePacket* packet) {
  PERFETTO_DCHECK(chunk_meta->num_fragments_read < chunk_meta->num_fragments);
  PERFETTO_DCHECK(!(chunk_meta->flags & kChunkNeedsPatching));

  const uint8_t* record_begin =
      reinterpret_cast<const uint8_t*>(chunk_meta->chunk_record);
  const uint8_t* record_end = record_begin + chunk_meta->chunk_record->size;
  const uint8_t* packets_begin = record_begin + sizeof(ChunkRecord);
  const uint8_t* packet_begin = packets_begin + chunk_meta->cur_fragment_offset;

  if (PERFETTO_UNLIKELY(packet_begin < packets_begin ||
                        packet_begin >= record_end)) {
    // The producer has a bug or is malicious and did declare that the chunk
    // contains more packets beyond its boundaries.
    stats_.abi_violations++;
    PERFETTO_DCHECK(suppress_sanity_dchecks_for_testing_);
    return false;
  }

  // A packet (or a fragment) starts with a varint stating its size, followed
  // by its content. The varint shouldn't be larger than 4 bytes (just in case
  // the producer is using a redundant encoding)
  uint64_t packet_size = 0;
  const uint8_t* header_end =
      std::min(packet_begin + protozero::proto_utils::kMessageLengthFieldSize,
               record_end);
  const uint8_t* packet_data = protozero::proto_utils::ParseVarInt(
      packet_begin, header_end, &packet_size);

  const uint8_t* next_packet = packet_data + packet_size;
  if (PERFETTO_UNLIKELY(next_packet <= packet_begin ||
                        next_packet > record_end)) {
    stats_.abi_violations++;
    PERFETTO_DCHECK(suppress_sanity_dchecks_for_testing_);
    chunk_meta->cur_fragment_offset = 0;
    chunk_meta->num_fragments_read = chunk_meta->num_fragments;
    return false;
  }
  chunk_meta->cur_fragment_offset =
      static_cast<uint16_t>(next_packet - packets_begin);
  chunk_meta->num_fragments_read++;

  if (PERFETTO_UNLIKELY(packet_size == 0)) {
    stats_.abi_violations++;
    PERFETTO_DCHECK(suppress_sanity_dchecks_for_testing_);
    return false;
  }

  if (PERFETTO_LIKELY(packet))
    packet->AddSlice(packet_data, static_cast<size_t>(packet_size));

  return true;
}

}  // namespace perfetto
