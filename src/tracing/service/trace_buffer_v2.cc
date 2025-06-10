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
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/shared_memory_abi.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/base/intrusive_list.h"

// TODO do a pass for uint32_t /uint16_t vs size_t etc etc.

#define TRACE_BUFFER_VERBOSE_LOGGING() 1  // Set to 1 when debugging unittests.
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

// Compares two ChunkID(s) in a wrapping 32-bit ID space.
// Returns:
//   -1 if a comes before b in modular (wrapping) order.
//    0 if a == b
//   +1 if a comes after b in modular order.
//
// The key idea is that the order between two distinct IDs is determined by
// whether the distance from a to b is less than 2^31 (half the range).
// Many other TCP/IP stacks do the same, e.g.
// https://github.com/openbsd/src/blob/master/sys/netinet/tcp_seq.h#L43
int ChunkIdCompare(ChunkID a, ChunkID b) {
  if (a == b)
    return 0;
  return (static_cast<int32_t>(a - b) < 0) ? -1 : 1;
}
}  // namespace.

namespace internal {

SequenceState::SequenceState(ProducerID p, WriterID w, ClientIdentity c)
    : producer_id(p), writer_id(w), client_identity(c) {}
SequenceState::~SequenceState() = default;
SequenceState::SequenceState(const SequenceState&) noexcept = default;
SequenceState& SequenceState::operator=(const SequenceState&) noexcept =
    default;

// +---------------------------------------------------------------------------+
// | BufIterator implementation                                                |
// +---------------------------------------------------------------------------+

BufIterator::BufIterator() : buf_(nullptr) {}

BufIterator::BufIterator(TraceBufferV2* buf, size_t limit)
    : buf_(buf), limit_(limit) {
  TBChunk* chunk = buf->GetTBChunkAt(buf->wr_);
  SetTargetChunkAndRewind(chunk);
}

bool BufIterator::SetTargetChunkAndRewind(TBChunk* chunk) {
  target_chunk_ = chunk;
  bool res = false;
  SequenceState* seq = buf_->GetSeqForChunk(chunk);
  seq_ = seq;
  if (seq) {
    res = seq_->read_pass == buf_->read_pass_;
    seq_->read_pass = buf_->read_pass_;

    size_t chunk_off = buf_->OffsetOf(chunk);
    auto& chunk_list = seq->chunks;
    // Ensure we have the current chunk in the list.
    PERFETTO_CHECK(!chunk_list.empty());
    PERFETTO_DCHECK(chunk_list.Find(chunk_off) != chunk_list.end());
    chunk = buf_->GetTBChunkAt(*chunk_list.begin());
  } else {
    // The only case when GetSeqForChunk() can legitimately return null is when
    // we are trying to iterate over a cleared padding chunk.
    PERFETTO_DCHECK(chunk->is_padding());
  }
  SetChunk(chunk);
  return res;
}

// See comments around BufIterator in the .h file for the rationale.
bool BufIterator::NextChunk() {
  const bool move_in_seq_order = chunk_ != target_chunk_;
  if (move_in_seq_order) {
    // Move to the next chunk in the linked list.
    // We must always be able to move next in the list because we reached this
    // state by initially rewining to the beginning of the list.
    // If this fails, the `SequenceState.chunks` list is corrupted.
    PERFETTO_CHECK(NextChunkInSequence());
    return true;
  }

  // Otherwise Move to the next chunk in buffer order.
  return NextChunkInBuffer();
}

bool BufIterator::NextChunkInSequence() {
  if (!seq_)
    return false;
  size_t cur_chunk_of = buf_->OffsetOf(chunk_);
  auto& chunk_list = seq_->chunks;
  // TODO this could be optimized to remove the find.
  auto it = chunk_list.Find(cur_chunk_of);
  PERFETTO_DCHECK(it != chunk_list.end());
  if (++it == chunk_list.end())
    return false;
  SetChunk(buf_->GetTBChunkAt(*it));
  // No need to update `seq_` as we are staying on the same sequence.
  return true;
}

bool BufIterator::NextChunkInBuffer() {
  for (;;) {
    PERFETTO_CHECK(target_chunk_);
    size_t off = buf_->OffsetOf(target_chunk_);
    off += target_chunk_->outer_size();
    if (limit_ > 0 && off >= limit_)  // TODO > vs >= ?
      return false;
    off = off >= buf_->used_size_ ? 0 : off;  // TODO used_size_ is brittle !!!!!!!!!!!!!!!!!!
    if (off == buf_->wr_)
      return false;  // We wrapped around and hit the write cursor.
    // SetTargetChunkAndRewind will update `seq_`.
    bool already_visited = SetTargetChunkAndRewind(buf_->GetTBChunkAt(off));
    if (!already_visited)
      break;
  }
  return true;
}

std::optional<TraceBufferV2::Frag> BufIterator::NextFragmentInChunk() {
  PERFETTO_DCHECK(next_frag_off_ <= chunk_->payload_size);
  uint8_t* chunk_end = chunk_->fragments_end();
  uint8_t* hdr_begin = chunk_->fragments_begin() + next_frag_off_;
  if (hdr_begin >= chunk_end)
    return std::nullopt;

  Frag frag{};
  bool is_first_frag = next_frag_off_ == 0;
  uint64_t frag_size_u64 = 0;

  uint8_t* frag_begin =
      const_cast<uint8_t*>(ParseVarInt(hdr_begin, chunk_end, &frag_size_u64));
  size_t hdr_size =
      static_cast<size_t>(reinterpret_cast<uintptr_t>(frag_begin) -
                          reinterpret_cast<uintptr_t>(hdr_begin));

  frag.chunk = chunk_;
  frag.payload_off = static_cast<uint16_t>(
      reinterpret_cast<uintptr_t>(frag_begin) -
      reinterpret_cast<uintptr_t>(chunk_->fragments_begin()));
  frag.size = static_cast<size_t>(frag_size_u64);

  // TODO soften this check. it can happen if the producer is really malicious
  // and changes them after we bound check.
  PERFETTO_CHECK(frag.size <= static_cast<size_t>(chunk_end - frag_begin));
  next_frag_off_ += hdr_size + frag.size;
  bool is_last_frag = next_frag_off_ >= chunk_->payload_size;
  bool first_frag_continues =
      chunk_->flags & kFirstPacketContinuesFromPrevChunk;
  bool last_frag_continues = chunk_->flags & kLastPacketContinuesOnNextChunk;

  if (is_last_frag && last_frag_continues) {
    if (is_first_frag && first_frag_continues) {
      frag.type = Frag::kFragContinue;
    } else {
      frag.type = Frag::kFragBegin;
    }
  } else if (is_first_frag && first_frag_continues) {
    frag.type = Frag::kFragEnd;
  } else {
    frag.type = Frag::kFragWholePacket;
  }
  return frag;
}

}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBuffer implementation                                                |
// +---------------------------------------------------------------------------+

// static
std::unique_ptr<TraceBufferV2> TraceBufferV2::Create(size_t size_in_bytes,
                                                     OverwritePolicy pol) {
  std::unique_ptr<TraceBufferV2> trace_buffer(new TraceBufferV2(pol));
  if (!trace_buffer->Initialize(size_in_bytes))
    return nullptr;
  return trace_buffer;
}

TraceBufferV2::TraceBufferV2(OverwritePolicy pol) : overwrite_policy_(pol) {}

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
  used_size_ = 0;
  stats_.set_buffer_size(size);
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
  ++read_pass_;
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

      // We are going to erase the current chunk, so move next first as the
      // erase will invalidate the next/prev pointers.
      TBChunk* cur_chunk = rd_iter_.chunk();
      SequenceState* seq = rd_iter_.sequence_state();
      bool has_next_chunk = rd_iter_.NextChunk();
      EraseTBChunk(cur_chunk, seq);
      if (!has_next_chunk)
        return false;  // There is nothing else to read in the buffer.
      continue;
    }
    Frag& frag = *maybe_frag;
    switch (frag.type) {
      case Frag::kFragWholePacket:
        out_packet->AddSlice(frag.begin(), frag.size);
        rd_iter_.Consume();
        return true;

      case Frag::kFragContinue:
      case Frag::kFragEnd:
        // We should never hit these cases while iterating in this outer loop.
        // In nominal conditions we should only see kFragBegin, and then we
        // should iterate over the kFrag{Continue/End} in the inner loop below
        // that performs the lookahead. If we hit this code path, instead, a
        // producer emitted a chunk that looks like
        // [kWholePacket, kFragContinue] or [kWholePacket, kFragEnd].
        // TODO report ABI violation
        // TODO maybe it's an ok error if we have data losses.
        rd_iter_.Consume();
        break;

      case Frag::kFragBegin:
        auto reassembly_res =
            ReassembleFragmentedPacket(out_packet, &frag, force_erase);
        if (reassembly_res == FragReassemblyResult::kSuccess) {
          // We found and consumed all the fragments for the packet.
          // On the next ReadNextTracePacket() call, NextFragmentInChunk() will
          // return nullopt (because, modulo client bugs, the kFragBegin here
          // is the alst fragment of the chunk). That code branch above will
          // erase the chunk and continue with the next chunk (either in buffer
          // or sequence order).
          return true;
        }
        if (reassembly_res == FragReassemblyResult::kDataLoss) {
          // If we detect a data loss, just mark all fragments processed as
          // invalid, so they don't trigger further error stats when we
          // iterate over the next chunks (Eventually we'll stumbled upon them
          // as we move in either buffer or sequence order).
          // The break will continue with the next fragments leaving the
          // chunk iteration unaltered. Imagine this:
          // - We start the read iteration
          // - On the first chunk we find there are prior in-sequence chunks
          //   so we rewind (we go back on the list, but go forward in buffer
          //   order)
          // - Then we find here that a fragmented packet is broken due to some
          //   data loss.
          // There is no point skipping the sequence as a data loss is forever.
          // We should keep going as if the data was invalid.
          break;
        }
        PERFETTO_DCHECK(reassembly_res == FragReassemblyResult::kNotEnoughData);
        // In this case we need two different behaviors:
        // 1. If we are doing a pure readback (force_erase = false), we should
        //    move away from this sequence non-destructively, as there is a
        //    chance that the missing chunks will appear in future.
        //    Note that by moving to NextChunkInBuffer we might still stumble
        //    on futher chunks of the current sequence. But the read_pass_
        //    generation counter will cause BufIterator to skip over them.
        // 2. We are overwriting chunks as part of a write: this was the last
        //    chance to read back the data (and pass it to protovm). We should
        //    destroy it and essentially treat it as a kDataLoss.
        if (!force_erase) {
          // Case 1: Continue the iteration on the next chunk in buffer order.
          if (!rd_iter_.NextChunkInBuffer())
            return false;
        }
        // In case 2: ReassembleFragmentedPacket has invalidated the fragments,
        // we should break the switch and continue with whatever is next.
        break;  // case kFragBegin
    }  // switch(frag.type)
  }  // for (;;)
  return false;
}

TraceBufferV2::FragReassemblyResult TraceBufferV2::ReassembleFragmentedPacket(
    TracePacket* out_packet,
    Frag* initial_frag,
    bool force_erase) {
  PERFETTO_DCHECK(initial_frag->type == Frag::kFragBegin);

  base::SmallVector<Frag, 16> frags;
  frags.emplace_back(*initial_frag);
  BufIterator it = rd_iter_;  // Copy the iterator to lookahead.

  FragReassemblyResult res;
  // Iterate over chunks using the linked list.
  for (;;) {
    if (!it.NextChunkInSequence()) {
      res = FragReassemblyResult::kNotEnoughData;
      break;
    }
    // TODO DNS check sequence IDs
    // TODO explain why we don't iterate over fragments but only read one.
    std::optional<Frag> frag = it.NextFragmentInChunk();
    if (!frag.has_value()) {
      // This can happen if the chunk needs patching (and is still unpatched).
      // In that case the BufIterator will bail out with nullopt.
      res = FragReassemblyResult::kNotEnoughData;
      break;
    }

    const auto& frag_type = frag->type;
    if (frag_type == Frag::kFragContinue) {
      frags.emplace_back(*frag);
      continue;
    }
    if (frag_type == Frag::kFragEnd) {
      frags.emplace_back(*frag);
      res = FragReassemblyResult::kSuccess;
      break;
    }
    // else: kFragBegin or kFragWholePacket

    // Even if force_consume is true, we want to leave frags these untouched
    // as they don't belog to us. The next ReadNextPacket calls will deal with
    // them. Our job here is to consume (forcefully or not) only fragments for
    // the packet we are trying to reassemble.
    // TODO report possible ABI violations? DNS
    // TODO record the data loss somewhere in SeqState?
    res = FragReassemblyResult::kDataLoss;
    break;
  }  // for (chunk in list)

  for (Frag& f : frags) {
    if (res == FragReassemblyResult::kSuccess) {
      out_packet->AddSlice(f.begin(), f.size);
    }
    if (force_erase || res == FragReassemblyResult::kSuccess ||
        res == FragReassemblyResult::kDataLoss) {
      // This is the equivalent to the Consume() call on the iterator. We cannot
      // use that as we can tell only at the end (after we have advanced the
      // iterator) whether we will consume or not.
      // TODO nobody seems to look at this?
      f.chunk->payload_consumed =
          std::max(f.chunk->payload_consumed, f.end_off());
    }
  }
  return res;
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
  // |num_fragments - 1| packets. For simplicity, we simply disregard
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

  // Compute the SUM(frags.size).
  size_t all_frags_size = 0;
  for (uint16_t frag_idx = 0; frag_idx < num_fragments; frag_idx++) {
    const bool is_last_frag = frag_idx == num_fragments - 1;

    // A fragment in the SMB starts with a varint stating its size.
    // The varint shouldn't be larger than 4 bytes, as the max size supported
    // by the SharedMemoryABI is kMaxMessageLength (256 MB).
    uint64_t frag_size_u64 = 0;
    const uint8_t* size_begin = cur;
    const uint8_t* size_limit =
        std::min(size_begin + proto_utils::kMessageLengthFieldSize, end);
    const uint8_t* payload_begin =
        ParseVarInt(size_begin, size_limit, &frag_size_u64);
    const uint8_t* payload_end = payload_begin + frag_size_u64;
    cur = payload_end;

    // Because of the kMessageLengthFieldSize, the frag size must be at most
    // 256MB.
    PERFETTO_DCHECK(frag_size_u64 <= proto_utils::kMaxMessageLength);
    const uint32_t frag_size = static_cast<uint32_t>(frag_size_u64);

    // In BufferExhaustedPolicy::kDrop mode, TraceWriter may abort a fragmented
    // packet by writing an invalid size in the last fragment's header. We
    // should handle this case without recording an ABI violation (since Android
    // R).
    if (PERFETTO_UNLIKELY(frag_size ==
                          SharedMemoryABI::kPacketSizeDropPacket)) {
      // TODO DNS
      stats_.set_trace_writer_packet_loss(stats_.trace_writer_packet_loss() +
                                          1);
      PERFETTO_DCHECK(is_last_frag || suppress_client_dchecks_for_testing_);
      break;
    }

    // TODO improve pointer arith without using ptrs.
    if (PERFETTO_UNLIKELY(payload_end > end || payload_end <= payload_begin)) {
      // Something is not right. Malicious producer or data corruption.
      // We will still do our best with copying over the previous valid
      // fragments, if any.
      stats_.set_abi_violations(stats_.abi_violations() + 1);
      PERFETTO_DCHECK(suppress_client_dchecks_for_testing_);
      break;
    }

    // We found a valid fragment.
    uint32_t frag_size_incl_hdr =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(payload_end) -
                              reinterpret_cast<uintptr_t>(size_begin));
    all_frags_size += frag_size_incl_hdr;
  }  // for (fragments)
  PERFETTO_CHECK(all_frags_size <= size);

  // Make space in the buffer for the chunk we are about to copy.

  // TODO update stats

  // We deliberately store the non-rounded-up size in the chunk header.
  // This is so we can tell where the last fragment ends exactly. When moving
  // in the buffer, we use aligned_size() which does the rounding.
  // tbchunk_outer_size := header + all_frags_size + alignment.
  size_t tbchunk_outer_size = TBChunk::OuterSize(all_frags_size);

  if (PERFETTO_UNLIKELY(tbchunk_outer_size > size_)) {
    // The chunk is bigger than the buffer. Extremely rare, but it can
    // technically happen if the user has specified a 16KB buffer and the SMB
    // chunk is 32KB.
    stats_.set_abi_violations(stats_.abi_violations() + 1);
    PERFETTO_DCHECK(suppress_client_dchecks_for_testing_);
    return;
  }

  // If there isn't enough room from the given write position: write a padding
  // record to clear the end of the buffer, wrap and start at offset 0.
  const size_t cached_size_to_end = size_to_end();
  if (PERFETTO_UNLIKELY(tbchunk_outer_size > cached_size_to_end)) {
    if (!DeleteNextChunksFor(cached_size_to_end))
      return DiscardWrite();
    wr_ = 0;
    stats_.set_write_wrap_count(stats_.write_wrap_count() + 1);
    PERFETTO_DCHECK(size_to_end() >= tbchunk_outer_size);
  }

  // Deletes all chunks from |wptr_| to |wptr_| + |record_size|.
  if (!DeleteNextChunksFor(tbchunk_outer_size))
    return DiscardWrite();

  TBChunk* tbchunk = GetTBChunkAtUnchecked(wr_);
  new (tbchunk) TBChunk(all_frags_size);
  tbchunk->chunk_id = chunk_id;
  tbchunk->flags = chunk_flags;
  auto* payload_begin = reinterpret_cast<uint8_t*>(tbchunk) + sizeof(TBChunk);
  uint8_t* wptr = payload_begin;

  // Copy all the (valid) fragments from the SMB chunk to the TBChunk.
  memcpy(wptr, src, all_frags_size);
  wptr += all_frags_size;
  PERFETTO_DCHECK(static_cast<size_t>(wptr - payload_begin) == all_frags_size);

  auto seq_key = MkProducerAndWriterID(producer_id_trusted, writer_id);
  auto seq_it = sequences_.find(seq_key);
  if (seq_it == sequences_.end()) {
    auto it_and_inserted = sequences_.emplace(
        seq_key,
        SequenceState(producer_id_trusted, writer_id, client_identity_trusted));
    seq_it = it_and_inserted.first;
  }
  SequenceState& seq = seq_it->second;

  // Insert the chunk in-order in the linked list.
  // TODO handle re-commit of same chunk id.
  auto& chunk_list = seq.chunks;
  auto insert_pos = chunk_list.rbegin();
  while (insert_pos != chunk_list.rend() &&
         ChunkIdCompare(chunk_id, GetTBChunkAt(*insert_pos)->chunk_id) < 0) {
    ++insert_pos;
  }
  chunk_list.InsertBefore(insert_pos, OffsetOf(tbchunk));

  stats_.set_chunks_written(stats_.chunks_written() + 1);
  stats_.set_bytes_written(stats_.bytes_written() + tbchunk_outer_size);
  // TODO more to do here, look at v1.
}

// Note for reviewer: unlike the old v1 impl, here DeleteNextChunksFor also
// takes care of writing the padding chunk in case of truncation.
// TODO copy over the diagram from v1::CopyChunkUntrusted.
bool TraceBufferV2::DeleteNextChunksFor(size_t bytes_to_clear) {
  PERFETTO_DCHECK(bytes_to_clear > sizeof(TBChunk));
  PERFETTO_DCHECK((bytes_to_clear % alignof(TBChunk)) == 0);
  // TODO apply logic that returns false if discard mode.
  // PERFETTO_CHECK(!discard_writes_);
  DcheckIsAlignedAndWithinBounds(wr_);
  const size_t clear_end = wr_ + bytes_to_clear;
  TRACE_BUFFER_DLOG("Delete [%zu %zu]", wr_, clear_end);
  PERFETTO_DCHECK(clear_end <= size_);

  // TODO DNS TOMORROW: this logic is wrong, too brittle and incomplete on 2nd
  // pass after we have read already (?).
  if (PERFETTO_UNLIKELY(wr_ == used_size_)) {
    // This happens only on the first write, when the buffer is 0-initialized
    // and we have no chunks at all.
    EnsureCommitted(clear_end);
    return true;
  }
  // uint64_t chunks_overwritten = stats_.chunks_overwritten();
  // uint64_t bytes_overwritten = stats_.bytes_overwritten();
  // uint64_t padding_bytes_cleared = stats_.padding_bytes_cleared();

  // TODO here what if there is nothing to read?

  BeginRead(/*limit=*/bytes_to_clear);
  for (;;) {
    TracePacket packet;
    PacketSequenceProperties seq_props{};
    bool dropped;  // TODO maybe
    bool res = ReadNextTracePacket(&packet, &seq_props, &dropped,
                                   /*force_erase=*/true);
    if (!res)
      break;
  }

  // When we set a limit in BeginRead,ReadNextTracePacket must stop precisely
  // at the chunk that contains the limit (bytes_to_clear).
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
  PERFETTO_CHECK(clear_end >= chunk_start && clear_end <= chunk_end);

  // TODO erase last_chunk_cleared? or did the read do that?

  TBChunk* pad_chunk = GetTBChunkAtUnchecked(clear_end);
  new (pad_chunk) TBChunk(chunk_end - clear_end);
  // TODO below
  // stats_.set_padding_bytes_cleared(padding_bytes_cleared);
  // update chunks_overwritten and bytes_overwritten
  return true;
}

// TODO the caller of this must walk back to the chain.
void TraceBufferV2::EraseTBChunk(TBChunk* tbchunk, SequenceState* seq) {
  if (tbchunk->is_padding())
    return;
  const uint32_t size = tbchunk->payload_size;  // TODO check statistics

  auto& chunk_list = seq->chunks;
  // At the time of writing the only case when we invoke EraseTBChunk is to
  // delete the first chunk of the sequence. Deleting the chunks in any other
  // order feels suspicious. If you ever need to remove this CHECK ask yourself
  // if you have been thinking of all the possible implications of it.
  PERFETTO_CHECK(*chunk_list.begin() == OffsetOf(tbchunk));
  chunk_list.pop_front();

  tbchunk->pri_wri_id = 0;
  tbchunk->chunk_id = 0;
  tbchunk->payload_size = 0;
  tbchunk->payload_consumed = 0;
  tbchunk->flags = 0;

  // TODO handle compaction by remembering the last erased chunk. But also
  // remember to not copy it when cloning the iterator.

  stats_.set_chunks_overwritten(stats_.chunks_overwritten() + 1);
  stats_.set_bytes_overwritten(stats_.bytes_overwritten() + size);
}

bool TraceBufferV2::TryPatchChunkContents(ProducerID,
                                          WriterID,
                                          ChunkID,
                                          const Patch* patches,
                                          size_t patches_size,
                                          bool other_patches_pending) {
  (void)patches;
  (void)patches_size;
  (void)other_patches_pending;
  return false;
}

}  // namespace perfetto

// TODO add a test that simulates: scrape, readback, then comitting a chunk
// with the same id, and make sure on the next read we only read the new frag.
// There might be one already, but check.
