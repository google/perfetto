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

#include "src/tracing/service/trace_buffer_v2.h"

#include <limits>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/base/intrusive_list.h"

#define TRACE_BUFFER_VERBOSE_LOGGING() 0  // Set to 1 when debugging unittests.
#if TRACE_BUFFER_VERBOSE_LOGGING()
#define TRACE_BUFFER_DLOG PERFETTO_DLOG
#else
#define TRACE_BUFFER_DLOG(...) base::ignore_result(__VA_ARGS__)
#endif

using protozero::proto_utils::ParseVarInt;
namespace proto_utils = ::protozero::proto_utils;

namespace perfetto {

namespace {
constexpr uint8_t kFirstPacketContinuesFromPrevChunk =
    SharedMemoryABI::ChunkHeader::kFirstPacketContinuesFromPrevChunk;
constexpr uint8_t kLastPacketContinuesOnNextChunk =
    SharedMemoryABI::ChunkHeader::kLastPacketContinuesOnNextChunk;
constexpr uint8_t kChunkNeedsPatching =
    SharedMemoryABI::ChunkHeader::kChunkNeedsPatching;
}  // namespace.

TraceBufferV2::BufIterator::BufIterator(TraceBufferV2* buf, size_t limit)
    : buf_(buf), limit_(limit) {
  TBChunk* chunk = buf->GetTBChunkAt(buf->wr_);
  // TODO either here or caller: mvoe all the way left.
  SetTargetChunkAndRewind(chunk);
  // TODO use the other methhod that rewinds here.
  // NextChunk();  TODO depends on the order of call in the for loop
}

void TraceBufferV2::BufIterator::SetTargetChunkAndRewind(TBChunk* chunk) {
  SetChunk(chunk);
  target_chunk_ = chunk;
  while (chunk->list_node.prev != TODO_list_head) {
    chunk = chunk->list_node.prev;
  }
  SetChunk(chunk);
}

// See comments around BufIterator in the .h file for the rationale.
bool TraceBufferV2::BufIterator::NextChunk() {
  const bool move_in_linked_list_order = chunk_ != target_chunk_;
  if (move_in_linked_list_order) {
    // Move to the next chunk in the linked list.
    // We must always be able to move next in the list because we reached this
    // state by initially walking the list backwards. If this fails, the list
    // is corrupted.
    PERFETTO_CHECK(NextChunkInSequence());
    return true;
  }

  // Otherwise Move to the next chunk in buffer order.
  return NextChunkInBuffer();
}

bool TraceBufferV2::BufIterator::NextChunkInSequence() {
  // Move to the next chunk in the linked list.
  // We must always be able to move next in the list because we reached this
  // state by initially walking the list backwards. If this fails, the list
  // is corrupted.
  if (chunk_->list_node.next == TODO_list_head)
    return false;
  SetChunk(chunk_->list_node.next);
  return true;
}

bool TraceBufferV2::BufIterator::NextChunkInBuffer() {
  PERFETTO_CHECK(target_chunk_);
  size_t off = buf_->OffsetOf(target_chunk_);
  off += target_chunk_->outer_size();
  if (limit_ > 0 && off >= limit_)  // TODO > vs >= ?
    return false;
  off = off >= buf_->size_ ? 0 : off;
  if (off == buf_->wr_)
    return false;  // We wrapped around and hit the write cursor.
  SetTargetChunkAndRewind(buf_->GetTBChunkAt(off));
  return true;
}

std::optional<TraceBufferV2::Frag>
TraceBufferV2::BufIterator::NextFragmentInChunk() {
  PERFETTO_DCHECK(next_frag_ >= chunk_->fragments_begin() &&
                  next_frag_ <= chunk_->fragments_end());
  uint8_t* chunk_end = chunk_->fragments_end();
  if (next_frag_ >= chunk_end)
    return std::nullopt;

  Frag frag{};

  bool is_first_frag = next_frag_ == chunk_->fragments_begin();
  uint64_t frag_size_and_flags = 0;
  frag.size_hdr = next_frag_;
  frag.begin = const_cast<uint8_t*>(
      ParseVarInt(next_frag_, chunk_end, &frag_size_and_flags));
  frag.size = static_cast<size_t>(frag_size_and_flags >> kFragShift);
  // We should never end up with an out-of-bound fragment in the buffer.
  // Even if a producer writes a malformed chunk, CopyChunkUntrusted is
  // supposed to discard that and never write it in the buffer.
  PERFETTO_CHECK(frag.size <= static_cast<size_t>(chunk_end - next_frag_));
  uint8_t frag_flags = frag_size_and_flags & kFragMask;
  next_frag_ = frag.begin + frag.size;
  bool is_last_frag = next_frag_ == chunk_end;
  bool first_frag_continues =
      chunk_->flags & kFirstPacketContinuesFromPrevChunk;
  bool last_frag_continues = chunk_->flags & kLastPacketContinuesOnNextChunk;

  if (!(frag_flags & kFragValid)) {
    frag.type = kFragInvalid;
  } else if (is_last_frag && last_frag_continues) {
    if (is_first_frag && first_frag_continues) {
      frag.type = kFragContinue;
    } else {
      frag.type = kFragBegin;
    }
  } else if (is_first_frag && first_frag_continues) {
    frag.type = kFragEnd;
  } else {
    frag.type = kFragWholePacket;
  }
  return frag;
}

bool TraceBufferV2::Initialize(size_t size) {
  size = base::AlignUp(std::max(size, size_t(1)), base::GetSysPageSize());
  // The size must be <= 4GB because we use 32 bit offsets everywhere (e.g. in
  // the TBChunk linked list) to reduce memory overhead.
  PERFETTO_CHECK(size <= UINT32_MAX);
  data_ = base::PagedMemory::Allocate(
      size, base::PagedMemory::kMayFail | base::PagedMemory::kDontCommit);
  if (!data_.IsValid()) {
    PERFETTO_ELOG("Trace buffer allocation failed (size: %zu)", size);
    return false;
  }
  size_ = size;
  wr_ = 0;
  // used_size_ = 0;
  // stats_.set_buffer_size(size);
  // last_chunk_id_written_.clear();
  // rd_iter_ = BufIterator(this);
  return true;
}

void TraceBufferV2::BeginRead(size_t limit) {
  // Start the read at the first chunk after the write cursor. However, if
  // due to out-of-order commits there is another chunk in the same sequence
  // prior to that (even if it's physically after in the buffer) start there
  // to respect sequence FIFO-ness.
  TRACE_BUFFER_DLOG("BeginRead()");
  rd_iter_ = BufIterator(this, limit);
}

bool TraceBufferV2::ReadNextTracePacket(
    TracePacket* out_packet,
    PacketSequenceProperties* sequence_properties,
    bool* previous_packet_on_sequence_dropped,
    bool force_erase) {
  TRACE_BUFFER_DLOG("ReadNextTracePacket()");

  // Just in case we forget to initialize these below.
  *sequence_properties = {0, ClientIdentity(), 0};
  *previous_packet_on_sequence_dropped = false;  // TODO this
  // TODO DCHECK changed_since_last_read_
  // TODO DNS check for sequence id gaps.

  // TODO remove chunk from list as we read fully.

  for (;;) {
    std::optional<Frag> maybe_frag = rd_iter_.NextFragmentInChunk();
    if (!maybe_frag.has_value()) {
      // We read all the fragments in the current chunk. Move to the next chunk.
      // NextChunk() moves "in the right direction", either in buffer order or
      // sequence order, depending on its internal state. If it returns false
      // we wrapped around the ring buffer and hit the wr_ pointer again.

      // TODO: erase the chunk +memoize the last chunk erased
      EraseTBChunk(rd_iter_.chunk());

      if (!rd_iter_.NextChunk())
        return false;  // There is nothing else to read in the buffer.
      continue;
    }
    Frag& frag = *maybe_frag;
    switch (frag.type) {
      case kFragInvalid:
        // This is either a whole packet that we consumed already, or a fragment
        // that has been invalidated because we had gaps while reassembling,
        // perhaps due to a data loss.
        continue;

      case kFragWholePacket:
        out_packet->AddSlice(frag.begin, frag.size);
        frag.MarkInvalid();
        // TODO when consuming all fragments in a chunk, we should also remove
        // the chunk altogether.
        return true;

      case kFragContinue:
      case kFragEnd:
        // We should never hit these cases while iterating in this outer loop.
        // In nominal conditions we should only see kFragBegin, and then we
        // should iterate over the kFrag{Continue/End} in the inner loop below
        // that performs the lookahead. If we find hit this code path, instead,
        // that's an ABI violation / buggy producer. This means that a producer
        // emitted a chunk that looks like [kWholePacket, kFragContinue] or
        // [kWholePacket, kFragEnd].
        // TODO report ABI violation
        // TODO maybe it's an ok error if we have data losses.
        frag.MarkInvalid();
        break;

      case kFragBegin: {
        // Try to reassemble the packet. This will require following the linked
        // list of chunks in the same sequence. 16 below is an educated guess,
        // if we go over it, SmallVector will expand on the heap.
        base::SmallVector<Frag, 16> packet_frags;
        packet_frags.emplace_back(frag);
        if (force_erase)
          frag.MarkInvalid();
        BufIterator it = rd_iter_;  // Copy the iterator to lookahead.
        for (bool reassembling = true; reassembling;) {
          if (!it.NextChunkInSequence()) {
            reassembling = false;
            break;
          }
          for (;;) {
            std::optional<Frag> frag2 = it.NextFragmentInChunk();
            if (!frag2.has_value()) {
              reassembling = false;
              break;
            }
            // TODO DNS check sequence IDs
            if (frag2->type == kFragContinue) {
              packet_frags.emplace_back(*frag2);
              if (force_erase)
                frag2->MarkInvalid();
            } else if (frag2->type == kFragEnd) {
              packet_frags.emplace_back(*frag2);
              // Success: we found all the fragments for the packet. Add them
              // to out_packet and invalidate them.
              for (Frag* f = packet_frags.begin(); f != packet_frags.end();
                   ++f) {
                out_packet->AddSlice(f->begin, f->size);
                f->MarkInvalid();  // TODO ditto here.
              }
              return true;
            } else {
              // TODO report ABI violations. DNS
            }
          }
        }
        // If we got here the lookahead failed. This is okay, we might have not
        // received yet all the fragments for the packet. We should just
        // move on onto the next chunk in buffer order, if there is any.
        if (force_erase) {
          // TODO here we should wipe all the chunks until we get back to the
          // target chunk, and then move next in buffer.
        }

        if (!rd_iter_.NextChunkInBuffer())
          return false;
        // Continue the iteration on the next chunk in buffer order.
      }  // case kFragBegin
      break;
    }  // switch(frag.type)
  }  // for (;;)
  return false;
}

void TraceBufferV2::CopyChunkUntrusted(
    ProducerID producer_id_trusted,
    const ClientIdentity& client_identity_trusted,
    WriterID writer_id,
    ChunkID chunk_id,
    uint16_t num_fragments,
    uint8_t chunk_flags,
    bool chunk_complete,
    const uint8_t* src,
    size_t size) {
  // Note: `src` points to the first packet fragment in the chunk. The caller
  // (TracingServiceImpl) does the chunk header decoding for us and breaks it
  // down into the various args passed here.
  // Each fragment is prefixed by a
  const uint8_t* cur = src;
  const uint8_t* const end = src + size;

  // If the chunk hasn't been completed, we should only consider the first
  // |num_fragments - 1| packets complete. For simplicity, we simply disregard
  // the last one when we copy the chunk.
  if (PERFETTO_UNLIKELY(!chunk_complete)) {
    if (num_fragments > 0) {
      num_fragments--;
      // These flags should only affect the last packet in the chunk. We clear
      // them, so that TraceBuffer is able to look at the remaining packets in
      // this chunk.
      chunk_flags &= ~kLastPacketContinuesOnNextChunk;
      chunk_flags &= ~kChunkNeedsPatching;
    }
  }

  struct FragInfo {
    uint32_t off;
    uint32_t size;
  };

  // Note: The 256 arg is only for the inline size before growing on heap. It's
  // an educated guess on how many packets we'll usually find.
  base::SmallVector<FragInfo, /* inline_size*/ 256> frags;
  size_t all_frags_size = 0;

  // Find the bounds of all the fragments and keep them in the `frags` vector.
  // Copy happens a bit later below. We first need to check for bound validity
  // and figure out the final size of the TBCHunk, which might be < the SMB
  // chunk (e.g. when doing a Flush() so that the payload is << chunk size).
  for (uint16_t frag_idx = 0; frag_idx < num_fragments; frag_idx++) {
    const bool is_last_frag = frag_idx == num_fragments - 1;

    // A fragment in the SMB starts with a varint stating its size.
    // The varint shouldn't be larger than 4 bytes, as the max size supported
    // by the SharedMemoryABI is kMaxMessageLength (256 MB).
    uint64_t frag_size_u64 = 0;
    // TODO update next pointer DNS
    const uint8_t* size_end =
        std::min(cur + proto_utils::kMessageLengthFieldSize, end);
    const uint8_t* payload_begin = ParseVarInt(cur, size_end, &frag_size_u64);
    const uint8_t* payload_end = payload_begin + frag_size_u64;

    // Because of the kMessageLengthFieldSize, the frag size must be at most
    // 256MB.
    PERFETTO_DCHECK(frag_size_u64 <= proto_utils::kMaxMessageLength);
    const uint32_t fragment_size = static_cast<uint32_t>(frag_size_u64);

    // In BufferExhaustedPolicy::kDrop mode, TraceWriter may abort a fragmented
    // packet by writing an invalid size in the last fragment's header. We
    // should handle this case without recording an ABI violation (since Android
    // R).
    if (PERFETTO_UNLIKELY(fragment_size ==
                          SharedMemoryABI::kPacketSizeDropPacket)) {
      // TODO DNS
      stats_.set_trace_writer_packet_loss(stats_.trace_writer_packet_loss() +
                                          1);
      PERFETTO_DCHECK(is_last_frag || suppress_client_dchecks_for_testing_);
      break;
    }

    if (PERFETTO_UNLIKELY(payload_end >= end || payload_end <= payload_begin)) {
      // Something is not right. Malicious producer or data corruption.
      // We will still do our best with copying over the previous valid
      // fragments, if any.
      stats_.set_abi_violations(stats_.abi_violations() + 1);
      PERFETTO_DCHECK(suppress_client_dchecks_for_testing_);
      break;
    }

    // We found a valid fragment.
    uint32_t off = static_cast<uint32_t>(payload_begin - src);
    frags.emplace_back(FragInfo{off, fragment_size});
    // Compute the length that will be needed to write the size&flags header.
    all_frags_size += proto_utils::VarintSize(fragment_size << kFragShift);
    all_frags_size += fragment_size;
  }  // for (fragments)

  if (frags.empty())
    return;

  // Make space in the buffer for the chunk we are about to copy.

  // TODO update stats

  // We deliberately store the non-rounded-up size in the chunk header.
  // This is so we can tell where the last fragment ends exactly. When moving
  // in the buffer, we use aligned_size() which does the rounding.
  size_t tbchunk_size = sizeof(TBChunk) + all_frags_size;
  size_t tbchunk_outer_size = TBChunk::OuterSize(tbchunk_size);

  if (PERFETTO_UNLIKELY(tbchunk_outer_size > size_)) {
    // The chunk is bigger than the buffer. Extremely rare, but it can
    // technically happen if the user has specified a 4KB buffer and the SMB
    // chunk is 16KB.
    stats_.set_abi_violations(stats_.abi_violations() + 1);
    PERFETTO_DCHECK(suppress_client_dchecks_for_testing_);
    return;
  }

  // If there isn't enough room from the given write position: write a padding
  // record to clear the end of the buffer, wrap and start at offset 0.
  const size_t cached_size_to_end = size_to_end();
  if (PERFETTO_UNLIKELY(tbchunk_outer_size > cached_size_to_end)) {
    ssize_t res = DeleteNextChunksFor(cached_size_to_end);
    if (res == -1)
      return DiscardWrite();
    wr_ = 0;
    stats_.set_write_wrap_count(stats_.write_wrap_count() + 1);
    PERFETTO_DCHECK(size_to_end() >= tbchunk_outer_size);
  }

  // Deletes all chunks from |wptr_| to |wptr_| + |record_size|.
  ssize_t del_res = DeleteNextChunksFor(tbchunk_outer_size);
  if (del_res == -1)
    return DiscardWrite();

  TBChunk* tbchunk = GetTBChunkAt(wr_);
  new (tbchunk) TBChunk(tbchunk_size);
  tbchunk->chunk_id = chunk_id;
  tbchunk->flags = chunk_flags;
  auto* payload_begin = reinterpret_cast<uint8_t*>(tbchunk) + sizeof(TBChunk);
  uint8_t* wptr = payload_begin;

  // Copy all the (valid) fragments from the SMB chunk to the TBChunk.
  for (FragInfo* frag = frags.begin(); frag != frags.end(); ++frag) {
    uint32_t frag_size_and_flags = frag->size << kFragShift | kFragValid;
    wptr = proto_utils::WriteVarInt(frag_size_and_flags, wptr);
    const size_t frag_size = frag->size;
    memcpy(wptr, &src[frag->off], frag_size);
    wptr += frag_size;
  }
  PERFETTO_DCHECK(static_cast<size_t>(wptr - payload_begin) == all_frags_size);

  // TODO chunk flags.

  auto seq_key = MkProducerAndWriterID(producer_id_trusted, writer_id);

  auto it = sequences_.find(seq_key);
  if (it == sequences_.end()) {
    auto it_and_inserted = sequences_.emplace(
        seq_key,
        SequenceState{producer_id_trusted, writer_id,
                      client_identity_trusted});  // TODO last_chunk_id DNS
    it = it_and_inserted.first;
  }

  // TODO handle OOO DNS.
  // TODO handle re-commit of same chunk id.
  it->second.tbchunks.PushBack(*tbchunk);

  stats_.set_chunks_written(stats_.chunks_written() + 1);
  stats_.set_bytes_written(stats_.bytes_written() + tbchunk_outer_size);
  // TODO DNS remember about FlatHashMap tombestones issue.
  // TODO more to do here, look at v1.
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// Note for reviewer: unlike the old v1 impl, here DeleteNextChunksFor also
// takes care of writing the padding chunk in case of truncation.
// TODO copy over the diagram from v1::CopyChunkUntrusted.
ssize_t TraceBufferV2::DeleteNextChunksFor(size_t bytes_to_clear) {
  PERFETTO_DCHECK(bytes_to_clear > sizeof(TBChunk));
  PERFETTO_DCHECK((bytes_to_clear % alignof(TBChunk)) == 0);
  // TODO apply logic that returns -1 if discard mode.
  // PERFETTO_CHECK(!discard_writes_);
  DcheckIsAlignedAndWithinBounds(wr_);
  const size_t clear_end = wr_ + bytes_to_clear;
  TRACE_BUFFER_DLOG("Delete [%zu %zu]", wr_, clear_end);
  PERFETTO_DCHECK(clear_end <= size_);
  // uint64_t chunks_overwritten = stats_.chunks_overwritten();
  // uint64_t bytes_overwritten = stats_.bytes_overwritten();
  uint64_t padding_bytes_cleared = stats_.padding_bytes_cleared();

  BeginRead(/*limit=*/bytes_to_clear);
  for (;;) {
    TracePacket packet;
    PacketSequenceProperties seq_props{};
    bool dropped;  // TODO maybe
    bool destructive_read = true;
    bool res =
        ReadNextTracePacket(&packet, &seq_props, &dropped, destructive_read);
    if (res)
      break;
  }

  // When we set a limit in BeginRead,ReadNextTracePacket must stop precisely
  // in the chunk that contains the limit (bytes_to_clear).
  // Now we need to take this last chunk and create a padding chunk precisely
  // on the `bytes_to_clear` boundary. This is so that the buffer remains well
  // formed, with a contiguous series of chunks.
  // Visually:
  //
  // Situation before:
  //           | wr_ is here initially
  //           V
  // +---------+----------+-----------+---------+
  // | xxxxxxx |  Chunk1  |  Chunk 2  | Chunk 3 |
  // +---------+----------+-----------+---------+
  //           |                  |
  //           +- bytes_to_clear -+
  //
  // Situation after:
  //                                + This new zero chunk is what we are after!
  //                                V
  // +---------+----------+-------+---+---------+
  // | xxxxxxx |  0000000 | 00000 | 0 | Chunk 3 |
  // +---------+----------+-------+---+---------+
  //           |                  |
  //           +- bytes_to_clear -+
  //

  TBChunk* last_chunk_cleared = rd_iter_.chunk();
  size_t chunk_start = OffsetOf(last_chunk_cleared);
  size_t chunk_end = chunk_start + last_chunk_cleared->outer_size();
  PERFETTO_CHECK(clear_end >= chunk_start && clear_end < chunk_end);

  // TODO erase last_chunk_cleared? or did the read do that?

  TBChunk* pad_chunk = GetTBChunkAtUnchecked(clear_end);
  new (pad_chunk) TBChunk(chunk_end - clear_end);
  // TODO below
  // stats_.set_padding_bytes_cleared(padding_bytes_cleared);

  ////////
  // DELETE BELOW
  ////

  size_t next;
  for (size_t cur = wr_; cur < clear_end; cur = next) {
    TBChunk* tbchunk = GetTBChunkAt(cur);
    next = cur + tbchunk->size;  // GetTBChunkAt() CHECKs that .size > 0.
    TRACE_BUFFER_DLOG("  scanning chunk [%zu %zu]", cur, next);
    if (PERFETTO_UNLIKELY(tbchunk->is_padding())) {
      padding_bytes_cleared += tbchunk->size;
      continue;
      // TODO update the padding header.
    }
    // Remove the TBChunk.

    EraseTBChunk(tbchunk);
    tbchunk = nullptr;
    // // Follow the linked list backwards until we get to the list head, which
    // // will get us to the SequenceState. Given we are removing chunks FIFO,
    // we
    // // expect that in most cases the walk backwards will resolve within one
    // hop. SequenceState& seq = GetSequenceStateForTBChunk(tbchunk);
    // ++chunks_overwritten;
    // bytes_overwritten += tbchunk.size;
    // seq.tbchunks.Erase(tbchunk);
    // PERFETTO_DCHECK(tbchunk.is_padding());  // Erase clears the prev/next
    // ptrs. tbchunk.chunk_id = 0;

    // We have two cases here:
    // 1. Full deletion, when clear_end >= the end of the cur chunk.
    // 2. Partial deletion, when clear_end falls inside the cur chunk.
    //    In this case we need to write a padding chunk after the end of the
    //    deletion boundary, to make sure that the next chunk is still readable.

    if (clear_end >= next)
      continue;  // Case 1

    // Case 2.
    PERFETTO_DCHECK(next - clear_end >= sizeof(TBChunk));
    PERFETTO_DCHECK(((next - clear_end) % sizeof(TBChunk)) == 0);
    TBChunk* pad_chunk = GetTBChunkAtUnchecked(clear_end);
    new (pad_chunk) TBChunk(next - clear_end);
  }

  // stats_.set_chunks_overwritten(chunks_overwritten);
  // stats_.set_bytes_overwritten(bytes_overwritten);
  stats_.set_padding_bytes_cleared(padding_bytes_cleared);
}

// TODO the caller of this must walk back to the chain.
void TraceBufferV2::EraseTBChunk(TBChunk* tbchunk) {
  // Follow the linked list backwards until we get to the list head, which
  // will get us to the SequenceState. Given we are removing chunks FIFO, we
  // expect that in most cases the walk backwards will resolve within one hop.
  SequenceState& seq = GetSequenceStateForTBChunk(tbchunk);
  const uint32_t size = tbchunk->size;

  // This must be the first chunk of the sequence.
  PERFETTO_DCHECK(tbchunk->list_node.prev == seq.tbchunks);

  IterateTBCHunkFragments(
      tbchunk, [](uint8_t* frag, size_t frag_size, uint8_t flags) {
        if (!(flags & kFragValid)) {
          return;  // The fragment has been consumed or invalidated.
        }
        // We should never end up in a state where we have a partial fragment
        // left hanging around.
        PERFETTO_DCHECK(!(flag & kFragContinuesFromPrev));

        if (flag & kFragContinuesOnNext) {
          ... PassToVM(scatterlist, ...);
          return;
        }

        PassToVM(frag, ...);
      });

  seq.tbchunks.Erase(*tbchunk);
  PERFETTO_DCHECK(tbchunk->is_padding());  // Erase clears the prev/next ptrs.
  tbchunk->chunk_id = 0;

  // TODO handle compaction by remembering the last erased chunk.

  stats_.set_chunks_overwritten(stats_.chunks_overwritten() + 1);
  stats_.set_bytes_overwritten(stats_.bytes_overwritten() + size);
}

/*
  read case:
    - Get chunk after wr
    - Go all the way left
    - Iterate through fragments
    - When passing the current chunk it's complicated
      - Go next only to complete the current fragment
      - Otherwise continue with next chunk in sequence
    - Append to local list
    - TODO pass to vm?
    - NOTE: They can be discarded only after TSImpl calls ReadNextPacket...

  overwrite case:
    - Get chunk @ x (after wr_ realistically)
    - go all the way left (but keep until this chunk is gone)
    - Consume fragments & pass to VM

  There is a big difference though: when I overwrite the chunk is lost
  anyways. WhenI read back, if the last fragment can't be read, I should leave
  it where it is and try again later.

*/
// This function reads and consumes fragments for
// - All chunks that precede the passed chunk (if any, rare)
// - All fragments in the passed chunk
// - Continuation fragment in chunks that follow the passed chunk, up for at
//   most one TracePacket.
// Returns a vector of TracePacket (possibly sliced, but guaranteed to be
// complete). The caller must consume the packets before invoking any other
// method, as future invocations of ReadBuffers of CopyChunk will overwrite
// that data. Note: this function can consume other chunks beyond
// target_chunk, if target_chunk contains a fragmented packet that continues
// in other chunks and we happen to have those chunks already.
// TODO maybe we don't need the input arg as it's always the one @ wr_?
// TODO in order to consume this needs to:
// - remove from linked list.
// - mark fragments as invalid.git
void TraceBufferV2::TODONameReadAndConsume(TBChunk* target_chunk) {
  // Due to out-of-order commits, this chunk might have other unconsumed
  // chunks before it, that must be consumed in order before getting to this.
  // In practice this is extremely rare. OOO commits happen only when scraping
  // and have usually a limited extent (one extra chunk committed out of
  // order).

  // Find the left-most chunk in the per-sequence list.
  TBChunk* cur_chunk = target_cunk;
  for (; cur_chunk->list_node.prev != TODO_sequencestate;
       cur_chunk = cur_chunk->list_node.prev) {
  }

  // Contains only full TracePackets that are found while scanning the chunks.
  std::vector<TracePacket> out_packets;

  // Contains the (partial) packet we are trying to reassemble. Once fully
  // reassembled it is moved into out_packets.
  std::optional<TracePacket> reassembling_packet;

  // IterateChunksAndFragments iterates over all fragments starting from the
  // passed chunk, and continuing on other chunks in the same sequence
  // (following the linked list) until:
  // - It exhausts chunks for the sequence, reaching the list head.
  // - The lambda returns false.
  // TODO DNS does it worry about chunk sequence numbers or not? prob not and
  // we should do it here.
  bool has_processed_target_chunk = false;
  IterateChunksAndFragments(
      cur_chunk,
      [&](TBChunk* chunk,              //
          uint8_t* frag_start,         //
          size_t frag_size,            //
          FragType frag_type,          //
          bool is_last_frag_of_chunk,  //
          ) ->                         //
      bool {                           //
        if (chunk == target_chunk)
          has_processed_target_chunk = true;

        // In any case, mark the fragment as invalid, as we are going to
        // consume it no matter what.
        // TODO what if frag_size == 0? think DNS
        // TODO DNS this is NOT correct in the case of readback. we are NOT
        // going to "consume no matter what" in that case.
        *frag_start |= 0x1;  // TODO DNS kFragInvalid

        bool fragmentation_error = false;
        switch (frag_type) {
          case kFragInvalid:
            break;

          case kFragWholePacket:
            if (reassembling_packet) {
              fragmentation_error = true;
              reassembling_packet.reset();
            }
            out_packets.emplace_back();
            out_packets.back().AddSlice(frag_payload, frag_size);
            break;

          case kFragBegin:
            if (reassembling_packet) {
              // This can happen if we seeing twice in a row a chunk with
              // "last packet continues on next chunk" without the
              // "continues from prev" bit.
              fragmentation_error = true;
            }
            reassembling_packet.emplace();
            reassembling_packet->AddSlice(frag_payload, frag_size);
            break;

          case kFragContinue:
          case kFragEnd:
            if (!reassembling_packet) {
              fragmentation_error = true;
              break;
            }
            reassembling_packet->AddSlice(frag_payload, frag_size);
            if (frag_type == kFragEnd) {
              out_packets.emplace_back(std::move(*reassembling_packet));
              reassembling_packet.reset();
            }
            break;
        }

        if (fragmentation_error) {
          TRACE_BUFFER_DLOG("Fragmentation error on chunk %" PRIu32
                            " frag_type:%d, frag_size:%zu",
                            chunk->chunk_id, frag_type, frag_size);
          stats_.set_abi_violations(stats_.abi_violations() + 1);
        }

        // Now we need to decide whether we should continue iterating or not.

        if (!has_processed_target_chunk) {
          // We had to scroll back to previous chunks and have not reached yet
          // the Chunk we are meaning to consume. Keep going.
          return true;
        }

        if (chunk == target_chunk) {
          if (!is_last_frag_of_chunk) {
            // There are more fragments in the chunk, keep going.
            return true;
          }
          // If this is the last fragment, stop here unless the last fragment
          // is a continuation and we are trying to reassemble it.
          // (Technically this return isn't required as the statement below is
          // i
          // dentical, but it makes the code a bit easier to read)
          return reassembling_packet != nullptr;
        }

        // If we get here, we adventured ourselves beyond target_chunk. Keep
        // as long as we are reassembling packets.
        return reassembling_packet != nullptr;
      });

  if (reassembling_packet) {
    // If we get here it means that our attempt to reassemble the last packet
    // failed (e.g. not all fragments were there). In this case remove it
    // from the list.
  }

  //  ---------------------------------------
  // TODO check the chunk id with the sequence metadata to detect data loss?

  // Now iterate through the sublist [cur_chunk, target_chunk] in order.
  TBChunk* end_chunk = target_chunk->list_node.next;

  // This lambda groups the logic for the middle term of the for loop below
  // for readabilituy.
  auto iterate_next = [&]() -> bool {
    if (!reassembling_packet) {
      // If we are not reassembling packets, stop precisely once we reach the
      // chunk that follows target_chunk (which might be the list head if
      // target_chunk was the last in the list).
      return cur_chunk != end_chunk;
    } else {
      // If we are reassembling packets, instead, keep going until we reach
      // the list head.
      return cur_chunk != TODO_sequencestate.
    }
  };

  bool beyond_target_chunk = false;
  for (; iterate_next(); cur_chunk = cur_chunk->list_node.next) {
    beyond_target_chunk |= cur_chunk == end_chunk;
    if (cur_chunk->is_padding())
      continue;

    uint8_t* cur = cur_chunk->fragments_begin();
    uint8_t* const chunk_end = cur_chunk->fragments_end();
    while (cur < chunk_end) {
      uint64_t frag_size_and_flags = 0;
      uint8_t* frag_payload = ParseVarInt(cur, chunk_end, &frag_size_and_flags);
      uint32_t frag_size =
          static_cast<uint32_t>(frag_size_and_flags >> kFragShift);
      uint8_t frags_flags = frag_size_and_flags & kFragFlagsMask;
      bool is_valid = frags_flags & 1;  // TODO add constant.
      // We should never end up with an out-of-bound fragment in the buffer.
      // Even if a producer writes a malformed chunk, CopyChunkUntrusted is
      // supposed to discard that and never write it in the buffer.
      PERFETTO_CHECK(frag_size <= static_cast<size_t>(chunk_end - cur));
      uint8_t* frag_payload_end = frag_payload + frag_size;
      cur = frag_payload_end;

      // TODO who checks holes in chunk id?

      if (continues_from_prev_chunk) {
        if (reassembling_packet) {
          reassembling_packet->AddSlice(frag_payload, frag_size);
        }
        if (!continues_on_next_chunk) {
          // Success, we got to the last fragment.
          // TODO who removes this if not?
          reassembling_packet = nullptr;
          // TODO quit the loop if we were going beyond target_chunk due to
          // this.
          if (beyond_target_chunk) {
            break;
          }
        }
        // TODO we should never end up in this state I think? or not? think
        // TODO mark frag as consumed/invalid.
        continue;
      }
      if (continues_on_next_chunk) {
        if (reassembling_packet) {
          // Something has gone wrong (producer screwed up).
          // TODO do somthing DNS
        }
        out_packets.emplace_back();
        reassembling_packet = &out_packets.back();
        reassembling_packet->AddSlice(frag_payload, frag_size);
        continue
      }

      // Here both continue_prev and continue_next are false.
      // This is a self-contained TracPacket.
      if (reassembling_packet) {
        // As above, something went wrong. TODO DNS.
        // Maybe factor in that code together
      }
      out_packets.emplace_back();
      out_packets.back().AddSlice(frag_payload, frag_size)
    }  // For fragment in cur_chunk
  }  // For chunk in [cur_chunk, target_chunk]

  if (reassembling_packet) {
    // TODO do something here.
  }
}

}  // namespace perfetto
