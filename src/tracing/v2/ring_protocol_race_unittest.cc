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

// The three two-party races, run for real on two threads, plus an
// MPSC stress test that checks the end-to-end guarantee: every fragment that
// was published comes out exactly once, in the order its writer wrote it,
// except for the ones the writer itself accounted as dropped.
//
// These are the tests worth running under ThreadSanitizer. Nothing here uses a
// sleep to make a race likely: the threads meet on a spin barrier, and each
// iteration asserts that exactly one of the two contenders won.

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/chunk_reader.h"
#include "src/tracing/v2/ring_writer.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/tracing_v2_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

constexpr WriterID kWriterA = 7;
constexpr WriterID kWriterB = 8;
constexpr BufferID kBuffer = 5;

uint32_t AcquiredWord(WriterID writer) {
  return MakeDataBearingWord(ChunkState::kAcquired, ChunkFormat::kTargetBuffer,
                             0, 0, writer);
}

// Sequences two threads through a fixed schedule. Each round has three steps:
// the reader thread prepares the chunk, then the two contenders take their
// compare-and-swap turns in an order that alternates every round. That makes
// *both* winners of a two-party race reachable on demand instead of depending
// on which thread happens to leave a barrier first - which, with a real
// barrier, is always the same one.
//
// The atomics under test still cross a genuine thread boundary, which is what
// makes these worth running under ThreadSanitizer.
class StepSequencer {
 public:
  static constexpr uint32_t kStepsPerRound = 3;

  void WaitForStep(uint64_t step) const {
    while (step_.load(std::memory_order_acquire) != step)
      std::this_thread::yield();
  }
  void FinishStep(uint64_t step) {
    step_.store(step + 1, std::memory_order_release);
  }

  // Step at which |round|'s preparation happens.
  static uint64_t PrepareStep(uint32_t round) {
    return uint64_t{round} * kStepsPerRound;
  }
  // Step at which the reader or the writer takes its turn in |round|. The
  // reader goes first on even rounds and second on odd ones.
  static uint64_t ReaderStep(uint32_t round) {
    return PrepareStep(round) + (round % 2 == 0 ? 1 : 2);
  }
  static uint64_t WriterStep(uint32_t round) {
    return PrepareStep(round) + (round % 2 == 0 ? 2 : 1);
  }

 private:
  std::atomic<uint64_t> step_{0};
};

// ---------------------------------------------------------------------------
// Race 1: a writer claims while the reader consumes an unclaimed position.
// Both compare against FreeForWrap(wrap_count(p)).
// ---------------------------------------------------------------------------

TEST(RingProtocolRaceTest, ClaimVersusUnclaimedAdvance) {
  constexpr uint32_t kRounds = 1000;
  constexpr uint32_t kNumChunks = 2;
  auto ring = SharedRingBuffer::Create(kNumChunks, 256);
  ASSERT_NE(ring, nullptr);

  StepSequencer sequencer;
  std::atomic<uint32_t> claims_won{0};
  std::atomic<uint32_t> advances_won{0};
  // Round r resolves position r, so which physical chunk and which wrap count
  // are in play changes every round.
  std::thread writer([&] {
    for (uint32_t round = 0; round < kRounds; ++round) {
      const uint64_t step = StepSequencer::WriterStep(round);
      sequencer.WaitForStep(step);
      if (ring->TryClaim(round, AcquiredWord(kWriterA)))
        claims_won.fetch_add(1, std::memory_order_relaxed);
      sequencer.FinishStep(step);
    }
  });

  for (uint32_t round = 0; round < kRounds; ++round) {
    const uint32_t position = round;
    const uint32_t chunk_index = ChunkIndexOfPosition(position, kNumChunks);

    sequencer.WaitForStep(StepSequencer::PrepareStep(round));
    uint32_t observed = ring->LoadStateAcquire(chunk_index);
    ASSERT_EQ(StateOf(observed), ChunkState::kFreeForWrap) << round;
    sequencer.FinishStep(StepSequencer::PrepareStep(round));

    const uint64_t step = StepSequencer::ReaderStep(round);
    sequencer.WaitForStep(step);
    const bool advanced = StateOf(observed) == ChunkState::kFreeForWrap &&
                          ring->TryAdvanceUnclaimed(position, &observed);
    if (advanced)
      advances_won.fetch_add(1, std::memory_order_relaxed);
    sequencer.FinishStep(step);

    sequencer.WaitForStep(StepSequencer::PrepareStep(round + 1));
    const uint32_t after = ring->LoadStateAcquire(chunk_index);
    if (advanced) {
      // The reader consumed the hole and prepared the chunk for the writer
      // holding position + num_chunks. The writer that lost holds |position|
      // and has spent its one claim attempt.
      ASSERT_EQ(StateOf(after), ChunkState::kFreeForWrap) << round;
      ASSERT_EQ(WrapCountOf(after),
                NextWrapCount(position, kNumChunks, ring->chunk_bits()))
          << round;
      ASSERT_FALSE(ring->TryClaim(position, AcquiredWord(kWriterB))) << round;
    } else {
      ASSERT_EQ(StateOf(after), ChunkState::kAcquired) << round;
      // Put the chunk back the way the writer's publication and the reader's
      // reclaim would, so the next round starts from a free word again.
      uint32_t acquired = AcquiredWord(kWriterA);
      const uint32_t complete = MakeDataBearingWord(
          ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0, 0, kWriterA);
      ASSERT_TRUE(ring->TryPublish(chunk_index, &acquired, complete)) << round;
      uint32_t to_reclaim = complete;
      ASSERT_TRUE(ring->TryReclaimComplete(position, &to_reclaim)) << round;
    }
  }
  writer.join();

  // Exactly one contender wins every round, and the schedule reaches both.
  EXPECT_EQ(claims_won.load() + advances_won.load(), kRounds);
  EXPECT_EQ(claims_won.load(), kRounds / 2);
  EXPECT_EQ(advances_won.load(), kRounds / 2);
}

// ---------------------------------------------------------------------------
// Race 2: a writer publishes while the reader scrapes.
// Both compare against Acquired(w,n).
// ---------------------------------------------------------------------------

TEST(RingProtocolRaceTest, PublishVersusScrape) {
  constexpr uint32_t kRounds = 1000;
  auto ring = SharedRingBuffer::Create(2, 256);
  ASSERT_NE(ring, nullptr);

  StepSequencer sequencer;
  std::atomic<uint32_t> publishes_won{0};
  std::atomic<uint32_t> scrapes_won{0};
  const uint32_t kComplete = MakeDataBearingWord(
      ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0, 1, kWriterA);

  std::thread writer([&] {
    for (uint32_t round = 0; round < kRounds; ++round) {
      const uint64_t step = StepSequencer::WriterStep(round);
      sequencer.WaitForStep(step);
      uint32_t observed = AcquiredWord(kWriterA);
      if (ring->TryPublish(0, &observed, kComplete)) {
        publishes_won.fetch_add(1, std::memory_order_relaxed);
      } else {
        // The reader marking the chunk is the only legal way to lose, and the
        // marked count says exactly which prefix it took.
        EXPECT_EQ(StateOf(observed), ChunkState::kRewriteRequested);
        EXPECT_EQ(WriterIdOf(observed), kWriterA);
        EXPECT_EQ(NumFragmentsOf(observed), 0u);
        EXPECT_TRUE(ring->TryAcknowledge(0, observed));
      }
      sequencer.FinishStep(step);
    }
  });

  for (uint32_t round = 0; round < kRounds; ++round) {
    sequencer.WaitForStep(StepSequencer::PrepareStep(round));
    ring->SetStateWordForTesting(0, AcquiredWord(kWriterA));
    uint32_t observed = ring->LoadStateAcquire(0);
    sequencer.FinishStep(StepSequencer::PrepareStep(round));

    const uint64_t step = StepSequencer::ReaderStep(round);
    sequencer.WaitForStep(step);
    const bool marked = ring->TryMarkForRewrite(0, &observed);
    if (marked) {
      scrapes_won.fetch_add(1, std::memory_order_relaxed);
    } else {
      // The writer completed, so the reader discards its speculative copy and
      // redispatches on a word that carries everything.
      ASSERT_EQ(StateOf(observed), ChunkState::kComplete) << round;
      ASSERT_EQ(NumFragmentsOf(observed), 1u) << round;
    }
    sequencer.FinishStep(step);

    sequencer.WaitForStep(StepSequencer::PrepareStep(round + 1));
    // A marked chunk comes back as Acknowledged: the writer released it and
    // said nothing about who gets it next.
    ASSERT_EQ(ring->LoadStateAcquire(0), marked ? kAcknowledgedWord : kComplete)
        << round;
  }
  writer.join();

  EXPECT_EQ(publishes_won.load() + scrapes_won.load(), kRounds);
  EXPECT_EQ(publishes_won.load(), kRounds / 2);
  EXPECT_EQ(scrapes_won.load(), kRounds / 2);
}

// ---------------------------------------------------------------------------
// Race 3: a writer reuses while the reader consumes.
// Both compare against Complete(w,n).
// ---------------------------------------------------------------------------

TEST(RingProtocolRaceTest, ReuseVersusReclaim) {
  constexpr uint32_t kRounds = 1000;
  constexpr uint32_t kNumChunks = 2;
  auto ring = SharedRingBuffer::Create(kNumChunks, 256);
  ASSERT_NE(ring, nullptr);

  StepSequencer sequencer;
  std::atomic<uint32_t> reuses_won{0};
  std::atomic<uint32_t> reclaims_won{0};
  const uint32_t kComplete = MakeDataBearingWord(
      ChunkState::kComplete, ChunkFormat::kTargetBuffer, 0, 1, kWriterA);

  std::thread writer([&] {
    for (uint32_t round = 0; round < kRounds; ++round) {
      const uint64_t step = StepSequencer::WriterStep(round);
      sequencer.WaitForStep(step);
      if (ring->TryReuse(0, kComplete))
        reuses_won.fetch_add(1, std::memory_order_relaxed);
      sequencer.FinishStep(step);
    }
  });

  for (uint32_t round = 0; round < kRounds; ++round) {
    sequencer.WaitForStep(StepSequencer::PrepareStep(round));
    ring->SetStateWordForTesting(0, kComplete);
    uint32_t observed = ring->LoadStateAcquire(0);
    sequencer.FinishStep(StepSequencer::PrepareStep(round));

    const uint64_t step = StepSequencer::ReaderStep(round);
    sequencer.WaitForStep(step);
    // Position 0 every round, so the reclaimed word is always FreeForWrap of
    // the wrap after position 0's.
    const bool reclaimed = ring->TryReclaimComplete(0, &observed);
    if (reclaimed) {
      reclaims_won.fetch_add(1, std::memory_order_relaxed);
    } else {
      // The writer took the chunk back; the reader handles that on its next
      // look, through the scrape path.
      ASSERT_EQ(StateOf(observed), ChunkState::kAcquired) << round;
      ASSERT_EQ(NumFragmentsOf(observed), 1u) << round;
    }
    sequencer.FinishStep(step);
    sequencer.WaitForStep(StepSequencer::PrepareStep(round + 1));
  }
  writer.join();

  EXPECT_EQ(reuses_won.load() + reclaims_won.load(), kRounds);
  EXPECT_EQ(reuses_won.load(), kRounds / 2);
  EXPECT_EQ(reclaims_won.load(), kRounds / 2);
}

// ---------------------------------------------------------------------------
// MPSC stress.
// ---------------------------------------------------------------------------

struct StressParams {
  uint32_t num_writers;
  uint32_t num_chunks;
  uint32_t chunk_size;
  uint32_t fragments_per_writer;
  uint32_t seed_position;
  BufferExhaustedPolicy policy;
};

// What a run actually did, so each test can assert that it exercised the path
// it is named after rather than passing because nothing happened.
struct StressStats {
  uint64_t received = 0;
  // The caller never got payload space for these.
  uint64_t unwritten = 0;
  // The reader scraped the chunk and there was no replacement capacity.
  uint64_t dropped = 0;
  // The reader scraped a chunk out from under a writer.
  uint64_t relocations = 0;
  // How many of the unwritten fragments were refused because the ring was
  // structurally full, as opposed to because a reserved position's chunk could
  // not be claimed. The two mean different things and a stalling policy is only
  // supposed to produce the second.
  uint64_t reported_full = 0;
};

class StressDelegate : public ChunkReader::Delegate {
 public:
  void OnChunkRead(const ChunkReader::ChunkContents& contents) override {
    for (uint32_t i = 0; i < contents.num_fragments; ++i) {
      const ChunkReader::Fragment& fragment = contents.fragments[i];
      ASSERT_GE(fragment.size, 8u);
      uint32_t writer = 0;
      uint32_t sequence = 0;
      memcpy(&writer, fragment.data, sizeof(writer));
      memcpy(&sequence, fragment.data + 4, sizeof(sequence));
      ASSERT_EQ(writer, contents.writer_id);
      received[writer].push_back(sequence);
    }
  }

  std::map<uint32_t, std::vector<uint32_t>> received;
};

void RunStress(const StressParams& params, StressStats* stats) {
  auto ring = SharedRingBuffer::Create(params.num_chunks, params.chunk_size);
  ASSERT_NE(ring, nullptr);
  ring->SetCursorsForTesting(params.seed_position);

  StressDelegate delegate;
  ChunkReader reader(ring.get(), &delegate);
  reader.SeekForTesting(params.seed_position);

  std::atomic<uint32_t> writers_done{0};
  std::vector<uint64_t> unwritten(params.num_writers, 0);
  std::vector<uint64_t> dropped(params.num_writers, 0);
  std::vector<uint64_t> relocations(params.num_writers, 0);
  std::vector<uint64_t> reported_full(params.num_writers, 0);
  std::vector<std::thread> writer_threads;

  for (uint32_t w = 0; w < params.num_writers; ++w) {
    writer_threads.emplace_back([&, w] {
      RingWriter writer(ring.get(), static_cast<WriterID>(w + 1), kBuffer,
                        params.policy);
      // A payload that varies in size so that chunk boundaries, reuse and the
      // 255-fragment cap all get exercised rather than one fixed shape.
      std::vector<uint8_t> payload(64);
      for (uint32_t n = 0; n < params.fragments_per_writer; ++n) {
        const uint32_t size = 8 + (n % 40);
        const uint32_t writer_tag = w + 1;
        memcpy(payload.data(), &writer_tag, sizeof(writer_tag));
        memcpy(payload.data() + 4, &n, sizeof(n));

        const RingWriter::FragmentSpan span = writer.OpenFragment(size, false);
        if (span.outcome != RingWriter::Outcome::kOk) {
          ++unwritten[w];
          if (span.outcome == RingWriter::Outcome::kFull)
            ++reported_full[w];
          writer.RecordDataLoss();
          std::this_thread::yield();
          continue;
        }
        memcpy(span.begin, payload.data(), size);
        // Yield while the chunk is still Acquired, so the reader lands inside
        // it and the scrape-and-relocate path is exercised for real. Yielding
        // at a protocol boundary is deliberate: a sleep would only make the
        // race likely, this makes it frequent.
        if ((n & 7) == 0)
          std::this_thread::yield();
        writer.CloseFragment(size, false);
      }
      writer.Release();
      dropped[w] = writer.num_fragments_dropped();
      relocations[w] = writer.num_relocations();
      writers_done.fetch_add(1, std::memory_order_release);
    });
  }

  // Drain until every writer has finished and the ring has been emptied.
  for (;;) {
    const bool all_done =
        writers_done.load(std::memory_order_acquire) == params.num_writers;
    const ChunkReader::DrainResult result = reader.Drain(64);
    if (result.last_outcome == ChunkReader::ReadOutcome::kProtocolError)
      break;
    if (all_done && !result.work_may_remain())
      break;
    if (result.positions_resolved == 0)
      std::this_thread::yield();
  }

  for (std::thread& thread : writer_threads)
    thread.join();
  ASSERT_FALSE(reader.stopped());
  reader.Drain(1u << 20);

  for (uint32_t w = 0; w < params.num_writers; ++w) {
    const std::vector<uint32_t>& sequences = delegate.received[w + 1];
    // Exactly once, and in the order the writer wrote them.
    for (size_t i = 1; i < sequences.size(); ++i) {
      ASSERT_LT(sequences[i - 1], sequences[i])
          << "writer " << w << " fragment " << i;
    }
    // Everything that is missing is something the writer itself accounted for.
    EXPECT_EQ(sequences.size() + unwritten[w] + dropped[w],
              params.fragments_per_writer)
        << "writer " << w;

    stats->received += sequences.size();
    stats->unwritten += unwritten[w];
    stats->dropped += dropped[w];
    stats->relocations += relocations[w];
    stats->reported_full += reported_full[w];
  }
}

// The reader only relocates when it lands inside a chunk a writer still owns,
// which is a scheduling outcome and not something these tests can force from
// the outside: roughly one run in a hundred sees none. Repeating a bounded
// number of times keeps them from failing on one unlucky schedule.
//
// This is a coverage check, not a proof. That relocation happens, and what it
// does to the payload and the flags, is settled deterministically by
// PublishVersusScrape above and by the reader and writer unit tests. All
// this asserts is that the stress ran with the contention it is named for.
constexpr uint32_t kAttemptsForRelocations = 8;

void RunStressUntilRelocations(const StressParams& params, StressStats* stats) {
  for (uint32_t attempt = 0; attempt < kAttemptsForRelocations; ++attempt) {
    RunStress(params, stats);
    if (stats->relocations > 0 || testing::Test::HasFatalFailure())
      return;
  }
}

TEST(RingProtocolRaceTest, StressFourWritersDropPolicy) {
  StressStats stats;
  RunStressUntilRelocations({/*num_writers=*/4, /*num_chunks=*/8,
                             /*chunk_size=*/256,
                             /*fragments_per_writer=*/4000, /*seed_position=*/0,
                             BufferExhaustedPolicy::kDrop},
                            &stats);
  // A drop policy on a small ring loses a lot, which is the point - but the
  // exactly-once and ordering checks above are worthless if nothing got
  // through. An absolute floor rather than a share of the total: how much a
  // drop-policy run keeps depends on how fast the reader is scheduled, and
  // under ThreadSanitizer that is a different number. What has to hold is that
  // the per-writer checks ran on real data, not that the ring achieved a
  // particular throughput.
  EXPECT_GT(stats.received, 100u);
  EXPECT_GT(stats.relocations, 0u);
}

TEST(RingProtocolRaceTest, StressTinyRingForcesHolesAndRelocations) {
  StressStats stats;
  RunStressUntilRelocations({/*num_writers=*/4, /*num_chunks=*/2,
                             /*chunk_size=*/256,
                             /*fragments_per_writer=*/2000, /*seed_position=*/0,
                             BufferExhaustedPolicy::kDrop},
                            &stats);
  EXPECT_GT(stats.received, 0u);
  // Two chunks and four writers is the shape that makes the reader land inside
  // a live writer's chunk constantly.
  EXPECT_GT(stats.relocations, 0u);
}

TEST(RingProtocolRaceTest, StressAcrossTheCursorRollover) {
  // Seeded so the 32-bit cursors roll over part way through, which is where a
  // reader that incremented the wrap it found in the chunk would lock writers
  // out.
  StressStats stats;
  RunStress({/*num_writers=*/4, /*num_chunks=*/16, /*chunk_size=*/512,
             /*fragments_per_writer=*/3000,
             /*seed_position=*/0xffffe000u, BufferExhaustedPolicy::kDrop},
            &stats);
  EXPECT_GT(stats.received, 0u);
}

TEST(RingProtocolRaceTest, StressStallThenDropPolicy) {
  StressStats stats;
  RunStress({/*num_writers=*/3, /*num_chunks=*/4, /*chunk_size=*/1024,
             /*fragments_per_writer=*/2000, /*seed_position=*/0,
             BufferExhaustedPolicy::kStallThenDrop},
            &stats);
  EXPECT_GT(stats.received, 0u);
}

TEST(RingProtocolRaceTest, StressStallPolicyNeverReportsFull) {
  StressStats stats;
  RunStress({/*num_writers=*/3, /*num_chunks=*/8, /*chunk_size=*/1024,
             /*fragments_per_writer=*/2000, /*seed_position=*/0,
             BufferExhaustedPolicy::kStall},
            &stats);
  EXPECT_GT(stats.received, 0u);
#if PERFETTO_TRACING_V2_HAS_FUTEX()
  // A stalling writer waits for the reader to expose capacity, so a full ring
  // never turns into a refusal. What it *can* still hit is a run of positions
  // whose chunks are pinned by writers the reader has marked for rewrite: that
  // is NoChunkAvailable, not Full, and the two are different events. Whatever
  // is lost that way is accounted for exactly, which the per-writer check
  // inside RunStress() has already asserted.
  EXPECT_EQ(stats.reported_full, 0u);
#endif
}

}  // namespace
}  // namespace perfetto::tracing_v2
