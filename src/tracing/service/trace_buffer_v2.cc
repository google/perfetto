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

constexpr uint8_t kChunkComplete = 0x80;

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
  if (buf_->used_size_ > 0)
    NextChunkInBuffer();
}

// static
BufIterator BufIterator::CloneReadOnly(const BufIterator& other) noexcept {
  auto bi = BufIterator(other);
  bi.read_only_iterator_ = true;
  bi.data_loss_ = false;
  return bi;
}

bool BufIterator::NextChunkInBuffer() {
  PERFETTO_DCHECK(buf_->used_size_ > 0);
  PERFETTO_DCHECK(buf_->wr_ < buf_->size_);

  // Before starting the loop we need to determine the offset of the current
  // chunk, the one we are advancing from. Here we have a classic iterator
  // problem, where the first time we build the iterator we need to do some
  // slightly different work to identify the first valid chunk.
  // Overall, we have 3 cases:
  // Case 1: wr_ hasn't wrapped yet, not even once. So wr_ == used_size_.
  //         we want to start reading at 0, and stop at used_size_.
  // Case 2: wr_ wrapped at least once and is somewhere in the middle of
  //         the buffer. The data immediately after wr_ is the oldest data
  //         where we want to start from. We want to start @ wr_, wrap over
  //         at used_size_ and stop once we reach again wr_.
  // Case 3: (edge case) wr_ wrapped and is precisely at offset 0. Proceed as
  //         case 2 (start at 0, wrap, end at 0).

  TBChunk* cur_chunk = nullptr;
  size_t off;
  if (end_chunk_ == nullptr) {
    // end_chunk_ is nullptr the first time we initialize from the ctor.
    if (buf_->wr_ < buf_->used_size_) {
      off = buf_->wr_;
    } else {
      off = 0;
      if (limit_ > 0)
        return false;
    }
    // cur_chunk stays deliberately = nullptr
  } else {
    cur_chunk = end_chunk_;
    off = buf_->OffsetOf(cur_chunk);
  }

  for (;;) {
    // If we have a chunk (always, with the exception of the first call from the
    // constructor) move to its end and start the search there.
    if (cur_chunk) {
      PERFETTO_DCHECK(cur_chunk->IsChecksumValid(off));
      off += cur_chunk->outer_size();
      if (limit_ && off >= limit_)
        return false;
      if (off == buf_->used_size_) {
        if (buf_->wr_ == buf_->used_size_) {
          return false;  // Case 1.
        }
        off = 0;
      }
      if (off == buf_->wr_)
        return false;  // Case 2-3. We reached the write pointer. Stop.
    }

    cur_chunk = buf_->GetTBChunkAt(off);
    if (cur_chunk->is_padding()) {
      continue;  // The chunk has been erased.
    }

    SequenceState* seq = buf_->GetSeqForChunk(cur_chunk);
    PERFETTO_DCHECK(seq);  // A non-padding chunk must be part of a sequence.

    // `next_chunk` might not be logically contiguous with the last chunk
    // consumed for the sequence. However that does NOT imply that we are
    // missing the chunk. Giving up here would be premature.
    // Imagine the buffer contains, in buffer order:
    // C1 (last consumed), C3 (cur_chunk), C2. Here we want to rewind in the
    // sequence (using SequenceState.chunks) to see if we have C2.
    // If yes, we should start a detour and proceed in sequence order until we
    // reach back C3 (which is where we started in buffer order).
    // If not, we have a gap and we should skip this sequence for the current
    // read cycle.
    // However, if instead we have: C1 (last consumed), C4 (next_chunk), C3
    // we should TODO here just mark data loss skip the sequence now and in
    // future (until BeginRead() is called again).

    auto& chunk_list = seq->chunks;
    PERFETTO_CHECK(!chunk_list.empty());
    PERFETTO_DCHECK(chunk_list.Find(off) != chunk_list.end());
    size_t first_off = *chunk_list.begin();
    TBChunk* first_chunk_of_seq = buf_->GetTBChunkAt(first_off);
    PERFETTO_DCHECK(!first_chunk_of_seq->is_padding());

    // TODO this cannot fail because violates the expectations of
    // // DeleteNextChunksFor(...)
    if (SetNextChunkIfContiguousAndValid(
            seq, /*prev_chunk_id=*/seq->last_chunk_id_consumed,
            /*next_chunk=*/first_chunk_of_seq, /*next_seq_idx=*/0)) {
      end_chunk_ = cur_chunk;
      return true;
    }
  }
}

bool BufIterator::NextChunkInSequence() {
  PERFETTO_DCHECK(valid());
  PERFETTO_DCHECK(seq_);

  auto& chunk_list = seq_->chunks;

  // Either the current chunk has been deleted (is_padding()), or if it exist it
  // must be consistent with the internal tracking in seq_idx_.
  PERFETTO_DCHECK(chunk_->is_padding() ||
                  chunk_list.at(seq_idx_) == buf_->OffsetOf(chunk_));

  size_t next_seq_idx = seq_idx_ + 1;
  if (next_seq_idx >= chunk_list.size()) {
    // There is no "next chunk" the chunk list for this sequence.
    // NOTE: this has nothing to do with the "ChunkID is not consecutive" check
    // which is performed below. This is a more basic failure mode where we just
    // don't have any chunks at all, whether they are consecutive or not.
    return false;
  }

  // At this point we need to work out if the sequence of ChunkID(s) is
  // contiguous or we have gaps. There are two scenarios here:
  // 1) We are iterating and consuming as part of ReadNextTracePacket(). When
  //    we do this, each iteration erases the last chunk before moving onto the
  //    next one. We can't use the SequenceState.chunks because the upcoming
  //    chunk will always be the "first" in this case. However, we can look at
  //    last_chunk_id_consumed.
  // 2) We are iterating read-only as part of ReassembleFragmentedPacket(). In
  //    this case we are not consuming any chunk, and we can use the combination
  //    of SequenceState.chunks[SequenceState.next_seq_idx].

  std::optional<ChunkID> last_chunk_id;
  if (chunk_->is_padding()) {
    last_chunk_id = seq_->last_chunk_id_consumed;  // Case 1.
  } else {
    last_chunk_id = chunk_->chunk_id;  // Case 2
  }

  size_t next_chunk_off = chunk_list.at(next_seq_idx);       // O(1)
  TBChunk* next_chunk = buf_->GetTBChunkAt(next_chunk_off);  // O(1)

  return SetNextChunkIfContiguousAndValid(seq_, last_chunk_id, next_chunk,
                                          next_seq_idx);
}

// TODO remove the IfContiguous part of the name
bool BufIterator::SetNextChunkIfContiguousAndValid(
    SequenceState* seq,
    const std::optional<ChunkID>& prev_chunk_id,
    TBChunk* next_chunk,
    size_t next_seq_idx) {
  PERFETTO_DCHECK(seq);

  if (seq->skip_in_generation == buf_->read_generation_) {
    // If we hit this, it means that we tried to read this chunk while
    // trying to reassmble a fragmented packet started in a prior chunk, but
    // we failed. We want to skip any chunk in this sequence until the next
    // BeginRead(), which will increment the read_pass_.
    return false;
  }

  PERFETTO_DCHECK(seq->chunks.at(next_seq_idx) == buf_->OffsetOf(next_chunk));

  bool chunk_id_gap =
      prev_chunk_id.has_value() && next_chunk->chunk_id != *prev_chunk_id + 1;

  bool needs_patching = next_chunk->flags & kChunkNeedsPatching;

  if (/*chunk_id_gap || */ needs_patching) {
    if (PERFETTO_LIKELY(!read_only_iterator_)) {
      seq->skip_in_generation = buf_->read_generation_;
    }
    return false;
  }

  if (chunk_id_gap)
    data_loss_ = true;

  chunk_ = next_chunk;
  seq_ = seq;
  seq_idx_ = next_seq_idx;
  next_frag_off_ = next_chunk->unread_payload_off();
  return true;
}

// See comments around BufIterator in the .h file for the rationale.
bool BufIterator::NextChunk() {
  PERFETTO_DCHECK(valid());
  const bool move_in_seq_order = chunk_ != end_chunk_;
  if (move_in_seq_order) {
    // Move to the next chunk in the linked list.
    // We should be able to move next in the list because we reached this
    // state by initially rewining to the beginning of the list.
    // However, if there is a gap in the sequence (e.g. producer data loss)
    // NextChunkInSequence() will return false.
    if (NextChunkInSequence()) {
      return true;
    }
  }

  // Otherwise Move to the next chunk in buffer order.
  return NextChunkInBuffer();
}

std::optional<TraceBufferV2::Frag> BufIterator::NextFragmentInChunk() {
  PERFETTO_DCHECK(valid());
  // We don't need to do anything special about padding chunks, because their
  // payload_avail is always 0.

  PERFETTO_DCHECK(next_frag_off_ >= chunk_->unread_payload_off());
  PERFETTO_DCHECK(next_frag_off_ <= chunk_->payload_size);

  uint8_t* chunk_end = chunk_->fragments_end();
  uint8_t* hdr_begin = chunk_->fragments_begin() + next_frag_off_;
  if (hdr_begin >= chunk_end)
    return std::nullopt;

  Frag frag{};
  bool is_first_frag = next_frag_off_ == 0;
  uint64_t frag_size_u64 = 0;

  // TODO refactor this code and CopyChunkUntrusted into TokenizeFrag or
  // similar.
  uint8_t* frag_begin =
      const_cast<uint8_t*>(ParseVarInt(hdr_begin, chunk_end, &frag_size_u64));
  size_t hdr_size =  // The fragment "header" is just a varint stating its size.
      static_cast<size_t>(reinterpret_cast<uintptr_t>(frag_begin) -
                          reinterpret_cast<uintptr_t>(hdr_begin));

  // TODO what happens if hdr_size == 0? do we spin loop?
  if (frag_size_u64 > reinterpret_cast<uintptr_t>(chunk_end) -
                          reinterpret_cast<uintptr_t>(frag_begin)) {
    // TODO mark ABI violation in stats.
    PERFETTO_DCHECK(buf_->suppress_client_dchecks_for_testing_);
    chunk_->payload_avail = 0;
    // TODO consume and delete chunk.
    return std::nullopt;
  }

  frag.chunk = chunk_;
  frag.seq = seq_;
  frag.off = next_frag_off_;
  frag.hdr_size = static_cast<uint8_t>(hdr_size);
  frag.size = static_cast<uint16_t>(hdr_size + frag_size_u64);
  next_frag_off_ += frag.size;
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

bool BufIterator::EraseCurrentChunkAndMoveNext() {
  PERFETTO_DCHECK(seq_);
  // We should not erase an unconsumed chunk. The cases of ABI violation should
  // forcefully clear the payload_avail.
  PERFETTO_DCHECK(chunk_->payload_avail == 0);
  if (!(chunk_->flags & kChunkComplete)) {
    // TODO this doesn't work with EraseNextChunksFor
    seq_->skip_in_generation = buf_->read_generation_;  // TODO: why?
  } else if (!chunk_->is_padding()) {
    size_t chunk_off = buf_->OffsetOf(chunk_);
    TRACE_BUFFER_DLOG("EraseChunk(%zu)", chunk_off);
    const uint32_t chunk_size = chunk_->size;
    const uint32_t outer_size = chunk_->outer_size();
    seq_->last_chunk_id_consumed = chunk_->chunk_id;

    // TODO check statistics

    // At the time of writing the only case when we invoke EraseTBChunk is to
    // delete the first chunk of the sequence. Deleting the chunks in any other
    // order feels suspicious. If you ever need to remove this CHECK ask
    // yourself if you have been thinking of all the possible implications of
    // it.
    auto& chunk_list = seq_->chunks;

    // At the time of writing we only support erasing the first chunk of the
    // sequence. Erasing from the middle is possible but requires more efforts
    // to keep in sync the SequenceState.chunks with our seq_idx_.
    PERFETTO_CHECK(seq_idx_ == 0 && *chunk_list.begin() == chunk_off);
    chunk_list.pop_front();

    // Zero all the fields of the chunk.
    uint16_t old_payload_size = chunk_->payload_size;
    TBChunk* cleared_chunk = buf_->CreateTBChunk(chunk_off, chunk_size);
    cleared_chunk->payload_size = old_payload_size;

    auto& stats = buf_->stats_;
    stats.set_chunks_overwritten(stats.chunks_overwritten() + 1);
    stats.set_bytes_overwritten(stats.bytes_overwritten() + outer_size);
  }  //  if (!is_padding)

  // Rationale for the -1: seq_idx_ is expected to point to the current chunk_.
  // However we have just deleted the "current" chunk from the
  // SequenceState.chunks list. The best "current chunk" is 0xff..ff, so that
  // the next call to NextChunkInSequence will +1 and move to the 0th entry,
  // which is the right "next".
  seq_idx_ = std::numeric_limits<decltype(seq_idx_)>::max();
  return NextChunk();
}
}  // namespace internal

// +---------------------------------------------------------------------------+
// | TraceBuffer implementation                                                |
// +---------------------------------------------------------------------------+

// static
std::unique_ptr<TraceBufferV2> TraceBufferV2::Create(size_t size_in_bytes,
                                                     OverwritePolicy pol) {
  // The size and alignment of TBChunk have implications on the memory
  // efficiency.
  static_assert(sizeof(TBChunk) == 16);
  static_assert(alignof(TBChunk) == 4);
  std::unique_ptr<TraceBufferV2> trace_buffer(new TraceBufferV2(pol));
  if (!trace_buffer->Initialize(size_in_bytes))
    return nullptr;
  return trace_buffer;
}

TraceBufferV2::TraceBufferV2(OverwritePolicy pol)
    : overwrite_policy_(pol), rd_iter_(this) {}

bool TraceBufferV2::Initialize(size_t size) {
  size = base::AlignUp(std::max(size, size_t(1)), 4096);
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
  TRACE_BUFFER_DLOG("BeginRead(limit=%zu)", limit);
  ++read_generation_;
  rd_iter_.Reset(limit);
}

bool TraceBufferV2::ReadNextTracePacket(
    TracePacket* out_packet,
    PacketSequenceProperties* sequence_properties,
    bool* previous_packet_on_sequence_dropped) {
  auto res = ReadNextTracePacketInternal(out_packet, sequence_properties,
                                         ReadPolicy::kStandardRead);
  *previous_packet_on_sequence_dropped = rd_iter_.data_loss();
  return res == ReadRes::kOk;
}

TraceBufferV2::ReadRes TraceBufferV2::ReadNextTracePacketInternal(
    TracePacket* out_packet,
    PacketSequenceProperties* sequence_properties,
    ReadPolicy read_policy) {
  TRACE_BUFFER_DLOG("ReadNextTracePacket(policy=%d)", read_policy);
  bool force_erase = read_policy == kForceErase;
  DumpForTesting();  // DNS

  // Just in case we forget to initialize these below.
  *sequence_properties = {0, ClientIdentity(), 0};
  // TODO DCHECK changed_since_last_read_
  for (;;) {
    if (!rd_iter_.valid())
      return ReadRes::kFail;

    if (PERFETTO_UNLIKELY(read_policy == kNoOverwrite)) {
      // kNoOverwrite is set by DeleteNextChunksFor() when the buffer is in
      // DISCARD mode. If we end up hitting valid data we should bail.
      if (rd_iter_.chunk()->payload_avail > 0) {
        return ReadRes::kWouldOverwrite;
      }
    }

    // If the current chunk is a padding chunk, NextFragmentInChunk() will just
    // rerturn nullopt. no need of special casing it.
    std::optional<Frag> maybe_frag = rd_iter_.NextFragmentInChunk();
    if (!maybe_frag.has_value()) {
      // We read all the fragments in the current chunk (or the current chunk
      // has none). Erase the current chunk and move to the next chunk.
      bool has_next_chunk = rd_iter_.EraseCurrentChunkAndMoveNext();
      // NextChunk() moves "in the right direction", either in buffer order or
      // sequence order, depending on its internal state. If it returns false
      // we wrapped around the ring buffer and hit the wr_ pointer again.
      if (!has_next_chunk) {
        // (1) There is nothing else to read in the buffer; (2) We reached the
        // `limit` passed to BeginRead() (for the deletion case).
        TRACE_BUFFER_DLOG("  ReadNextTracePacket -> false");
        return ReadRes::kFail;
      }
      continue;
    }
    Frag& frag = *maybe_frag;
    switch (frag.type) {
      case Frag::kFragWholePacket:
        // It's questionable whether we should propagate out empty packets. Here
        // we match the implementation of the old TraceBuffer. Some client might
        // be relying on the fact that empty packets don't bloat the final trace
        // file size.
        ConsumeFragment(&frag);
        if (frag.payload_size() == 0) {
          break;
        }
        out_packet->AddSlice(frag.begin(), frag.payload_size());
        *sequence_properties = {frag.seq->producer_id,
                                frag.seq->client_identity,
                                frag.seq->writer_id};
        TRACE_BUFFER_DLOG("  ReadNextTracePacket -> true (whole packet)");
        return ReadRes::kOk;

      case Frag::kFragContinue:
      case Frag::kFragEnd:
        // We should never hit these cases while iterating in this loop.
        // In nominal conditions we should only see kFragBegin, and then we
        // should iterate over the Continue/End in ReassembleFragmentedPacket(),
        // which performs the lookahead. If we hit this code path, either a
        // producer emitted a chunk that looks like
        // [kWholePacket, kFragContinue] or [kWholePacket, kFragEnd].
        // or, more realistically, we had a data losss and missed the chunk with
        // the kFragBegin.
        rd_iter_.set_data_loss();  // TODO This is a bit weird.
        ConsumeFragment(&frag);
        break;

      case Frag::kFragBegin:
        auto reassembly_res =
            ReassembleFragmentedPacket(out_packet, &frag, force_erase);
        if (reassembly_res == FragReassemblyResult::kSuccess) {
          *sequence_properties = {frag.seq->producer_id,
                                  frag.seq->client_identity,
                                  frag.seq->writer_id};
          stats_.set_readaheads_succeeded(stats_.readaheads_succeeded() + 1);

          // We found and consumed all the fragments for the packet.
          // On the next ReadNextTracePacket() call, NextFragmentInChunk() will
          // return nullopt (because, modulo client bugs, the kFragBegin here
          // is the alst fragment of the chunk). That code branch above will
          // erase the chunk and continue with the next chunk (either in buffer
          // or sequence order).
          TRACE_BUFFER_DLOG("  ReadNextTracePacket -> true (reassembly)");
          return ReadRes::kOk;
        }
        if (reassembly_res == FragReassemblyResult::kDataLoss) {
          // If we detect a data loss, ReassembleFragmentedPacket() marks
          // all fragments as consumed, so they don't trigger further error
          // stats when we iterate over them again.
          // The break below will continue with the next fragments leaving the
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
        stats_.set_readaheads_failed(stats_.readaheads_failed() + 1);
        // In this case we need two different behaviors:
        // 1. If we are doing a pure readback (force_erase = false), we should
        //    move away from this sequence non-destructively, as there is a
        //    chance that the missing chunks will appear in future.
        //    Note that by moving to NextChunkInBuffer we might still stumble
        //    on futher chunks of the current sequence. But the read_pass_
        //    generation counter will cause BufIterator to skip over them.
        // 2. We are overwriting chunks as part of a write: this was the last
        //    chance to read back the data. We should destroy it and treat
        //    this as a kDataLoss.
        if (!force_erase) {
          // Case 1: Continue the iteration on the next chunk in buffer order.
          // Any chunks of the current sequence encountered within the current
          // BeginRead() session will be skipped. TODO explain this is to avoid
          // marking future chunks as data losses or loops.
          frag.seq->skip_in_generation = read_generation_;

          // TODO make thid rd_iter_.InterruptSequence and hanlde the rest above
          // uniformly. Hmm not that easy, we don't want to EraseCurrentChunk
          // in this case.
          if (!rd_iter_.NextChunkInBuffer()) {
            TRACE_BUFFER_DLOG("  ReadNextTracePacket -> false (reassembly)");
            return ReadRes::kFail;
          }
        }
        // In case 2: ReassembleFragmentedPacket has invalidated the fragments,
        // we should break the switch and continue with whatever is next.
        break;  // case kFragBegin
    }  // switch(frag.type)
  }  // for (;;)
  PERFETTO_DCHECK(false);  // Unreacahble
  return ReadRes::kFail;
}

TraceBufferV2::FragReassemblyResult TraceBufferV2::ReassembleFragmentedPacket(
    TracePacket* out_packet,
    Frag* initial_frag,
    bool force_erase) {
  PERFETTO_DCHECK(initial_frag->type == Frag::kFragBegin);

  base::SmallVector<Frag, 16> frags;
  frags.emplace_back(*initial_frag);
  BufIterator it = BufIterator::CloneReadOnly(rd_iter_);

  // Iterate over chunks using the linked list.
  FragReassemblyResult res;
  for (;;) {
    PERFETTO_DCHECK((it.valid()));
    if (!it.NextChunkInSequence()) {
      res = FragReassemblyResult::kNotEnoughData;
      break;
    }
    if (it.data_loss()) {
      // There is a gap in the sequence ID.
      // TODO reason if we want to delete now also next fragments.
      res = FragReassemblyResult::kDataLoss;
      break;
    }
    // TODO explain why we don't iterate over fragments but only read one.
    std::optional<Frag> frag = it.NextFragmentInChunk();
    if (!frag.has_value()) {
      // This can happen if a chunk in the middle of a sequence is empty. Rare
      // but technically possible. See test Fragments_EmptyChunkInTheMiddle.
      continue;
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
    if (res == FragReassemblyResult::kSuccess && f.payload_size() > 0) {
      out_packet->AddSlice(f.begin(), f.payload_size());
    }
    if (res == FragReassemblyResult::kSuccess ||
        res == FragReassemblyResult::kDataLoss || force_erase) {
      ConsumeFragment(&f);
    }
  }
  return res;
}

void TraceBufferV2::ConsumeFragment(Frag* frag) {
  TBChunk* chunk = frag->chunk;
  // We must consume fragments in order (and no more than once).
  PERFETTO_DCHECK(frag->off == chunk->unread_payload_off());
  PERFETTO_DCHECK(chunk->payload_avail >= frag->size);
  chunk->payload_avail -= frag->size;
  // TODO update stats
  if (chunk->payload_avail == 0) {
    stats_.set_chunks_read(stats_.chunks_read() + 1);
    stats_.set_bytes_read(stats_.bytes_read() + chunk->outer_size());
    // EraseTBChunk(chunk, frag->seq);
  }
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
    size_t src_size) {
  TRACE_BUFFER_DLOG("");
  TRACE_BUFFER_DLOG("CopyChunkUntrusted(%zu) @ wr_=%zu", src_size, wr_);

  if (PERFETTO_UNLIKELY(discard_writes_))
    return DiscardWrite();

  // Note: `src` points to the first packet fragment in the chunk. The caller
  // (TracingServiceImpl) does the chunk header decoding for us and breaks it
  // down into the various args passed here.
  // Each fragment is prefixed by a
  const uint8_t* cur = src;
  const uint8_t* const end = src + src_size;

  // chunk_complete is true in the majority of cases, and is false only when
  // the service performs SBM scraping (upon flush).
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
  } else {
    chunk_flags |= kChunkComplete;
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

    TRACE_BUFFER_DLOG("  Frag %u: %p - %p", frag_idx, (void*)(payload_begin),
                      (void*)(payload_end));
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
    if (PERFETTO_UNLIKELY(payload_end > end || payload_end < payload_begin)) {
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
  PERFETTO_CHECK(all_frags_size <= src_size);

  // Make space in the buffer for the chunk we are about to copy.

  // TODO update stats

  size_t tbchunk_size = all_frags_size;
  if (PERFETTO_UNLIKELY(!chunk_complete)) {
    // If the chunk is incomplete (due to scraping), we want to reserve the
    // whole chunk space in the buffer, to allow later re-commits that will
    // increase the payload size.
    tbchunk_size = src_size;
  }
  const size_t tbchunk_outer_size = TBChunk::OuterSize(tbchunk_size);

  if (PERFETTO_UNLIKELY(tbchunk_outer_size > size_)) {
    // The chunk is bigger than the buffer. Extremely rare, but can happen, e.g.
    // if the user has specified a 16KB buffer and the SMB chunk is 32KB.
    stats_.set_abi_violations(stats_.abi_violations() + 1);
    PERFETTO_DCHECK(suppress_client_dchecks_for_testing_);
    return;
  }

  auto seq_key = MkProducerAndWriterID(producer_id_trusted, writer_id);
  auto seq_it = sequences_.find(seq_key);
  if (seq_it == sequences_.end()) {
    TRACE_BUFFER_DLOG("  Added seq %x", seq_key);
    auto it_and_inserted = sequences_.emplace(
        seq_key,
        SequenceState(producer_id_trusted, writer_id, client_identity_trusted));
    seq_it = it_and_inserted.first;
  }
  SequenceState& seq = seq_it->second;
  if (seq.last_chunk_id_consumed.has_value() &&
      ChunkIdCompare(chunk_id, *seq.last_chunk_id_consumed) <= 0) {
    // TODO discuss this with daniele.
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

  // Find the insert poisition in the SequenceState's chunk list. We iterate the
  // list in reverse order as in the majority of cases chunks arrive naturally
  // in order. SMB scraping is really the only thing that might commit chunks
  // slightly out of order.
  // This loop must happen before the DeleteNextChunksFor(), as that can
  // delete chunks and hence invalidate the `insert_pos`.
  auto& chunk_list = seq.chunks;
  auto insert_pos = chunk_list.rbegin();
  TBChunk* recommit_chunk = nullptr;
  for (; insert_pos != chunk_list.rend(); ++insert_pos) {
    TBChunk* other_chunk = GetTBChunkAt(*insert_pos);
    int cmp = ChunkIdCompare(chunk_id, other_chunk->chunk_id);
    if (cmp > 0)
      break;
    if (cmp == 0) {
      // The producer is trying to re-commit a previously copied chunk. This
      // can happen when the service does SMB scraping (the same chunk could
      // be scraped more than once), and later the producer does a commit.
      // We allow recommit only if the new chunk is larger than the existing.
      recommit_chunk = other_chunk;
      break;
    }
  }

  // In the case of a re-commit we don't need to create a new chunk, we just
  // want to overwrite the existing one.
  if (recommit_chunk) {
    if (all_frags_size < recommit_chunk->payload_size ||
        all_frags_size > recommit_chunk->size ||
        (recommit_chunk->flags & chunk_flags) != recommit_chunk->flags) {
      // The payload should never shrink, cannot grow more than the original
      // chunk size. Flags can be added but not removed.
      stats_.set_abi_violations(stats_.abi_violations() + 1);
      PERFETTO_DCHECK(suppress_client_dchecks_for_testing_);
      return;
    }
    if (all_frags_size == recommit_chunk->payload_size) {
      TRACE_BUFFER_DLOG("  skipping recommit of identical chunk");
      return;
    }
    uint16_t payload_consumed =
        recommit_chunk->payload_size - recommit_chunk->payload_avail;
    recommit_chunk->payload_size = all_frags_size;
    recommit_chunk->payload_avail = all_frags_size - payload_consumed;
    memcpy(recommit_chunk->fragments_begin(), src, all_frags_size);
    recommit_chunk->flags |= chunk_flags;
    stats_.set_chunks_rewritten(stats_.chunks_rewritten() + 1);
    return;
  }

  TBChunk* tbchunk = CreateTBChunk(wr_, tbchunk_size);
  tbchunk->payload_size = all_frags_size;
  tbchunk->payload_avail = all_frags_size;
  tbchunk->chunk_id = chunk_id;
  tbchunk->flags = chunk_flags;
  tbchunk->pri_wri_id = seq_key;
  auto* payload_begin = reinterpret_cast<uint8_t*>(tbchunk) + sizeof(TBChunk);
  uint8_t* wptr = payload_begin;

  // Copy all the (valid) fragments from the SMB chunk to the TBChunk.
  memcpy(wptr, src, all_frags_size);

  PERFETTO_DCHECK(wr_ == OffsetOf(tbchunk));
  if (insert_pos != chunk_list.rbegin()) {
    stats_.set_chunks_committed_out_of_order(
        stats_.chunks_committed_out_of_order() + 1);
  }
  chunk_list.InsertAfter(insert_pos, wr_);

  TRACE_BUFFER_DLOG(" END OF CopyChunkUntrusted(%zu) @ wr=%zu", src_size, wr_);

  wr_ += tbchunk_outer_size;
  PERFETTO_DCHECK(wr_ <= size_ && wr_ <= used_size_);
  wr_ = wr_ >= size_ ? 0 : wr_;

  stats_.set_chunks_written(stats_.chunks_written() + 1);
  stats_.set_bytes_written(stats_.bytes_written() + tbchunk_outer_size);

  // TODO more to do here, look at v1.
}

TraceBufferV2::TBChunk* TraceBufferV2::CreateTBChunk(size_t off, size_t size) {
  DcheckIsAlignedAndWithinBounds(off);
  size_t end = off + TBChunk::OuterSize(size);
  if (end > used_size_) {
    used_size_ = end;
    data_.EnsureCommitted(end);
  }
  TBChunk* chunk = GetTBChunkAtUnchecked(off);
  return new (chunk) TBChunk(off, size);
}

// Note for reviewer: unlike the old v1 impl, here DeleteNextChunksFor also
// takes care of writing the padding chunk in case of truncation.
// TODO copy over the diagram from v1::CopyChunkUntrusted.
bool TraceBufferV2::DeleteNextChunksFor(size_t bytes_to_clear) {
  TRACE_BUFFER_DLOG("DeleteNextChunksFor(%zu) @ wr=%zu", bytes_to_clear, wr_);
  PERFETTO_CHECK(!discard_writes_);
  PERFETTO_DCHECK(bytes_to_clear >= sizeof(TBChunk));
  PERFETTO_DCHECK((bytes_to_clear % alignof(TBChunk)) == 0);
  PERFETTO_DCHECK(bytes_to_clear <= TBChunk::kMaxSize);
  DcheckIsAlignedAndWithinBounds(wr_);
  const size_t clear_end = wr_ + bytes_to_clear;
  PERFETTO_DCHECK(clear_end <= size_);

  // uint64_t chunks_overwritten = stats_.chunks_overwritten();
  // uint64_t bytes_overwritten = stats_.bytes_overwritten();
  // uint64_t padding_bytes_cleared = stats_.padding_bytes_cleared();

  // TODO here what if there is nothing to read?

  BeginRead(/*limit=*/clear_end);
  for (;;) {
    TracePacket packet;
    PacketSequenceProperties seq_props{};
    ReadPolicy read_pol = overwrite_policy_ == kDiscard
                              ? ReadPolicy::kNoOverwrite
                              : ReadPolicy::kForceErase;
    ReadRes res = ReadNextTracePacketInternal(&packet, &seq_props, read_pol);
    if (res == ReadRes::kFail) {
      break;
    }
    if (res == ReadRes::kWouldOverwrite) {
      PERFETTO_DCHECK(overwrite_policy_ == kDiscard);
      return false;
    }
  }

  // When we set a limit in BeginRead(), ReadNextTracePacket() will stop
  // at the chunk that contains the limit (bytes_to_clear), unless
  // there are no chunks in the buffer (we are at the first write pass and
  // haven't wrapped even once).
  // Note that ReadNextTracePacket() might stop well before the limit, if the
  // last chunks that precede the limits are already cleared. So we can't just
  // assume that it will stop _precisely_ on that chunk. But we can assume that
  // it will free up all the chunks between wr_ and the clear_end limit (incl.).
  // As part of the its walking algorithm, it might free up also chunks that
  // are not in the range [wr_, clear_end] if they happen to be earlier in the
  // sequence of one of the chunks in that range.
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

  for (size_t off = wr_; off < clear_end && off < used_size_;) {
    TBChunk* chunk = GetTBChunkAt(off);
    PERFETTO_DCHECK(chunk->is_padding());
    size_t chunk_end = off + chunk->outer_size();
    if (clear_end > off && clear_end < chunk_end) {
      PERFETTO_DCHECK(chunk_end - clear_end >= sizeof(TBChunk));
      // Create a zero padding chunk at the end.
      CreateTBChunk(clear_end, chunk_end - clear_end - sizeof(TBChunk));
    }
    off += chunk->outer_size();
  }

  // stats_.set_padding_bytes_cleared(padding_bytes_cleared);
  // update chunks_overwritten and bytes_overwritten
  return true;
}

bool TraceBufferV2::TryPatchChunkContents(ProducerID producer_id,
                                          WriterID writer_id,
                                          ChunkID chunk_id,
                                          const Patch* patches,
                                          size_t patches_size,
                                          bool other_patches_pending) {
  ProducerAndWriterID seq_key = MkProducerAndWriterID(producer_id, writer_id);
  auto seq_it = sequences_.find(seq_key);
  if (seq_it == sequences_.end()) {
    stats_.set_patches_failed(stats_.patches_failed() + 1);
    return false;
  }

  // We have to do a linear search to find the chunk to patch. In the majority
  // of cases the chunk to patch is one of the last ones committed, so we walk
  // the list backwards.
  auto& chunk_list = seq_it->second.chunks;
  TBChunk* chunk = nullptr;
  for (auto it = chunk_list.rbegin(); it != chunk_list.rend(); ++it) {
    TBChunk* it_chunk = GetTBChunkAt(*it);
    if (it_chunk->chunk_id == chunk_id) {
      chunk = it_chunk;
      break;
    }
  }

  if (chunk == nullptr) {
    stats_.set_patches_failed(stats_.patches_failed() + 1);
    return false;
  }

  size_t payload_size = chunk->payload_size;
  PERFETTO_DCHECK(chunk->chunk_id == chunk_id);

  static_assert(Patch::kSize == SharedMemoryABI::kPacketHeaderSize,
                "Patch::kSize out of sync with SharedMemoryABI");

  for (size_t i = 0; i < patches_size; i++) {
    const size_t offset_untrusted = patches[i].offset_untrusted;
    if (payload_size < Patch::kSize ||
        offset_untrusted > payload_size - Patch::kSize) {
      // Either the IPC was so slow and in the meantime the writer managed to
      // wrap over |chunk_id| or the producer sent a malicious IPC.
      stats_.set_patches_failed(stats_.patches_failed() + 1);
      return false;
    }
    PERFETTO_DCHECK(offset_untrusted >= payload_size - chunk->payload_avail);
    TRACE_BUFFER_DLOG("PatchChunk {%" PRIu32 ",%" PRIu32
                      ",%u} size=%zu @ %zu with {%02x %02x %02x %02x}",
                      producer_id, writer_id, chunk_id,
                      size_t(chunk->payload_size), offset_untrusted,
                      patches[i].data[0], patches[i].data[1],
                      patches[i].data[2], patches[i].data[3]);
    uint8_t* dst = chunk->fragments_begin() + offset_untrusted;
    memcpy(dst, &patches[i].data[0], Patch::kSize);
  }
  TRACE_BUFFER_DLOG(
      "Chunk raw (after patch): %s",
      base::HexDump(chunk->fragments_begin(), chunk->payload_size).c_str());
  stats_.set_patches_succeeded(stats_.patches_succeeded() + patches_size);
  if (!other_patches_pending) {
    chunk->flags &= ~kChunkNeedsPatching;
  }
  return true;
}

void TraceBufferV2::DiscardWrite() {
  PERFETTO_DCHECK(overwrite_policy_ == kDiscard);
  discard_writes_ = true;
  stats_.set_chunks_discarded(stats_.chunks_discarded() + 1);
  TRACE_BUFFER_DLOG("  discarding write");
}

void TraceBufferV2::DumpForTesting() {
  if ((!TRACE_BUFFER_VERBOSE_LOGGING()))
    return;
  PERFETTO_DLOG(
      "------------------- DUMP BEGIN ------------------------------");
  PERFETTO_DLOG("wr=%zu, size=%zu, used_size=%zu", wr_, size_, used_size_);
  if (rd_iter_.valid()) {
    PERFETTO_DLOG("rd=%zu, target=%zu, seq=%d", OffsetOf(rd_iter_.chunk()),
                  OffsetOf(rd_iter_.end_chunk()), !!rd_iter_.sequence_state());
  } else {
    PERFETTO_DLOG("rd=invalid");
  }
  for (size_t rd = 0; rd < size_;) {
    TBChunk* c = GetTBChunkAtUnchecked(rd);
    bool checksum_valid = c->IsChecksumValid(rd);
    if (checksum_valid) {
      PERFETTO_DLOG(
          "[%06zu-%06zu] size=%05u(%05u) id=%05u pr_wr=%08x flags=%08x", rd,
          rd + c->outer_size(), c->payload_size,
          c->payload_size - c->payload_avail, c->chunk_id, c->pri_wri_id,
          c->flags);
      rd += c->outer_size();
      continue;
    }
    size_t zero_start = rd;
    // Count zeros.
    for (; rd < size_ && begin()[rd] == 0; rd++) {
    }
    PERFETTO_DLOG("%zu zeros, %zu left", rd - zero_start, size_ - rd);
    break;
  }
  PERFETTO_DLOG("------------------------------------------------------------");
}

}  // namespace perfetto

// TODO delete SequenceState once its empty?

// TODO add a test to check that we fixed the "previous_packet_dropped" cases.