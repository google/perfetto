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

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace internal {
namespace {

using DataLossReason = protos::pbzero::TracePacket_DataLossReason;

// Enough new sequences to start a prune, in a new map or after another prune.
constexpr ProducerID kManySequences = 2000;

constexpr WriterID kWriter = 1;

constexpr uint32_t kGapLoss =
    DataLossReason::DATA_LOSS_PRESENT | DataLossReason::DATA_LOSS_READ_GAP;
constexpr uint32_t kOverwriteLoss =
    DataLossReason::DATA_LOSS_PRESENT | DataLossReason::DATA_LOSS_OVERWRITE;

ProducerAndWriterID Key(ProducerID producer) {
  return MkProducerAndWriterID(producer, kWriter);
}

SequenceState* InsertSeq(SequenceStateMap* states, ProducerID producer) {
  return states->FindOrInsert(
      Key(producer), {producer, ClientIdentity(), kWriter}, kProtocolAbiV1);
}

// This doesn't use Find(), because a lookup would count as a use.
std::vector<ProducerID> SortedProducers(SequenceStateMap* states) {
  std::vector<ProducerID> producers;
  for (const auto& seq_entry : *states)
    producers.push_back(seq_entry.second.properties.producer_id_trusted);
  std::sort(producers.begin(), producers.end());
  return producers;
}

size_t Size(SequenceStateMap* states) {
  return static_cast<size_t>(std::distance(states->begin(), states->end()));
}

TEST(SequenceStateMapTest, PruneOnlyAfterEnoughInserts) {
  SequenceStateMap states;
  ProducerID next_producer = 1;
  auto insert_seqs = [&](size_t count) {
    for (size_t i = 0; i < count; i++)
      InsertSeq(&states, next_producer++);
  };

  // 1280 new sequences start a prune, which keeps 1024 empty sequences.
  insert_seqs(1280);
  EXPECT_TRUE(states.MaybePrune());
  EXPECT_EQ(Size(&states), 1024u);

  // 256 new sequences start the next prune.
  // It deletes nothing because only 1024 sequences are empty.
  const ProducerID first_with_chunk = next_producer;
  std::vector<SequenceState*> seqs;
  for (size_t i = 0; i < 256; i++) {
    seqs.push_back(InsertSeq(&states, next_producer++));
    seqs.back()->chunks.emplace_back(0u);
  }
  EXPECT_FALSE(states.MaybePrune());
  EXPECT_EQ(Size(&states), 1280u);

  // More empty sequences don't start a prune, because nothing new was added.
  for (size_t i = 1; i < seqs.size(); i++)
    seqs[i]->chunks.pop_front();
  EXPECT_FALSE(states.MaybePrune());
  EXPECT_EQ(Size(&states), 1280u);

  // Neither do lookups of sequences that already exist.
  for (size_t i = 0; i < 256; i++) {
    EXPECT_EQ(states.Find(Key(first_with_chunk)), seqs[0]);
    EXPECT_EQ(InsertSeq(&states, first_with_chunk), seqs[0]);
  }
  EXPECT_FALSE(states.MaybePrune());
  EXPECT_EQ(Size(&states), 1280u);

  // 255 new sequences aren't enough, but one more starts a prune.
  // It keeps 1024 empty sequences and the one with a chunk.
  insert_seqs(255);
  EXPECT_FALSE(states.MaybePrune());
  EXPECT_EQ(Size(&states), 1535u);
  insert_seqs(1);
  EXPECT_TRUE(states.MaybePrune());
  EXPECT_EQ(Size(&states), 1025u);
  EXPECT_EQ(states.Find(Key(first_with_chunk)), seqs[0]);
}

TEST(SequenceStateMapTest, PruneDeletesLeastRecentlyUsedFirst) {
  SequenceStateMap states;
  SequenceState* first_seq = InsertSeq(&states, 1);
  SequenceState* second_seq = InsertSeq(&states, 2);
  for (ProducerID producer = 3; producer <= 1025; producer++)
    InsertSeq(&states, producer);

  // Sequences 1 and 2 were inserted first, but these make them the last used.
  EXPECT_EQ(states.Find(Key(1)), first_seq);
  EXPECT_EQ(InsertSeq(&states, 2), second_seq);
  EXPECT_TRUE(states.MaybePrune());

  // One sequence is over the limit, so only sequence 3, now the oldest, goes.
  std::vector<ProducerID> expected = {1, 2};
  for (ProducerID producer = 4; producer <= 1025; producer++)
    expected.push_back(producer);
  EXPECT_EQ(SortedProducers(&states), expected);

  // Pointers to the sequences that stay are still valid.
  EXPECT_EQ(states.Find(Key(1)), first_seq);
}

TEST(SequenceStateMapTest, LeaseKeepsSequenceThroughPrune) {
  SequenceStateMap states;
  SequenceState* seq = InsertSeq(&states, 1);
  SequenceState::Lease lease = seq->AcquireLease();

  // Sequence 1 has no chunks and was used before all the others.
  for (ProducerID producer = 2; producer <= kManySequences; producer++)
    InsertSeq(&states, producer);
  ASSERT_TRUE(states.MaybePrune());
  EXPECT_EQ(states.Find(Key(1)), seq);

  // After the lease ends, the next prune deletes it like any old sequence.
  lease = SequenceState::Lease();
  constexpr ProducerID kLastProducer = 2 * kManySequences;
  for (ProducerID producer = kManySequences + 1; producer <= kLastProducer;
       producer++) {
    InsertSeq(&states, producer);
  }
  ASSERT_TRUE(states.MaybePrune());
  EXPECT_EQ(states.Find(Key(1)), nullptr);
}

TEST(SequenceStateMapTest, TakeDataLossReturnsAddedLossAndClearsIt) {
  SequenceStateMap states;
  SequenceState* seq = InsertSeq(&states, 1);
  EXPECT_EQ(seq->TakeDataLoss(), 0u);

  seq->AddDataLoss(DataLossReason::DATA_LOSS_READ_GAP);
  seq->AddDataLoss(DataLossReason::DATA_LOSS_OVERWRITE);
  EXPECT_EQ(seq->TakeDataLoss(),
            static_cast<uint32_t>(DataLossReason::DATA_LOSS_PRESENT |
                                  DataLossReason::DATA_LOSS_READ_GAP |
                                  DataLossReason::DATA_LOSS_OVERWRITE));
  EXPECT_EQ(seq->TakeDataLoss(), 0u);
}

TEST(SequenceStateMapTest, TakeBundleLossReturnsAddedLossAndClearsIt) {
  SequenceStateMap states;
  SequenceState* seq = InsertSeq(&states, 1);
  SequenceState::Lease lease = seq->AcquireLease();
  EXPECT_EQ(seq->TakeBundleLoss(), 0u);

  seq->AddBundleLoss(DataLossReason::DATA_LOSS_OVERWRITE);
  EXPECT_EQ(seq->TakeDataLoss(), 0u);
  EXPECT_EQ(seq->TakeBundleLoss(), kOverwriteLoss);
  EXPECT_EQ(seq->TakeBundleLoss(), 0u);
}

TEST(SequenceStateMapTest, LastLeaseMovesBundleLossToDataLoss) {
  SequenceStateMap states;
  SequenceState* seq = InsertSeq(&states, 1);
  SequenceState::Lease first = seq->AcquireLease();
  SequenceState::Lease second = seq->AcquireLease();
  seq->AddBundleLoss(DataLossReason::DATA_LOSS_OVERWRITE);

  // A lease remains, so the bundle loss keeps waiting.
  first = SequenceState::Lease();
  EXPECT_EQ(seq->TakeDataLoss(), 0u);

  // The last lease adds it to any loss that is already pending.
  seq->AddDataLoss(DataLossReason::DATA_LOSS_READ_GAP);
  second = SequenceState::Lease();
  EXPECT_EQ(seq->TakeBundleLoss(), 0u);
  EXPECT_EQ(seq->TakeDataLoss(), kGapLoss | kOverwriteLoss);
}

// A moved-from lease is empty, so it ends nothing when it goes away.
TEST(SequenceStateMapTest, MovedLeaseKeepsSequence) {
  SequenceStateMap states;
  SequenceState* seq = InsertSeq(&states, 1);
  SequenceState::Lease assigned;
  {
    SequenceState::Lease lease = seq->AcquireLease();
    seq->AddBundleLoss(DataLossReason::DATA_LOSS_OVERWRITE);
    SequenceState::Lease constructed = std::move(lease);
    assigned = std::move(constructed);
  }
  EXPECT_EQ(&*assigned, seq);
  EXPECT_EQ(seq->TakeDataLoss(), 0u);

  assigned = SequenceState::Lease();
  EXPECT_EQ(seq->TakeDataLoss(), kOverwriteLoss);
}

TEST(SequenceStateMapTest, CopyHasNoLeases) {
  SequenceStateMap src;
  SequenceState* src_seq = InsertSeq(&src, 1);
  SequenceState::Lease src_lease = src_seq->AcquireLease();
  src_seq->AddBundleLoss(DataLossReason::DATA_LOSS_OVERWRITE);

  SequenceStateMap copy(src);
  SequenceState* copy_seq = copy.Find(Key(1));
  ASSERT_NE(copy_seq, nullptr);

  // A lease on the copy is its only one, so ending it moves the copy's loss.
  // The source's loss keeps waiting for the source's lease.
  {
    SequenceState::Lease copy_lease = copy_seq->AcquireLease();
  }
  EXPECT_EQ(copy_seq->TakeDataLoss(), kOverwriteLoss);
  EXPECT_EQ(src_seq->TakeDataLoss(), 0u);
  EXPECT_EQ(src_seq->TakeBundleLoss(), kOverwriteLoss);

  // The source's lease doesn't keep the copy's sequence through a prune.
  for (ProducerID producer = 2; producer <= kManySequences; producer++)
    InsertSeq(&copy, producer);
  ASSERT_TRUE(copy.MaybePrune());
  EXPECT_EQ(copy.Find(Key(1)), nullptr);
  EXPECT_EQ(src.Find(Key(1)), src_seq);
}

}  // namespace
}  // namespace internal
}  // namespace perfetto
