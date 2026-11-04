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

#ifndef SRC_TRACING_SERVICE_SEQUENCE_STATE_MAP_H_
#define SRC_TRACING_SERVICE_SEQUENCE_STATE_MAP_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <unordered_map>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "src/tracing/service/trace_buffer.h"

namespace perfetto {

enum ProtocolAbiVersion : uint32_t;

namespace internal {

// +---------------------------------------------------------------------------+
// | SequenceState                                                             |
// +---------------------------------------------------------------------------+
// Holds the state for each sequence with TBCHunk(s) in the buffer or leases.
// Remember that this class must be copyable for CloneReadOnly(). Don't hold
// onto any pointers in here.
// SequenceState(s) are not deleted aggressively to preserve the
// last_chunk_consumed and detect data losses in long tracing mode. We allow
// the last kPrunableSequencesToKeep to stay alive to balance data loss
// detection with memory bloats.
class SequenceState {
 public:
  // Protects a SequenceState from pruning while the lease is alive.
  // SequenceState::AcquireLease() is the only way to get a lease on a sequence.
  // The map owns the state, so destroy the lease before the map.
  class Lease {
   public:
    Lease() = default;
    ~Lease();
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

    SequenceState& operator*() const { return *state_; }
    SequenceState* operator->() const { return state_; }

   private:
    friend class SequenceState;
    explicit Lease(SequenceState* state) : state_(state) {}

    SequenceState* state_ = nullptr;
  };

  SequenceState(const TraceBuffer::PacketSequenceProperties&,
                ProtocolAbiVersion);

  SequenceState(const SequenceState&) noexcept;
  SequenceState& operator=(const SequenceState&) = delete;
  ~SequenceState();

  // Adds nonzero |reasons| to the |data_loss_reasons_| bitmask.
  void AddDataLoss(uint32_t reasons);

  // Returns the |data_loss_reasons_| bitmask and clears it.
  uint32_t TakeDataLoss() { return std::exchange(data_loss_reasons_, 0u); }

  // Keeps this sequence until the lease ends.
  PERFETTO_WARN_UNUSED_RESULT Lease AcquireLease();

  // Adds nonzero |reasons| to the |bundle_loss_reasons_| bitmask.
  void AddBundleLoss(uint32_t reasons);

  // Returns the |bundle_loss_reasons_| bitmask and clears it.
  uint32_t TakeBundleLoss() { return std::exchange(bundle_loss_reasons_, 0u); }

  const TraceBuffer::PacketSequenceProperties properties;

  // This is semantically a boolean that resets every time BeginRead()
  // increments the generation counter. The semantic is:
  // skip := skip_in_generation == TraceBuffer.read_generation_.
  uint64_t skip_in_generation = 0;

  // Snapshot of the last chunk that was erased (consumed or evicted) in this
  // sequence. Used to detect gaps between consecutive chunks and to decide
  // whether a re-committed chunk should be accepted or discarded.
  // |payload_size| and |was_incomplete| capture the chunk's state at the time
  // of consumption.
  struct ConsumedChunkInfo {
    ChunkID chunk_id = 0;
    uint16_t payload_size = 0;
    bool was_incomplete = false;
  };
  std::optional<ConsumedChunkInfo> last_chunk_consumed;

  // An ordered list of chunk offsets, sorted by ChunkID for SMB sequences or
  // by append order for SMB v2 sequences.
  // We store buffer offsets rather than pointers to make buffer cloning easier.
  base::CircularQueue<size_t> chunks;

  // All chunks of a sequence use the same format: either the legacy SMB v1 or
  // the newer proto-group based SMB v2. SMB v2 chunks are immutable, arrive in
  // order and need rewriting at readback.
  const bool is_smb_v2;

  // A gap after the last appended SMB v2 chunk, not yet stamped on a
  // chunk.
  // - SMB sequences need no such state. A gap shows up as a non-successor
  //   ChunkID, so the reader finds it at the right position by itself.
  // - SMB v2 chunks have no ChunkID. The gap must be recorded when it
  //   happens, at the position between two chunks.
  // - data_loss_reasons_ cannot hold it. The reader reports that on the next
  //   packet it reads, which can be an older packet buffered before the gap.
  // The next append moves this flag to its chunk. The
  // reader then reports the loss on the first packet after the gap.
  //
  // Example for one writer:
  //
  //   Step                      Buffer        pending_chunk_v2_data_loss
  //   Append(A)                 [A]           false
  //   RecordChunkV2DataLoss()   [A]           true
  //   Append(C)                 [A] [C*]      false, moved to C as *
  //
  //   * = C carries the gap. A read returns A with no loss, then C with
  //   DATA_LOSS_READ_GAP.
  bool pending_chunk_v2_data_loss = false;

 private:
  friend class SequenceStateMap;

  bool CanBePruned() const { return chunks.empty() && lease_count_ == 0; }

  void ReleaseLease();

  // Pending DataLossReason bitmask, OR-ed until TakeDataLoss() resets it.
  // Every nonzero value has DATA_LOSS_PRESENT set.
  uint32_t data_loss_reasons_ = 0;

  // Loss from stored bundles, reported by this sequence's next decoded bundle.
  // The last lease moves it to |data_loss_reasons_| if no bundle is left.
  uint32_t bundle_loss_reasons_ = 0;

  // Number of live leases.
  uint32_t lease_count_ = 0;

  // Used by SequenceStateMap::MaybePrune() to keep the latest
  // kPrunableSequencesToKeep objects around.
  // Set from SequenceStateMap::use_counter_.
  uint64_t last_use_ = 0;
};

// +---------------------------------------------------------------------------+
// | SequenceStateMap |
// +---------------------------------------------------------------------------+
// Tracks every sequence of a trace buffer.
// A sequence stays while it has chunks or leases and is prunable when it has
// neither.
// A SequenceState pointer stays valid until MaybePrune() deletes that sequence.
class SequenceStateMap {
 public:
  SequenceStateMap();
  // A copy has no leases, since each lease points at a sequence in the source.
  explicit SequenceStateMap(const SequenceStateMap&);
  SequenceStateMap& operator=(const SequenceStateMap&) = delete;
  ~SequenceStateMap();

  // Returns the sequence if present otherwise nullptr and updates the
  // last_use_ counter.
  SequenceState* Find(ProducerAndWriterID key) {
    const auto it = sequences_.find(key);
    if (PERFETTO_UNLIKELY(it == sequences_.end()))
      return nullptr;
    it->second.last_use_ = ++use_counter_;
    return &it->second;
  }

  // Same as Find(), for a sequence that must be present.
  SequenceState& FindOrDie(ProducerAndWriterID key) {
    SequenceState* sequence = Find(key);
    PERFETTO_CHECK(sequence);
    return *sequence;
  }

  // Returns the sequence, adding it with no chunks and no leases if it is new
  // and updates the last_use_ counter.
  SequenceState* FindOrInsert(
      ProducerAndWriterID key,
      const TraceBuffer::PacketSequenceProperties& properties,
      ProtocolAbiVersion abi) {
    const auto [it, inserted] = sequences_.try_emplace(key, properties, abi);
    if (inserted)
      ++inserts_since_prune_;
    it->second.last_use_ = ++use_counter_;
    return &it->second;
  }

  // Deletes the least recently used prunable sequences, once enough new
  // sequences have been added since the last prune.
  // Returns true if it deleted any, invalidating pointers to those sequences.
  bool MaybePrune() {
    if (inserts_since_prune_ < kInsertsPerPrune)
      return false;
    return PruneInternal();
  }

  auto begin() { return sequences_.begin(); }
  auto end() { return sequences_.end(); }

 private:
  bool PruneInternal();

  // The number of the latest prunable SequenceState(s) to keep around.
  static constexpr size_t kPrunableSequencesToKeep = 1024;

  // The number of new sequences to add between prunes.
  static constexpr size_t kInsertsPerPrune = 256;

  // All the sequences, by producer and writer ID.
  // We use std::unordered_map rather than base::FlatHashMap because callers
  // keep SequenceState pointers, so inserts must not move them.
  std::unordered_map<ProducerAndWriterID, SequenceState> sequences_;

  // The number of new sequences added since the last prune.
  size_t inserts_since_prune_ = 0;

  // A monotonic counter incremented every time a SequenceState is looked up or
  // created.
  // This is used to sort SequenceState by least-recently used.
  uint64_t use_counter_ = 0;
};

}  // namespace internal
}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_SEQUENCE_STATE_MAP_H_
