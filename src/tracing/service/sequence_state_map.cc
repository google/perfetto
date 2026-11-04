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

#include "src/tracing/service/sequence_state_map.h"

#include <algorithm>
#include <new>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/tracing/core/tracing_service.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

using DataLossReason = protos::pbzero::TracePacket_DataLossReason;

namespace internal {

// +---------------------------------------------------------------------------+
// | SequenceState                                                             |
// +---------------------------------------------------------------------------+

SequenceState::SequenceState(const TraceBuffer::PacketSequenceProperties& props,
                             ProtocolAbiVersion abi)
    : properties(props),
      chunks(/*initial_capacity=*/64),
      is_smb_v2(abi == kProtocolAbiV2) {}

SequenceState::SequenceState(const SequenceState&) noexcept = default;

SequenceState::~SequenceState() {
  PERFETTO_CHECK(lease_count_ == 0);
}

void SequenceState::AddDataLoss(uint32_t reasons) {
  PERFETTO_DCHECK(reasons != 0);
  // DATA_LOSS_PRESENT is always set so any nonzero value reads as "dropped".
  data_loss_reasons_ |= DataLossReason::DATA_LOSS_PRESENT | reasons;
}

void SequenceState::AddBundleLoss(uint32_t reasons) {
  PERFETTO_DCHECK(reasons != 0);
  bundle_loss_reasons_ |= DataLossReason::DATA_LOSS_PRESENT | reasons;
}

SequenceState::Lease SequenceState::AcquireLease() {
  ++lease_count_;
  return Lease(this);
}

void SequenceState::ReleaseLease() {
  PERFETTO_CHECK(lease_count_ > 0);
  --lease_count_;
  if (lease_count_ == 0) {
    // No bundle is left, so this writer's next packet comes from its chunks.
    data_loss_reasons_ |= std::exchange(bundle_loss_reasons_, 0u);
  }
}

SequenceState::Lease::~Lease() {
  if (state_)
    state_->ReleaseLease();
}

SequenceState::Lease::Lease(Lease&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

SequenceState::Lease& SequenceState::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    this->~Lease();
    new (this) Lease(std::move(other));
  }
  return *this;
}

// +---------------------------------------------------------------------------+
// | SequenceStateMap |
// +---------------------------------------------------------------------------+

SequenceStateMap::SequenceStateMap() = default;
SequenceStateMap::SequenceStateMap(const SequenceStateMap& src)
    : sequences_(src.sequences_),
      inserts_since_prune_(src.inserts_since_prune_),
      use_counter_(src.use_counter_) {
  for (auto& [key, sequence] : sequences_)
    sequence.lease_count_ = 0;
}

SequenceStateMap::~SequenceStateMap() = default;

// When a sequence has 0 chunks and no leases we could delete it straight away.
// However doing do would remove also the last_chunk_consumed used to detect
// data losses.
// Here we have to balance the tradeoff between:
// - Bounding memory usage: if we have a lot of writer threads, we can't just
//   keeping growing the SequenceStates without bounds.
// - Retaining our ability to detect data losses for sporadic writers.
// Here the decision is to keep the last used 1024 (kPrunableSequencesToKeep)
// prunable sequences and delete anything older. We have some hysteresis and
// prune only after 256 (kInsertsPerPrune) new sequences since the last prune.
// This is to avoid thrashing with a prune every time a new sequence arrives.
bool SequenceStateMap::PruneInternal() {
  // Wait for more new sequences before looking again.
  inserts_since_prune_ = 0;

  if (sequences_.size() <= kPrunableSequencesToKeep)
    return false;

  // Build a vector of iterators; sort it; delete the first
  // size() - kPrunableSequencesToKeep.
  using SeqIterator = decltype(sequences_)::iterator;
  std::vector<SeqIterator> prunable_seqs;
  for (auto it = sequences_.begin(); it != sequences_.end(); ++it) {
    if (it->second.CanBePruned())
      prunable_seqs.emplace_back(it);
  }

  if (prunable_seqs.size() <= kPrunableSequencesToKeep)
    return false;

  std::sort(prunable_seqs.begin(), prunable_seqs.end(),
            [](const SeqIterator& a, const SeqIterator& b) {
              return a->second.last_use_ < b->second.last_use_;
            });

  // TODO(sashwinbalaji): Pruning an SMB v2 sequence discards its
  // pending_chunk_v2_data_loss. If that writer appends again, the loss is not
  // reported. SMB sequences have the same limit: pruning discards
  // last_chunk_consumed. Revisit if field traces show missed loss for writers
  // that write rarely while many others churn.
  const size_t n_oldest = prunable_seqs.size() - kPrunableSequencesToKeep;
  for (size_t i = 0; i < n_oldest; ++i) {
    sequences_.erase(prunable_seqs[i]);
  }
  return true;
}

}  // namespace internal
}  // namespace perfetto
