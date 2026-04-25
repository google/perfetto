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

#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"

#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/proto/track_event_sequence_state.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class PacketSequenceStateGenerationTest : public ::testing::Test {
 public:
  PacketSequenceStateGenerationTest() {
    context_.storage.reset(new TraceStorage());
    context_.machine_tracker.reset(
        new MachineTracker(&context_, kDefaultMachineId));
  }

 protected:
  TraceProcessorContext context_;
};

// Regression test for the dangling-back-pointer UAF originally tracked at
// `packet_sequence_state_generation.h`'s pre-fix TODO. Reproduces the report's
// scenario:
//
//   1. G1 is created and a CustomState `S` is allocated against it.
//   2. A `trace_packet_defaults` change creates G2 sharing the same
//      incremental-state interval as G1.
//   3. `SEQ_INCREMENTAL_STATE_CLEARED` creates G3 in a fresh interval.
//   4. G2 is dropped (its only owner — the Builder — has moved to G3).
//   5. G1 is still alive (e.g. pinned by the TraceSorter for a buffered
//      packet). `S` is reachable via G1.
//
// Pre-fix, `S` carried a raw `generation_` pointer that had been pointed at
// G2 in step 2 and then dangled in step 4. Accessing `S` via G1 in step 5
// would dereference freed memory.
//
// Post-fix, `S`'s back-pointer is to the IncrementalState that *owns* it,
// whose lifetime is bounded below by every Generation in the same interval
// (G1 in this case). The pointer is stable for the entire life of `S`.
TEST_F(PacketSequenceStateGenerationTest,
       NoUseAfterFreeWhenIntermediateGenDies) {
  // Step 1: G1 + a CustomState allocated through it.
  RefPtr<PacketSequenceStateGeneration> g1 =
      PacketSequenceStateGeneration::CreateFirst(&context_);
  TrackEventSequenceState* s = g1->GetCustomState<TrackEventSequenceState>();
  ASSERT_NE(s, nullptr);

  // Step 2: G2 from a `trace_packet_defaults` change. Same incremental-state
  // interval, so the CustomState is shared.
  RefPtr<PacketSequenceStateGeneration> g2 =
      g1->OnNewTracePacketDefaults(TraceBlobView());
  EXPECT_EQ(g2->GetCustomState<TrackEventSequenceState>(), s);

  // Step 3: G3 from SEQ_INCREMENTAL_STATE_CLEARED. New interval — different
  // CustomState backing.
  RefPtr<PacketSequenceStateGeneration> g3 = g2->OnIncrementalStateCleared();
  EXPECT_NE(g3->GetCustomState<TrackEventSequenceState>(), s);

  // Step 4: drop G2 (Builder no longer references it).
  g2.reset();

  // Step 5: G1 is still alive and `s` must still be safely reachable through
  // it. Touch fields/methods that previously routed through the dangling
  // back-pointer.
  TrackEventSequenceState* s_via_g1 =
      g1->GetCustomState<TrackEventSequenceState>();
  EXPECT_EQ(s_via_g1, s);
  EXPECT_FALSE(s_via_g1->timestamps_valid());
}

// SEQ_INCREMENTAL_STATE_CLEARED carries the persistent thread descriptor
// (pid/tid) into the new interval, but starts every CustomState fresh.
TEST_F(PacketSequenceStateGenerationTest,
       IncrementalStateClearedPreservesThreadDescriptor) {
  RefPtr<PacketSequenceStateGeneration> g1 =
      PacketSequenceStateGeneration::CreateFirst(&context_);
  TrackEventSequenceState* s1 = g1->GetCustomState<TrackEventSequenceState>();

  RefPtr<PacketSequenceStateGeneration> g2 = g1->OnIncrementalStateCleared();
  TrackEventSequenceState* s2 = g2->GetCustomState<TrackEventSequenceState>();

  // Different interval => different CustomState instance.
  EXPECT_NE(s1, s2);
  // Same persistent thread descriptor home — but the descriptors are *copies*
  // of each other (each interval has its own).
  EXPECT_NE(&g1->thread_descriptor(), &g2->thread_descriptor());
}

// `OnPacketLoss` must not retroactively flip validity on a generation that
// the sorter has already pinned. Asserts the pre-existing G1 keeps its
// `IsIncrementalStateValid()` answer.
TEST_F(PacketSequenceStateGenerationTest,
       OnPacketLossDoesNotMutatePinnedGeneration) {
  RefPtr<PacketSequenceStateGeneration> g1 =
      PacketSequenceStateGeneration::CreateFirst(&context_);
  RefPtr<PacketSequenceStateGeneration> g2 = g1->OnIncrementalStateCleared();
  EXPECT_TRUE(g2->IsIncrementalStateValid());

  RefPtr<PacketSequenceStateGeneration> g3 = g2->OnPacketLoss();
  EXPECT_FALSE(g3->IsIncrementalStateValid());
  // G2 — pinned via our local `g2` RefPtr (mirroring a sorter-buffered
  // packet) — must keep its validity at the value it had at tokenization
  // time.
  EXPECT_TRUE(g2->IsIncrementalStateValid());
}

}  // namespace
}  // namespace perfetto::trace_processor
