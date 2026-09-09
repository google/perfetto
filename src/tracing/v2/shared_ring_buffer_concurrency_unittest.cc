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

// Unsynchronized end-to-end stress, worth running under ThreadSanitizer:
// several writers against one draining reader, no schedule imposed. Every
// published fragment must come out exactly once and in its writer's order,
// except the ones the writer itself accounted as dropped.
//
// The individual races are pinned deterministically, in both orders, by the
// SharedRingBufferTest race tests in shared_ring_buffer_unittest.cc.

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <map>
#include <thread>
#include <vector>

#include "perfetto/base/time.h"
#include "perfetto/tracing/buffer_exhausted_policy.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "src/tracing/v2/shared_ring_buffer_test_utils.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

using Internals = test::SharedRingBufferInternalsForTest;
using BeginFragmentResult = SharedRingBufferWriter::BeginFragmentResult;
using test::GetNoopWriterDelegate;

constexpr BufferID kBuffer = 5;

struct StressParams {
  uint32_t num_writers;
  uint32_t num_chunks;
  uint32_t chunk_size;
  // An upper bound on the attempts each writer makes.
  uint32_t fragments_per_writer;
  uint32_t seed_pos;
  BufferExhaustedPolicy policy;
  // If nonzero, the writers are stopped once the reader has consumed this
  // many positions past seed_pos. That makes the run end on reader
  // progress rather than on the attempt budget, which matters for a policy
  // like kDrop where writers never wait for the reader.
  uint32_t min_positions_consumed = 0;
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
  // Acquisitions that returned kFull without reserving a position. kStall
  // keeps retrying both full reservations and failed chunk claims, so it must
  // leave this counter at zero.
  uint64_t reported_full = 0;
  uint32_t final_read_pos = 0;
};

class StressDelegate : public SharedRingBufferReader::Delegate {
 public:
  void OnChunkRead(
      const SharedRingBufferReader::ChunkContents& contents) override {
    for (uint32_t i = 0; i < contents.num_fragments; ++i) {
      const SharedRingBufferReader::Fragment& fragment = contents.fragments[i];
      ASSERT_GE(fragment.size, 8u);
      uint32_t writer = 0;
      uint32_t sequence = 0;
      memcpy(&writer, fragment.data, sizeof(writer));
      memcpy(&sequence, fragment.data + 4, sizeof(sequence));
      ASSERT_EQ(writer, contents.writer_id);
      received[writer].push_back(sequence);
    }
  }

  void OnDataLoss(WriterID writer_id) override {
    ADD_FAILURE() << "reader discarded a chunk from writer " << writer_id;
  }

  std::map<uint32_t, std::vector<uint32_t>> received;
};

void RunStress(const StressParams& params, StressStats* stats) {
  test::SharedRingBufferForTesting ring(params.num_chunks, params.chunk_size);
  Internals::SetPositions(ring.get(), params.seed_pos);

  StressDelegate delegate;
  SharedRingBufferReader reader(ring.get(), &delegate);
  Internals::SetReaderPos(&reader, params.seed_pos);

  std::atomic<uint32_t> writers_done{0};
  // Set when the run's progress target is reached or when the drain below
  // gives up, so the writers stop producing and can be joined instead of
  // racing an unresponsive protocol forever. Every blocking call a writer
  // makes has a 30 second deadline, so a stopped writer leaves its loop within
  // one BeginFragment() call.
  std::atomic<bool> stop_writers{false};
  // Attempts each writer made before it left its loop.
  std::vector<uint64_t> attempts(params.num_writers, 0);
  std::vector<uint64_t> unwritten(params.num_writers, 0);
  std::vector<uint64_t> dropped(params.num_writers, 0);
  std::vector<uint64_t> relocations(params.num_writers, 0);
  std::vector<uint64_t> reported_full(params.num_writers, 0);
  std::vector<std::thread> writer_threads;

  for (uint32_t w = 0; w < params.num_writers; ++w) {
    writer_threads.emplace_back([&, w] {
      SharedRingBufferWriter writer(ring.get(), static_cast<WriterID>(w + 1),
                                    kBuffer, params.policy,
                                    GetNoopWriterDelegate());
      // A payload that varies in size so that chunk boundaries and reuse get
      // exercised rather than one fixed shape. The 255-fragment cap is out of
      // reach: every fragment costs at least nine bytes, so even the largest
      // stress chunk (1024 bytes) holds at most 113 of them.
      // SharedRingBufferWriterTest.MaxFragmentsPerChunk covers that boundary.
      std::vector<uint8_t> payload(64);
      // Every iteration past the stop check makes exactly one attempt, whether
      // it succeeds or not, and the iteration that sees the flag makes none.
      // So n is the attempt count on both ways out of the loop.
      uint32_t n = 0;
      for (; n < params.fragments_per_writer; ++n) {
        if (stop_writers.load(std::memory_order_relaxed))
          break;
        const uint32_t size = 8 + (n % 40);
        const uint32_t writer_tag = w + 1;
        memcpy(payload.data(), &writer_tag, sizeof(writer_tag));
        memcpy(payload.data() + 4, &n, sizeof(n));

        const auto range = writer.BeginFragment(size, false);
        if (range.result != BeginFragmentResult::kSuccess) {
          ++unwritten[w];
          if (range.result == BeginFragmentResult::kFull)
            ++reported_full[w];
          writer.RecordDataLoss();
          std::this_thread::yield();
          continue;
        }
        memcpy(range.begin, payload.data(), size);
        // Yield while the chunk is still BeingWritten, so the reader lands
        // inside it and the scrape-and-relocate path is exercised for real.
        // Yielding at a protocol boundary is deliberate: a sleep would only
        // make the race likely, this makes it frequent.
        if ((n & 7) == 0)
          std::this_thread::yield();
        writer.EndFragment(size, false);
      }
      writer.FinishCurrentChunk();
      attempts[w] = n;
      dropped[w] = writer.GetStats().fragments_dropped;
      relocations[w] = writer.GetStats().relocations;
      writers_done.fetch_add(1, std::memory_order_release);
    });
  }

  // Drain until every writer has finished and the ring buffer has been emptied.
  // The deadline is far beyond what a correct run needs, even under TSAN;
  // hitting it means the protocol stopped making progress, and the test must
  // fail rather than spin here forever.
  const base::TimeMillis drain_deadline =
      base::GetWallTimeMs() + base::TimeMillis(60000);
  bool drain_timed_out = false;
  for (;;) {
    const bool all_done =
        writers_done.load(std::memory_order_acquire) == params.num_writers;
    const SharedRingBufferReader::DrainResult result = reader.Drain(64);
    if (result.last_result ==
        SharedRingBufferReader::ConsumeResult::kProtocolError) {
      break;
    }
    // Unsigned subtraction keeps this right when the positions roll over.
    if (params.min_positions_consumed != 0 &&
        reader.read_pos() - params.seed_pos >= params.min_positions_consumed) {
      stop_writers.store(true, std::memory_order_relaxed);
    }
    if (all_done && !result.needs_another_drain())
      break;
    if (base::GetWallTimeMs() >= drain_deadline) {
      drain_timed_out = true;
      ADD_FAILURE() << "Stress drain did not finish within its deadline: "
                    << writers_done.load() << "/" << params.num_writers
                    << " writers done, read_pos " << reader.read_pos() << " ("
                    << reader.read_pos() - params.seed_pos
                    << " positions past the seed), last result "
                    << static_cast<int>(result.last_result);
      break;
    }
    if (result.positions_consumed == 0)
      std::this_thread::yield();
  }

  // Stop the writers - a no-op on the success path, where they are already
  // done - and keep draining until they have all left their loops, so the
  // joins below cannot wait on a writer parked on a full ring buffer.
  stop_writers.store(true, std::memory_order_relaxed);
  const base::TimeMillis shutdown_deadline =
      base::GetWallTimeMs() + base::TimeMillis(60000);
  while (writers_done.load(std::memory_order_acquire) != params.num_writers &&
         base::GetWallTimeMs() < shutdown_deadline) {
    reader.Drain(64);
    std::this_thread::yield();
  }
  for (std::thread& thread : writer_threads)
    thread.join();
  if (drain_timed_out)
    return;  // Already failed; the accounting below would only add noise.
  ASSERT_FALSE(reader.has_protocol_error());
  reader.Drain(1u << 20);
  stats->final_read_pos = reader.read_pos();

  for (uint32_t w = 0; w < params.num_writers; ++w) {
    const std::vector<uint32_t>& sequences = delegate.received[w + 1];
    // Exactly once, and in the order the writer wrote them.
    for (size_t i = 1; i < sequences.size(); ++i) {
      ASSERT_LT(sequences[i - 1], sequences[i])
          << "writer " << w << " fragment " << i;
    }
    // Everything that is missing is something the writer itself accounted for.
    EXPECT_EQ(sequences.size() + unwritten[w] + dropped[w], attempts[w])
        << "writer " << w;

    stats->received += sequences.size();
    stats->unwritten += unwritten[w];
    stats->dropped += dropped[w];
    stats->relocations += relocations[w];
    stats->reported_full += reported_full[w];
  }
}

// The writer relocates only when the reader requests a rewrite before the
// writer publishes. This stress test does not force that schedule, so retry
// a bounded number of times if a run sees no relocations.
//
// This is a coverage check, not a proof. That relocation happens, and what it
// does to the payload and the flags, is settled deterministically by
// SharedRingBufferTest.PublicationLosesToScrape and by the reader and writer
// unit tests. All this asserts is that the stress ran with the contention it
// is named for.
constexpr uint32_t kAttemptsForRelocations = 8;

void RunStressUntilRelocations(const StressParams& params, StressStats* stats) {
  for (uint32_t attempt = 0; attempt < kAttemptsForRelocations; ++attempt) {
    RunStress(params, stats);
    if (stats->relocations > 0 || testing::Test::HasFatalFailure())
      return;
  }
}

TEST(SharedRingBufferConcurrencyTest, StressFourWritersDropPolicy) {
  StressStats stats;
  RunStressUntilRelocations({/*num_writers=*/4, /*num_chunks=*/8,
                             /*chunk_size=*/256,
                             /*fragments_per_writer=*/4000, /*seed_pos=*/0,
                             BufferExhaustedPolicy::kDrop},
                            &stats);
  // A drop policy on a small ring buffer loses a lot, which is the point - but
  // the exactly-once and ordering checks above are worthless if nothing got
  // through. An absolute floor rather than a share of the total: how much a
  // drop-policy run keeps depends on how fast the reader is scheduled, and
  // under ThreadSanitizer that is a different number. What has to hold is that
  // the per-writer checks ran on real data, not that the ring buffer achieved a
  // particular throughput.
  EXPECT_GT(stats.received, 100u);
  EXPECT_GT(stats.relocations, 0u);
}

TEST(SharedRingBufferConcurrencyTest, StressAcrossWrapCountRollover) {
  // Seeded so the 16-bit wrap count rolls over part way through - for four
  // chunks that is every 4 * 65536 positions - while the 32-bit positions are
  // nowhere near wrapping. The claim/advance arbitration runs right across the
  // truncation boundary.
  //
  // The run ends on reader progress, not on the writers' attempt budget: with
  // kDrop the writers never wait, so a reader short of CPU could otherwise
  // watch them use up a fixed budget before it had consumed the positions up
  // to the boundary. Requiring 16 traversals past it also means the writers
  // were still claiming with the wrapped-around count while the reader kept
  // advancing Free words from it.
  constexpr uint32_t kNumChunks = 4;
  constexpr uint32_t kBoundaryPos = kNumChunks * 65536u;
  constexpr uint32_t kSeedPos = kBoundaryPos - 16 * kNumChunks;
  constexpr uint32_t kMinPositionsConsumed = 32 * kNumChunks;
  StressStats stats;
  RunStress({/*num_writers=*/4, kNumChunks, /*chunk_size=*/256,
             /*fragments_per_writer=*/UINT32_MAX, kSeedPos,
             BufferExhaustedPolicy::kDrop, kMinPositionsConsumed},
            &stats);
  EXPECT_GT(stats.received, 0u);
  EXPECT_GE(stats.final_read_pos - kSeedPos, kMinPositionsConsumed);
  EXPECT_LT(WrapCountForPosition(stats.final_read_pos, kNumChunks),
            WrapCountForPosition(kSeedPos, kNumChunks));
}

TEST(SharedRingBufferConcurrencyTest, StressStallPolicy) {
  // kStall needs the futex wait. Without it the first full ring buffer reaches
  // the deliberate PERFETTO_FATAL in AcquireNewChunk(), which would take the
  // whole test binary down rather than fail this test.
  if (!SharedRingBuffer::SupportsWriterWait())
    GTEST_SKIP() << "The futex wait is not available on this platform";

  StressStats stats;
  RunStress({/*num_writers=*/3, /*num_chunks=*/8, /*chunk_size=*/1024,
             /*fragments_per_writer=*/2000, /*seed_pos=*/0,
             BufferExhaustedPolicy::kStall},
            &stats);
  EXPECT_GT(stats.received, 0u);
  // kStall retries until it acquires a chunk, including after failed claims.
  // Exhausting its deadline would abort rather than return kFull.
  EXPECT_EQ(stats.reported_full, 0u);
}

}  // namespace
}  // namespace perfetto::tracing_v2
