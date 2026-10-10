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

#include "src/trace_processor/importers/common/machine_claim_tracker.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/sparse_counter_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_internal.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

struct alignas(8) DummyEvent {
  uint64_t dummy = 0;
};
static_assert(sizeof(DummyEvent) % 8 == 0, "Size must be multiple of 8");
static_assert(alignof(DummyEvent) == 8, "Alignment must be 8");

class DummySink : public TraceSorter::Sink<DummyEvent, DummySink> {
 public:
  void Parse(int64_t, DummyEvent) {}
};

static constexpr auto kMachineTrackBlueprint =
    tracks::MachineScoped(tracks::CounterBlueprint(
        "machine_counter",
        tracks::UnknownUnitBlueprint(),
        tracks::DimensionBlueprints(tracks::UintDimensionBlueprint("cpu"))));

static constexpr auto kTraceTrackBlueprint = tracks::CounterBlueprint(
    "trace_counter",
    tracks::UnknownUnitBlueprint(),
    tracks::DimensionBlueprints(tracks::UintDimensionBlueprint("cpu")));

static constexpr auto kDynamicMachineBlueprint =
    tracks::MachineScoped(tracks::CounterBlueprint(
        "machine_dynamic_name",
        tracks::UnknownUnitBlueprint(),
        tracks::DimensionBlueprints(tracks::UintDimensionBlueprint("cpu")),
        tracks::DynamicNameBlueprint()));

class MachineClaimTrackerTest : public ::testing::Test {
 protected:
  TraceProcessorContext root_ =
      TraceProcessorContext::CreateRootContext(Config{});
  TraceProcessorContext* ctx1_ =
      root_.ForkContextForTrace(TraceId{1}, /*default_raw_machine_id=*/0);
  TraceProcessorContext* ctx2_ =
      root_.ForkContextForTrace(TraceId{2}, /*default_raw_machine_id=*/0);
  MachineClaimTracker* claims_ = ctx1_->machine_claim_tracker.get();

  TrackId InternTrack(TraceProcessorContext* ctx, uint32_t cpu) {
    return ctx->track_tracker->InternTrack(kMachineTrackBlueprint,
                                           tracks::Dimensions(cpu));
  }
};

// The first writer owns the track until its end; subsequent writes by another
// trace before that end are hidden.
TEST_F(MachineClaimTrackerTest, FirstWriterOwns) {
  TrackId track = InternTrack(ctx1_, 0);
  claims_->SetTraceEnd(TraceId{1}, 200);

  EXPECT_TRUE(claims_->Keep(ctx1_, track, 100));
  EXPECT_FALSE(claims_->Keep(ctx2_, track, 100));
  EXPECT_FALSE(claims_->Keep(ctx2_, track, 150));
}

// Owner end is inclusive: t2 is hidden at end1 (200) and hands over at 201,
// after which t1 is hidden.
TEST_F(MachineClaimTrackerTest, OwnerEndInclusive) {
  TrackId track = InternTrack(ctx1_, 0);
  claims_->SetTraceEnd(TraceId{1}, 200);
  claims_->SetTraceEnd(TraceId{2}, 300);

  EXPECT_TRUE(claims_->Keep(ctx1_, track, 100));
  EXPECT_FALSE(claims_->Keep(ctx2_, track, 200));
  EXPECT_TRUE(claims_->Keep(ctx2_, track, 201));
  EXPECT_FALSE(claims_->Keep(ctx1_, track, 190));
}

// An unknown trace end defaults to infinity so handovers never occur.
TEST_F(MachineClaimTrackerTest, UnknownEndMeansNoHandover) {
  TrackId track = InternTrack(ctx1_, 0);

  EXPECT_TRUE(claims_->Keep(ctx1_, track, 100));
  EXPECT_FALSE(claims_->Keep(ctx2_, track, 1'000'000'000'000LL));
}

// Ownership chains across three traces: A hands over to B after 100, B to C
// after 200.
TEST_F(MachineClaimTrackerTest, ThreeChained) {
  TraceProcessorContext* ctxA = ctx1_;
  TraceProcessorContext* ctxB = ctx2_;
  TraceProcessorContext* ctxC =
      root_.ForkContextForTrace(TraceId{3}, /*default_raw_machine_id=*/0);

  TrackId track = InternTrack(ctxA, 0);
  claims_->SetTraceEnd(TraceId{1}, 100);
  claims_->SetTraceEnd(TraceId{2}, 200);
  claims_->SetTraceEnd(TraceId{3}, 300);

  EXPECT_TRUE(claims_->Keep(ctxA, track, 50));
  EXPECT_FALSE(claims_->Keep(ctxB, track, 80));
  EXPECT_TRUE(claims_->Keep(ctxB, track, 120));
  EXPECT_FALSE(claims_->Keep(ctxC, track, 150));
  EXPECT_TRUE(claims_->Keep(ctxC, track, 250));
  EXPECT_FALSE(claims_->Keep(ctxB, track, 260));
}

// Tracks not scoped to a machine, invalid track IDs, and IDs past the end are
// always kept without recording stats.
TEST_F(MachineClaimTrackerTest, NotMachineTrackAlwaysKept) {
  TrackId normal_track = root_.storage->mutable_track_table()->Insert({}).id;
  TrackId past_end_track{99999};

  EXPECT_TRUE(claims_->Keep(ctx1_, normal_track, 100));
  EXPECT_TRUE(claims_->Keep(ctx2_, normal_track, 100));
  EXPECT_TRUE(claims_->Keep(ctx1_, kInvalidTrackId, 100));
  EXPECT_TRUE(claims_->Keep(ctx2_, kInvalidTrackId, 100));
  EXPECT_TRUE(claims_->Keep(ctx1_, past_end_track, 100));
  EXPECT_TRUE(claims_->Keep(ctx2_, past_end_track, 100));

  EXPECT_FALSE(ctx1_->global_stats_tracker
                   ->GetIndexedStats(ctx1_->machine_id(), ctx1_->trace_id(),
                                     stats::machine_track_events_hidden,
                                     static_cast<int>(normal_track.value))
                   .has_value());
  EXPECT_FALSE(ctx2_->global_stats_tracker
                   ->GetIndexedStats(ctx2_->machine_id(), ctx2_->trace_id(),
                                     stats::machine_track_events_hidden,
                                     static_cast<int>(normal_track.value))
                   .has_value());
}

// A single trace on the machine keeps all writes without stats; adding the
// same trace again is idempotent.
TEST_F(MachineClaimTrackerTest, SingleTraceNoClaims) {
  TraceProcessorContext single_root =
      TraceProcessorContext::CreateRootContext(Config{});
  TraceProcessorContext* single_ctx =
      single_root.ForkContextForTrace(TraceId{1}, /*default_raw_machine_id=*/0);

  TrackId track = single_ctx->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                         tracks::Dimensions(0));

  EXPECT_FALSE(single_ctx->machine_claim_tracker->has_multiple_traces());
  single_ctx->machine_claim_tracker->AddTrace(TraceId{1});
  EXPECT_FALSE(single_ctx->machine_claim_tracker->has_multiple_traces());

  EXPECT_TRUE(MachineClaimTracker::KeepWrite(single_ctx, track, 50));
  EXPECT_TRUE(MachineClaimTracker::KeepWrite(single_ctx, track, 100));
  EXPECT_TRUE(MachineClaimTracker::KeepWrite(single_ctx, track, 500));

  EXPECT_FALSE(single_ctx->global_stats_tracker
                   ->GetIndexedStats(single_ctx->machine_id(),
                                     single_ctx->trace_id(),
                                     stats::machine_track_events_hidden,
                                     static_cast<int>(track.value))
                   .has_value());

  single_ctx->machine_claim_tracker->AddTrace(TraceId{2});
  EXPECT_TRUE(single_ctx->machine_claim_tracker->has_multiple_traces());
}

// Hidden writes increment the indexed stat per track (3 for X, 1 for Y) and
// log once per track with the owner trace ID.
TEST_F(MachineClaimTrackerTest, StatsAndImportLogs) {
  TrackId track_x = InternTrack(ctx1_, 0);
  TrackId track_y = InternTrack(ctx1_, 1);
  claims_->SetTraceEnd(TraceId{1}, 500);

  EXPECT_TRUE(claims_->Keep(ctx1_, track_x, 100));
  EXPECT_TRUE(claims_->Keep(ctx1_, track_y, 100));

  EXPECT_FALSE(claims_->Keep(ctx2_, track_x, 110));
  EXPECT_FALSE(claims_->Keep(ctx2_, track_x, 120));
  EXPECT_FALSE(claims_->Keep(ctx2_, track_x, 130));

  EXPECT_FALSE(claims_->Keep(ctx2_, track_y, 140));

  EXPECT_EQ(
      ctx2_->global_stats_tracker->GetIndexedStats(
          ctx2_->machine_id(), ctx2_->trace_id(),
          stats::machine_track_events_hidden, static_cast<int>(track_x.value)),
      3);
  EXPECT_EQ(
      ctx2_->global_stats_tracker->GetIndexedStats(
          ctx2_->machine_id(), ctx2_->trace_id(),
          stats::machine_track_events_hidden, static_cast<int>(track_y.value)),
      1);

  const auto& logs = ctx2_->storage->trace_import_logs_table();
  EXPECT_EQ(logs.row_count(), 2u);

  StringId track_id_key = ctx2_->storage->InternString("track_id");
  StringId owner_trace_id_key = ctx2_->storage->InternString("owner_trace_id");
  const auto& args = ctx2_->storage->arg_table();

  bool found_log_x = false;
  bool found_log_y = false;

  for (uint32_t i = 0; i < logs.row_count(); ++i) {
    ASSERT_TRUE(logs[i].arg_set_id().has_value());
    uint32_t arg_set_id = *logs[i].arg_set_id();

    std::optional<int64_t> logged_track_id;
    std::optional<int64_t> logged_owner_trace_id;
    for (auto arg_it = args.IterateRows(); arg_it; ++arg_it) {
      if (arg_it.arg_set_id() == arg_set_id) {
        if (arg_it.key() == track_id_key) {
          logged_track_id = arg_it.int_value();
        } else if (arg_it.key() == owner_trace_id_key) {
          logged_owner_trace_id = arg_it.int_value();
        }
      }
    }
    ASSERT_TRUE(logged_track_id.has_value());
    ASSERT_TRUE(logged_owner_trace_id.has_value());
    EXPECT_EQ(*logged_owner_trace_id,
              static_cast<int64_t>(ctx1_->trace_id().value));

    if (*logged_track_id == static_cast<int64_t>(track_x.value)) {
      found_log_x = true;
    } else if (*logged_track_id == static_cast<int64_t>(track_y.value)) {
      found_log_y = true;
    }
  }

  EXPECT_TRUE(found_log_x);
  EXPECT_TRUE(found_log_y);
}

// Check() predicts the exact outcome of Keep() without mutating claim state.
TEST_F(MachineClaimTrackerTest, CheckAgreesWithKeep) {
  TrackId track = InternTrack(ctx1_, 0);
  claims_->SetTraceEnd(TraceId{1}, 200);

  EXPECT_EQ(claims_->Check(ctx1_->trace_id(), track, 100),
            MachineClaimTracker::Claim::kFirstWrite);
  EXPECT_TRUE(claims_->Keep(ctx1_, track, 100));

  EXPECT_EQ(claims_->Check(ctx1_->trace_id(), track, 150),
            MachineClaimTracker::Claim::kOwned);
  EXPECT_TRUE(claims_->Keep(ctx1_, track, 150));

  EXPECT_EQ(claims_->Check(ctx2_->trace_id(), track, 180),
            MachineClaimTracker::Claim::kHidden);
  EXPECT_FALSE(claims_->Keep(ctx2_, track, 180));

  EXPECT_EQ(claims_->Check(ctx2_->trace_id(), track, 200),
            MachineClaimTracker::Claim::kHidden);
  EXPECT_FALSE(claims_->Keep(ctx2_, track, 200));

  EXPECT_EQ(claims_->Check(ctx2_->trace_id(), track, 201),
            MachineClaimTracker::Claim::kHandover);
  EXPECT_TRUE(claims_->Keep(ctx2_, track, 201));

  EXPECT_EQ(claims_->Check(ctx2_->trace_id(), track, 250),
            MachineClaimTracker::Claim::kOwned);
  EXPECT_TRUE(claims_->Keep(ctx2_, track, 250));
}

// Trace ends are read from sorter queues once and frozen, ignoring later
// pushes and extraction.
TEST_F(MachineClaimTrackerTest, ReadTraceEndsOnce) {
  TrackId sorter_track = ctx1_->track_tracker->InternTrack(
      kTraceTrackBlueprint, tracks::Dimensions(0));
  TrackId claim_track1 = ctx1_->track_tracker->InternTrack(
      kMachineTrackBlueprint, tracks::Dimensions(0));
  TrackId claim_track2 = ctx1_->track_tracker->InternTrack(
      kMachineTrackBlueprint, tracks::Dimensions(1));

  ctx1_->sparse_counter_tracker->PushCounter(100, sorter_track, 1.0);

  auto stream1_extra =
      root_.sorter->CreateStream(TraceId{1}, std::make_unique<DummySink>());
  stream1_extra->Push(200, DummyEvent{});

  ctx2_->sparse_counter_tracker->PushCounter(300, sorter_track, 2.0);

  EXPECT_EQ(root_.sorter->MaxTimestampForTrace(TraceId{1}), 200);
  EXPECT_EQ(root_.sorter->MaxTimestampForTrace(TraceId{2}), 300);

  MachineClaimTracker::ReadTraceEnds(&root_);

  root_.sorter->ExtractEventsForced();
  EXPECT_EQ(root_.sorter->MaxTimestampForTrace(TraceId{1}), std::nullopt);
  EXPECT_EQ(root_.sorter->MaxTimestampForTrace(TraceId{2}), std::nullopt);

  // Push new events with later timestamps into each trace's queues.
  ctx1_->sparse_counter_tracker->PushCounter(500, sorter_track, 10.0);
  stream1_extra->Push(600, DummyEvent{});
  ctx2_->sparse_counter_tracker->PushCounter(700, sorter_track, 20.0);

  EXPECT_EQ(root_.sorter->MaxTimestampForTrace(TraceId{1}), 600);
  EXPECT_EQ(root_.sorter->MaxTimestampForTrace(TraceId{2}), 700);

  MachineClaimTracker::ReadTraceEnds(&root_);

  // Ends remain 200 and 300: t2 is hidden at 200 and takes over at 201.
  EXPECT_TRUE(claims_->Keep(ctx1_, claim_track1, 50));
  EXPECT_FALSE(claims_->Keep(ctx2_, claim_track1, 200));
  EXPECT_TRUE(claims_->Keep(ctx2_, claim_track1, 201));

  EXPECT_TRUE(claims_->Keep(ctx1_, claim_track2, 50));
  EXPECT_TRUE(claims_->Keep(ctx2_, claim_track2, 400));
}

// ForkContextForTrace tags the sparse counter stream with its trace ID.
TEST_F(MachineClaimTrackerTest, ForkQueueTagged) {
  TrackId track = ctx1_->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                    tracks::Dimensions(0));
  ctx1_->sparse_counter_tracker->PushCounter(500, track, 1.0);
  EXPECT_EQ(root_.sorter->MaxTimestampForTrace(TraceId{1}), 500);
  EXPECT_EQ(root_.sorter->MaxTimestampForTrace(TraceId{2}), std::nullopt);
}

class MachineTrackInterningTest : public ::testing::Test {
 protected:
  TraceProcessorContext root_ =
      TraceProcessorContext::CreateRootContext(Config{});
};

// Traces on the same machine share machine-scoped tracks, while trace-scoped
// tracks remain distinct.
TEST_F(MachineTrackInterningTest, SameMachineSharesTrack) {
  TraceProcessorContext* c1 =
      root_.ForkContextForTrace(TraceId{1}, /*default_raw_machine_id=*/0);
  TraceProcessorContext* c2 =
      root_.ForkContextForTrace(TraceId{2}, /*default_raw_machine_id=*/0);

  TrackId m1 = c1->track_tracker->InternTrack(kMachineTrackBlueprint,
                                              tracks::Dimensions(0));
  TrackId m2 = c2->track_tracker->InternTrack(kMachineTrackBlueprint,
                                              tracks::Dimensions(0));
  EXPECT_EQ(m1, m2);

  TrackId t1 = c1->track_tracker->InternTrack(kTraceTrackBlueprint,
                                              tracks::Dimensions(0));
  TrackId t2 = c2->track_tracker->InternTrack(kTraceTrackBlueprint,
                                              tracks::Dimensions(0));
  EXPECT_NE(t1, t2);
}

// The first trace to intern a machine track sets its name and args; subsequent
// traces reuse the existing track row.
TEST_F(MachineTrackInterningTest, FirstTraceWritesNameAndArgs) {
  TraceProcessorContext* c1 =
      root_.ForkContextForTrace(TraceId{1}, /*default_raw_machine_id=*/0);
  TraceProcessorContext* c2 =
      root_.ForkContextForTrace(TraceId{2}, /*default_raw_machine_id=*/0);

  StringId name_a = root_.storage->InternString("a");
  StringId name_b = root_.storage->InternString("b");
  StringId key_k = root_.storage->InternString("k");

  TrackId tr1 = c1->track_tracker->InternTrack(
      kDynamicMachineBlueprint, tracks::Dimensions(0),
      tracks::DynamicName(name_a), [&](ArgsTracker::BoundInserter& inserter) {
        inserter.AddArg(key_k, Variadic::Integer(1));
      });

  TrackId tr2 = c2->track_tracker->InternTrack(
      kDynamicMachineBlueprint, tracks::Dimensions(0),
      tracks::DynamicName(name_b), [&](ArgsTracker::BoundInserter& inserter) {
        inserter.AddArg(key_k, Variadic::Integer(2));
      });

  EXPECT_EQ(tr1, tr2);
  EXPECT_EQ(root_.storage->track_table()[tr1].name(), name_a);

  std::optional<uint32_t> arg_set_id =
      root_.storage->track_table()[tr1].source_arg_set_id();
  ASSERT_TRUE(arg_set_id.has_value());

  const auto& args = root_.storage->arg_table();
  std::optional<int64_t> arg_val;
  for (auto it = args.IterateRows(); it; ++it) {
    if (it.arg_set_id() == *arg_set_id && it.key() == key_k) {
      arg_val = it.int_value();
    }
  }
  ASSERT_TRUE(arg_val.has_value());
  EXPECT_EQ(*arg_val, 1);
}

// Machine-scoped tracks with identical blueprints on different machines are
// assigned separate track IDs.
TEST_F(MachineTrackInterningTest, DifferentMachinesSeparate) {
  TraceProcessorContext* c_m0 =
      root_.ForkContextForTrace(TraceId{1}, /*default_raw_machine_id=*/0);
  TraceProcessorContext* c_m1 =
      root_.ForkContextForTrace(TraceId{2}, /*default_raw_machine_id=*/1);

  TrackId tr0 = c_m0->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                 tracks::Dimensions(0));
  TrackId tr1 = c_m1->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                 tracks::Dimensions(0));

  EXPECT_NE(tr0, tr1);
  uint32_t row0 = root_.storage->track_table()[tr0].ToRowNumber().row_number();
  uint32_t m_id0 =
      root_.storage->track_table()
          .dataframe()
          .GetCellUnchecked<tables::TrackTable::ColumnIndex::machine_id>(
              tables::TrackTable::kSpec, row0);
  EXPECT_EQ(m_id0, c_m0->machine_id().value);

  uint32_t row1 = root_.storage->track_table()[tr1].ToRowNumber().row_number();
  uint32_t m_id1 =
      root_.storage->track_table()
          .dataframe()
          .GetCellUnchecked<tables::TrackTable::ColumnIndex::machine_id>(
              tables::TrackTable::kSpec, row1);
  EXPECT_EQ(m_id1, c_m1->machine_id().value);
  EXPECT_NE(c_m0->machine_id(), c_m1->machine_id());
}

// Contexts without a machine claim tracker fall back to per-trace interning.
TEST_F(MachineTrackInterningTest, NullMachineStateFallsBackPerTrace) {
  auto ctx1 = std::make_unique<TraceProcessorContext>();
  ctx1->storage = TraceProcessorContextPtr<TraceStorage>::MakeRoot();
  ctx1->global_stats_tracker =
      TraceProcessorContextPtr<GlobalStatsTracker>::MakeRoot(
          ctx1->storage.get());
  ctx1->global_args_tracker =
      TraceProcessorContextPtr<GlobalArgsTracker>::MakeRoot(
          ctx1->storage.get());
  ctx1->machine_tracker =
      std::make_unique<MachineTracker>(ctx1.get(), kDefaultMachineId);
  ctx1->trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{1}});
  ctx1->stats_tracker = std::make_unique<StatsTracker>(ctx1.get());
  ctx1->cpu_tracker = std::make_unique<CpuTracker>(ctx1.get());
  ctx1->track_tracker = std::make_unique<TrackTracker>(ctx1.get());

  auto ctx2 = std::make_unique<TraceProcessorContext>();
  ctx2->storage = ctx1->storage.Fork();
  ctx2->global_stats_tracker = ctx1->global_stats_tracker.Fork();
  ctx2->global_args_tracker = ctx1->global_args_tracker.Fork();
  ctx2->machine_tracker =
      std::make_unique<MachineTracker>(ctx2.get(), kDefaultMachineId);
  ctx2->trace_state =
      TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
          TraceProcessorContext::TraceState{TraceId{2}});
  ctx2->stats_tracker = std::make_unique<StatsTracker>(ctx2.get());
  ctx2->cpu_tracker = std::make_unique<CpuTracker>(ctx2.get());
  ctx2->track_tracker = std::make_unique<TrackTracker>(ctx2.get());

  EXPECT_FALSE(ctx1->machine_claim_tracker);
  EXPECT_FALSE(ctx2->machine_claim_tracker);

  TrackId tr1_a = ctx1->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                   tracks::Dimensions(0));
  TrackId tr1_b = ctx1->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                   tracks::Dimensions(0));
  EXPECT_EQ(tr1_a, tr1_b);

  TrackId tr2_a = ctx2->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                   tracks::Dimensions(0));
  TrackId tr2_b = ctx2->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                   tracks::Dimensions(0));
  EXPECT_EQ(tr2_a, tr2_b);

  EXPECT_NE(tr1_a, tr2_a);
}

// MachineScoped preserves blueprint typedefs and hashes while setting
// kScope to kMachine.
TEST_F(MachineTrackInterningTest, StaticChecks) {
  constexpr auto kPlain = tracks::CounterBlueprint(
      "test_counter", tracks::StaticUnitBlueprint("bytes"),
      tracks::DimensionBlueprints(tracks::UintDimensionBlueprint("cpu")),
      tracks::StaticNameBlueprint("test_name"));
  constexpr auto kScoped = tracks::MachineScoped(kPlain);

  static_assert(std::is_same_v<decltype(kScoped)::dimensions_t,
                               decltype(kPlain)::dimensions_t>);
  static_assert(
      std::is_same_v<decltype(kScoped)::name_t, decltype(kPlain)::name_t>);
  static_assert(
      std::is_same_v<decltype(kScoped)::unit_t, decltype(kPlain)::unit_t>);
  static_assert(std::is_same_v<decltype(kScoped)::name_blueprint_t,
                               decltype(kPlain)::name_blueprint_t>);

  static_assert(decltype(kScoped)::kScope == tracks::Scope::kMachine);
  static_assert(decltype(kPlain)::kScope == tracks::Scope::kTrace);

  auto dims = tracks::Dimensions(uint32_t{42});
  EXPECT_EQ(tracks::HashFromBlueprintAndDimensions(kScoped, dims),
            tracks::HashFromBlueprintAndDimensions(kPlain, dims));
}

class MachineSeriesTest : public ::testing::Test {
 protected:
  TraceProcessorContext root_ =
      TraceProcessorContext::CreateRootContext(Config{});
  TraceProcessorContext* ctxA_ =
      root_.ForkContextForTrace(TraceId{1}, /*default_raw_machine_id=*/0);
  TraceProcessorContext* ctxB_ =
      root_.ForkContextForTrace(TraceId{2}, /*default_raw_machine_id=*/0);
  TraceProcessorContext* ctxC_ =
      root_.ForkContextForTrace(TraceId{3}, /*default_raw_machine_id=*/0);
  MachineClaimTracker* claims_ = ctxA_->machine_claim_tracker.get();
  TrackId track_ = ctxA_->track_tracker->InternTrack(kMachineTrackBlueprint,
                                                     tracks::Dimensions(0));

  // Time in nanoseconds given hours and minutes.
  static constexpr int64_t Time(int hours, int minutes) {
    return (static_cast<int64_t>(hours) * 60 + minutes) * 60'000'000'000LL;
  }

  struct StepResult {
    double series_value;
    bool kept;
  };

  StepResult Step(TraceProcessorContext* ctx,
                  MachineClaimTracker::SeriesInput* input,
                  int64_t ts,
                  double value) {
    double s = claims_->SeriesValue(ctx->trace_id(), track_, ts, value, input);
    bool kept = claims_->Keep(ctx, track_, ts);
    return {s, kept};
  }
};

// B takes over at 10:05: B is 230 at A's last sample (10:00), so
// 500 + (260 - 230) = 530.
TEST_F(MachineSeriesTest, DocExample) {
  claims_->SetTraceEnd(TraceId{1}, Time(10, 2));
  claims_->SetTraceEnd(TraceId{2}, Time(12, 0));

  MachineClaimTracker::SeriesInput inA;
  MachineClaimTracker::SeriesInput inB;

  auto r1 = Step(ctxA_, &inA, Time(9, 50), 400);
  EXPECT_TRUE(r1.kept);
  EXPECT_DOUBLE_EQ(r1.series_value, 400.0);

  auto r2 = Step(ctxB_, &inB, Time(9, 55), 200);
  EXPECT_FALSE(r2.kept);

  auto r3 = Step(ctxA_, &inA, Time(10, 0), 500);
  EXPECT_TRUE(r3.kept);
  EXPECT_DOUBLE_EQ(r3.series_value, 500.0);

  auto r4 = Step(ctxB_, &inB, Time(10, 5), 260);
  EXPECT_TRUE(r4.kept);
  EXPECT_DOUBLE_EQ(r4.series_value, 530.0);

  auto r5 = Step(ctxB_, &inB, Time(10, 7), 270);
  EXPECT_TRUE(r5.kept);
  EXPECT_DOUBLE_EQ(r5.series_value, 540.0);
}

// Interpolation at A's last sample (10:00) is stored on B's hidden sample at
// 10:01 (230), yielding 500 + (260 - 230) = 530 at 10:05.
TEST_F(MachineSeriesTest, StoredValuePath) {
  claims_->SetTraceEnd(TraceId{1}, Time(10, 2));
  claims_->SetTraceEnd(TraceId{2}, Time(12, 0));

  MachineClaimTracker::SeriesInput inA;
  MachineClaimTracker::SeriesInput inB;

  Step(ctxA_, &inA, Time(9, 50), 400);
  Step(ctxB_, &inB, Time(9, 55), 200);
  Step(ctxA_, &inA, Time(10, 0), 500);

  auto r_hidden = Step(ctxB_, &inB, Time(10, 1), 236);
  EXPECT_FALSE(r_hidden.kept);

  auto r_handover = Step(ctxB_, &inB, Time(10, 5), 260);
  EXPECT_TRUE(r_handover.kept);
  EXPECT_DOUBLE_EQ(r_handover.series_value, 530.0);
}

// A writes after B's hidden sample, so interpolation is recomputed at 10:02
// (242), yielding 510 + (260 - 242) = 528 at 10:05.
TEST_F(MachineSeriesTest, RecomputePath) {
  claims_->SetTraceEnd(TraceId{1}, Time(10, 2));
  claims_->SetTraceEnd(TraceId{2}, Time(12, 0));

  MachineClaimTracker::SeriesInput inA;
  MachineClaimTracker::SeriesInput inB;

  auto r1 = Step(ctxA_, &inA, Time(10, 0), 500);
  EXPECT_TRUE(r1.kept);
  EXPECT_DOUBLE_EQ(r1.series_value, 500.0);

  auto r2 = Step(ctxB_, &inB, Time(10, 1), 236);
  EXPECT_FALSE(r2.kept);

  auto r3 = Step(ctxA_, &inA, Time(10, 2), 510);
  EXPECT_TRUE(r3.kept);
  EXPECT_DOUBLE_EQ(r3.series_value, 510.0);

  auto r4 = Step(ctxB_, &inB, Time(10, 5), 260);
  EXPECT_TRUE(r4.kept);
  EXPECT_DOUBLE_EQ(r4.series_value, 528.0);
}

// Without prior samples from B, handover falls back to flat continuation from
// A's last value (500 at 10:03, then 510 at 10:06).
TEST_F(MachineSeriesTest, FirstSampleFallback) {
  claims_->SetTraceEnd(TraceId{1}, Time(10, 2));
  claims_->SetTraceEnd(TraceId{2}, Time(12, 0));

  MachineClaimTracker::SeriesInput inA;
  MachineClaimTracker::SeriesInput inB;

  auto r1 = Step(ctxA_, &inA, Time(10, 0), 500);
  EXPECT_TRUE(r1.kept);
  EXPECT_DOUBLE_EQ(r1.series_value, 500.0);

  auto r2 = Step(ctxB_, &inB, Time(10, 3), 300);
  EXPECT_TRUE(r2.kept);
  EXPECT_DOUBLE_EQ(r2.series_value, 500.0);

  auto r3 = Step(ctxB_, &inB, Time(10, 6), 310);
  EXPECT_TRUE(r3.kept);
  EXPECT_DOUBLE_EQ(r3.series_value, 510.0);
}

// Interpolated handovers chain across A -> B -> C: B takes over at 120 (235)
// and C takes over at 220 (285).
TEST_F(MachineSeriesTest, Chain) {
  claims_->SetTraceEnd(TraceId{1}, 100);
  claims_->SetTraceEnd(TraceId{2}, 200);
  claims_->SetTraceEnd(TraceId{3}, 300);

  MachineClaimTracker::SeriesInput inA;
  MachineClaimTracker::SeriesInput inB;
  MachineClaimTracker::SeriesInput inC;

  EXPECT_DOUBLE_EQ(Step(ctxA_, &inA, 10, 100).series_value, 100.0);
  EXPECT_FALSE(Step(ctxB_, &inB, 40, 50).kept);
  EXPECT_DOUBLE_EQ(Step(ctxA_, &inA, 50, 200).series_value, 200.0);
  EXPECT_FALSE(Step(ctxB_, &inB, 80, 70).kept);

  auto r_b120 = Step(ctxB_, &inB, 120, 90);
  EXPECT_TRUE(r_b120.kept);
  EXPECT_DOUBLE_EQ(r_b120.series_value, 235.0);

  EXPECT_FALSE(Step(ctxC_, &inC, 140, 10).kept);

  auto r_b160 = Step(ctxB_, &inB, 160, 110);
  EXPECT_TRUE(r_b160.kept);
  EXPECT_DOUBLE_EQ(r_b160.series_value, 255.0);

  EXPECT_FALSE(Step(ctxC_, &inC, 180, 30).kept);

  auto r_c220 = Step(ctxC_, &inC, 220, 50);
  EXPECT_TRUE(r_c220.kept);
  EXPECT_DOUBLE_EQ(r_c220.series_value, 285.0);

  auto r_c260 = Step(ctxC_, &inC, 260, 70);
  EXPECT_TRUE(r_c260.kept);
  EXPECT_DOUBLE_EQ(r_c260.series_value, 305.0);
}

// Monotonically increasing inputs across chained handovers produce a
// monotonically increasing series.
TEST_F(MachineSeriesTest, NeverDecreases) {
  claims_->SetTraceEnd(TraceId{1}, 1000);
  claims_->SetTraceEnd(TraceId{2}, 2000);
  claims_->SetTraceEnd(TraceId{3}, 3000);

  MachineClaimTracker::SeriesInput inA;
  MachineClaimTracker::SeriesInput inB;
  MachineClaimTracker::SeriesInput inC;

  std::minstd_rand0 rng(42);

  struct Event {
    TraceProcessorContext* ctx;
    MachineClaimTracker::SeriesInput* input;
    int64_t ts;
    double value;
  };
  std::vector<Event> events;

  auto generate_trace_events = [&](TraceProcessorContext* ctx,
                                   MachineClaimTracker::SeriesInput* input,
                                   int64_t start_ts, int64_t end_ts,
                                   int count) {
    int64_t ts = start_ts;
    double val = 10.0;
    int64_t step_ts = std::max<int64_t>(1, (end_ts - start_ts) / (count + 1));
    for (int i = 0; i < count; ++i) {
      ts += step_ts + static_cast<int64_t>(rng() % 5);
      val += static_cast<double>(rng() % 10);
      events.push_back({ctx, input, ts, val});
    }
  };

  generate_trace_events(ctxA_, &inA, 0, 1000, 20);
  generate_trace_events(ctxB_, &inB, 500, 2000, 20);
  generate_trace_events(ctxC_, &inC, 1500, 3000, 20);

  std::sort(events.begin(), events.end(),
            [](const Event& a, const Event& b) { return a.ts < b.ts; });

  double prev_kept_series = std::numeric_limits<double>::lowest();
  size_t kept_count = 0;

  for (const auto& ev : events) {
    StepResult res = Step(ev.ctx, ev.input, ev.ts, ev.value);
    if (res.kept) {
      EXPECT_GE(res.series_value, prev_kept_series);
      prev_kept_series = res.series_value;
      ++kept_count;
    }
  }

  EXPECT_GT(kept_count, 0u);
}

// A single owner produces bit-identical double values without offset
// adjustment.
TEST_F(MachineSeriesTest, SingleOwnerBitIdentical) {
  MachineClaimTracker::SeriesInput inA;

  double values[] = {0.1, 0.3, 0.7, 1e15 + 0.5};
  int64_t ts = 100;

  for (double val : values) {
    double res = claims_->SeriesValue(TraceId{1}, track_, ts, val, &inA);
    bool kept = claims_->Keep(ctxA_, track_, ts);
    EXPECT_TRUE(kept);
    EXPECT_EQ(res, val);
    ts += 100;
  }
}

// Events through sparse counter sink and sorter produce the expected series
// table rows and hidden count.
TEST_F(MachineSeriesTest, EndToEndThroughSink) {
  // Trace 1 events (endA = 200):
  ctxA_->sparse_counter_tracker->PushMachineSeriesCounter(100, track_, 100.0);
  ctxA_->sparse_counter_tracker->PushMachineSeriesCounter(200, track_, 200.0);

  // Trace 2 events:
  ctxB_->sparse_counter_tracker->PushMachineSeriesCounter(150, track_, 50.0);
  ctxB_->sparse_counter_tracker->PushMachineSeriesCounter(200, track_, 60.0);
  ctxB_->sparse_counter_tracker->PushMachineSeriesCounter(201, track_, 70.0);
  ctxB_->sparse_counter_tracker->PushMachineSeriesCounter(250, track_, 80.0);

  MachineClaimTracker::ReadTraceEnds(&root_);

  root_.sorter->ExtractEventsForced();

  const auto& counters = root_.storage->counter_table();
  ASSERT_EQ(counters.row_count(), 4u);

  EXPECT_EQ(counters[0].ts(), 100);
  EXPECT_DOUBLE_EQ(counters[0].value(), 100.0);
  EXPECT_EQ(counters[0].track_id(), track_);

  EXPECT_EQ(counters[1].ts(), 200);
  EXPECT_DOUBLE_EQ(counters[1].value(), 200.0);
  EXPECT_EQ(counters[1].track_id(), track_);

  EXPECT_EQ(counters[2].ts(), 201);
  EXPECT_DOUBLE_EQ(counters[2].value(), 210.0);
  EXPECT_EQ(counters[2].track_id(), track_);

  EXPECT_EQ(counters[3].ts(), 250);
  EXPECT_DOUBLE_EQ(counters[3].value(), 220.0);
  EXPECT_EQ(counters[3].track_id(), track_);

  EXPECT_EQ(
      ctxB_->global_stats_tracker->GetIndexedStats(
          ctxB_->machine_id(), ctxB_->trace_id(),
          stats::machine_track_events_hidden, static_cast<int>(track_.value)),
      2);
}

}  // namespace
}  // namespace perfetto::trace_processor
