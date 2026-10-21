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

// Tests for SliceTracker's SliceSink: the delivery contract in both modes and
// the regressions found while reviewing it.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

constexpr char kHashKey[] = "chrome_histogram_sample.name_hash";
constexpr char kNameKey[] = "chrome_histogram_sample.name";

class RecordingSink : public SliceSink {
 public:
  void OnSliceFinalized(SliceId id) override {
    ids.push_back(id);
    if (storage)
      durs.push_back(storage->slice_table()[id].dur());
  }
  void OnSliceRecord(FinalizedSlice&& r) override {
    records.push_back(std::move(r));
  }
  std::vector<SliceId> ids;
  std::vector<FinalizedSlice> records;
  // If set, each delivered row's dur at delivery time lands in |durs|.
  const TraceStorage* storage = nullptr;
  std::vector<int64_t> durs;
};

// Calls back into the tracker from both callbacks while |armed|.
class ReentrantSink : public SliceSink {
 public:
  explicit ReentrantSink(SliceTracker* t) : t_(t) {}
  void OnSliceFinalized(SliceId) override { Reenter(); }
  void OnSliceRecord(FinalizedSlice&&) override { Reenter(); }
  bool armed = true;

 private:
  void Reenter() {
    if (armed)
      t_->Begin(99, TrackId{1u}, kNullStringId, StringId::Raw(1));
  }
  SliceTracker* t_;
};

// Captures the arg set a kAlongsideTable row has at the moment it is delivered.
class ArgSetSink : public SliceSink {
 public:
  explicit ArgSetSink(const TraceStorage* storage) : storage_(storage) {}
  void OnSliceFinalized(SliceId id) override {
    delivered.emplace_back(id, storage_->slice_table()[id].arg_set_id());
  }
  std::vector<std::pair<SliceId, std::optional<uint32_t>>> delivered;

 private:
  const TraceStorage* storage_;
};

class SliceSinkTest : public ::testing::Test {
 public:
  SliceSinkTest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    context_.args_translation_table =
        std::make_unique<ArgsTranslationTable>(context_.storage.get());
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(context_.storage.get());
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.import_logs_tracker = std::make_unique<ImportLogsTracker>(
        &context_, tables::TraceFileTable::Id{0});
    context_.slice_tracker = std::make_unique<SliceTracker>(&context_);
    context_.args_translation_table->AddChromeHistogramTranslationRule(
        42, "Hist.Name");
    hash_key_ = context_.storage->InternString(kHashKey);
    name_key_ = context_.storage->InternString(kNameKey);
  }

 protected:
  SliceTracker& tracker() { return *context_.slice_tracker; }

  // Adds a histogram hash arg, which ArgsTranslationTable translates.
  auto TranslatableArg() {
    return [this](ArgsTracker::BoundInserter* inserter) {
      inserter->AddArg(hash_key_, Variadic::UnsignedInteger(42));
    };
  }

  static const ArgsTracker::CompactArg* FindArg(const FinalizedSlice& r,
                                                StringId key) {
    for (const auto& a : r.args) {
      if (a.key == key)
        return &a;
    }
    return nullptr;
  }

  // Declared before |context_| so they outlive the trackers it owns, whose
  // destructors flush into them.
  RecordingSink sink_;
  std::unique_ptr<ArgSetSink> arg_sink_;
  TraceProcessorContext context_;
  StringId hash_key_;
  StringId name_key_;
  static constexpr TrackId kTrack{1u};
  static constexpr TrackId kOtherTrack{2u};
};

TEST_F(SliceSinkTest, InsteadOfTableRecordsCarryTheRowAndTableStaysEmpty) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  StringId key = context_.storage->InternString("k");
  auto parent = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1),
                                [&](ArgsTracker::BoundInserter* i) {
                                  i->AddArg(key, Variadic::Integer(7));
                                });
  auto child = tracker().Begin(20, kTrack, kNullStringId, StringId::Raw(2));
  tracker().End(30, kTrack);
  tracker().End(40, kTrack);
  tracker().FlushPendingSlices();

  EXPECT_EQ(context_.storage->slice_table().row_count(), 0u);
  EXPECT_EQ(context_.storage->arg_table().row_count(), 0u);
  ASSERT_EQ(sink.records.size(), 2u);
  const FinalizedSlice& c = sink.records[0];  // Children are delivered first.
  const FinalizedSlice& p = sink.records[1];
  EXPECT_EQ(c.id, *child);
  EXPECT_EQ(c.ts, 20);
  EXPECT_EQ(c.dur, 10);
  EXPECT_EQ(c.depth, 1u);
  EXPECT_EQ(c.parent_id, parent);
  EXPECT_EQ(p.id, *parent);
  EXPECT_EQ(p.dur, 30);
  ASSERT_EQ(p.args.size(), 1u);
  EXPECT_EQ(p.args[0].key, key);
  EXPECT_EQ(p.args[0].value.int_value, 7);
}

// Review issue 1: FlushPendingSlices routed translatable args of open slices
// through ArgsTracker::AddArgsTo(SliceId), i.e. into a slice table row that
// does not exist, and delivered the record with no args.
TEST_F(SliceSinkTest,
       InsteadOfTableOpenSliceTranslatableArgsAreTranslatedAtFlush) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1),
                  TranslatableArg());
  tracker().FlushPendingSlices();

  EXPECT_EQ(context_.storage->slice_table().row_count(), 0u);
  EXPECT_EQ(context_.storage->arg_table().row_count(), 0u);
  ASSERT_EQ(sink.records.size(), 1u);
  EXPECT_EQ(sink.records[0].dur, -1);
  ASSERT_NE(FindArg(sink.records[0], hash_key_), nullptr);
  const auto* name = FindArg(sink.records[0], name_key_);
  ASSERT_NE(name, nullptr);
  EXPECT_EQ(context_.storage->GetString(name->value.string_value), "Hist.Name");
}

// Review issue 5 (kInsteadOfTable half): translatable args used to be
// delivered untranslated. They are held back and translated at flush, and
// the caller can still patch thread deltas on them after End().
TEST_F(SliceSinkTest,
       InsteadOfTablePoppedTranslatableSliceWaitsForTranslation) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  auto id = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1),
                            TranslatableArg());
  SliceTracker::ThreadTiming start;
  start.ts = 100;
  tracker().SetThreadTiming(*id, start);
  tracker().End(20, kTrack);
  auto begun = tracker().ThreadTimingOfRecentlyEnded(*id);
  ASSERT_TRUE(begun && begun->ts);
  tracker().SetThreadDeltas(*id, 105 - *begun->ts, std::nullopt);

  tracker().Begin(30, kOtherTrack, kNullStringId, StringId::Raw(2));
  EXPECT_TRUE(sink.records.empty());
  tracker().FlushPendingSlices();

  ASSERT_EQ(sink.records.size(), 2u);
  const FinalizedSlice& r =
      sink.records[0].id == *id ? sink.records[0] : sink.records[1];
  EXPECT_EQ(r.id, *id);
  EXPECT_EQ(r.thread.ts, 100);
  EXPECT_EQ(r.thread.dur, 5);
  EXPECT_NE(FindArg(r, name_key_), nullptr);
}

// Review issue 5 (kAlongsideTable half): the whole arg set of a translatable
// slice is deferred to end of trace, so delivering it at pop exposed a row
// with no args at all.
TEST_F(SliceSinkTest,
       AlongsideTableTranslatableSliceIsDeliveredAfterTranslation) {
  arg_sink_ = std::make_unique<ArgSetSink>(context_.storage.get());
  ArgSetSink& sink = *arg_sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kAlongsideTable);
  auto id = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1),
                            TranslatableArg());
  tracker().End(20, kTrack);
  tracker().Begin(30, kOtherTrack, kNullStringId, StringId::Raw(2));
  EXPECT_TRUE(sink.delivered.empty());
  tracker().FlushPendingSlices();

  std::optional<uint32_t> arg_set;
  for (const auto& [slice, set] : sink.delivered) {
    if (slice == *id)
      arg_set = set;
  }
  ASSERT_TRUE(arg_set);
  bool translated = false;
  const auto& args = context_.storage->arg_table();
  for (auto it = args.IterateRows(); it; ++it)
    translated |= it.arg_set_id() == *arg_set && it.key() == name_key_;
  EXPECT_TRUE(translated);
}

// Review issue 3: each SliceTracker counted ids from 0, but ids land in
// storage shared by every trace and machine context (flows, callstacks, GPU
// correlation, virtual track slices).
TEST_F(SliceSinkTest, InsteadOfTableIdsAreUniqueAcrossTrackersSharingStorage) {
  RecordingSink& sink = sink_;
  SliceTracker second(&context_);
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  second.SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  auto a = tracker().Scoped(10, kTrack, kNullStringId, StringId::Raw(1), 5);
  auto b = second.Scoped(10, kTrack, kNullStringId, StringId::Raw(1), 5);
  ASSERT_TRUE(a && b);
  EXPECT_NE(*a, *b);
}

// Review issue 7 / finding 3: clearing the sink left queued records behind for
// a null sink_ and flipped WritesTable() under detached inserters.
TEST_F(SliceSinkTest, ClearingTheSinkDeliversQueuedRecordsToIt) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  tracker().Scoped(10, kTrack, kNullStringId, StringId::Raw(1), 5);
  tracker().Scoped(20, kTrack, kNullStringId, StringId::Raw(1), 5);
  tracker().End(30, kTrack);
  tracker().SetSliceSink(nullptr);
  EXPECT_EQ(sink.records.size(), 2u);
}

// Detached ids continue after the table's rows, so a trace that starts in
// table mode and then installs a kInsteadOfTable sink keeps ids unique.
TEST_F(SliceSinkTest, DetachedIdsContinueAfterTableRows) {
  RecordingSink& sink = sink_;
  tracker().Scoped(10, kTrack, kNullStringId, StringId::Raw(1), 5);
  tracker().Scoped(20, kTrack, kNullStringId, StringId::Raw(1), 5);
  tracker().End(30, kTrack);  // Pops the last slice: none is open.
  ASSERT_EQ(context_.storage->slice_table().row_count(), 2u);
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  auto id = tracker().Scoped(40, kTrack, kNullStringId, StringId::Raw(1), 5);
  ASSERT_TRUE(id);
  EXPECT_EQ(id->value, 2u);
}

// The table's next id would be one already handed out as detached.
TEST_F(SliceSinkTest, TableSliceAfterDetachedIdsIsFatal) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  tracker().Scoped(10, kTrack, kNullStringId, StringId::Raw(1), 5);
  tracker().End(30, kTrack);
  tracker().SetSliceSink(nullptr);
  EXPECT_DEATH(tracker().Begin(40, kTrack, kNullStringId, StringId::Raw(1)),
               "next_detached_slice_id");
}

TEST_F(SliceSinkTest, SwitchingTableModeWithAnOpenSliceIsFatal) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1),
                  TranslatableArg());
  EXPECT_DEATH(tracker().SetSliceSink(nullptr), "slice_stack.empty");
}

// Review issue 6: renames used to go straight to the table, leaving the
// tracker's cached name (used for End matching and records) stale.
TEST_F(SliceSinkTest, SetNameUpdatesOpenAndJustEndedSlices) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  auto id = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1));
  EXPECT_TRUE(tracker().SetName(kTrack, *id, StringId::Raw(2)));
  EXPECT_EQ(tracker().End(20, kTrack, kNullStringId, StringId::Raw(2)), id);
  EXPECT_TRUE(tracker().SetName(kTrack, *id, StringId::Raw(3)));
  tracker().FlushPendingSlices();
  ASSERT_EQ(sink.records.size(), 1u);
  EXPECT_EQ(sink.records[0].name, StringId::Raw(3));
  EXPECT_FALSE(tracker().SetName(kTrack, *id, StringId::Raw(4)));
}

TEST_F(SliceSinkTest, SetNameWithTableRenamesRowAndMatching) {
  auto id = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1));
  EXPECT_TRUE(tracker().SetName(kTrack, *id, StringId::Raw(2)));
  EXPECT_EQ(tracker().End(20, kTrack, kNullStringId, StringId::Raw(2)), id);
  EXPECT_EQ(context_.storage->slice_table()[*id].name(), StringId::Raw(2));
}

// Review finding 4: an entry default-inserted by ClosePendingEventsOnTrack
// names slice 0; Step/End must compare against slice 0's real timestamp, as
// upstream does, not against 0.
TEST_F(SliceSinkTest, FlowFromPendingEndUsesSliceZeroTimestamp) {
  FlowTracker flows(&context_);
  tracker().SetOnSliceBeginCallback([&flows](TrackId track, SliceId slice) {
    flows.ClosePendingEventsOnTrack(track, slice);
  });
  constexpr FlowId kFlow = 7;
  auto zero =
      tracker().Scoped(1000, kTrack, kNullStringId, StringId::Raw(1), 1);
  ASSERT_EQ(zero->value, 0u);
  flows.End(kOtherTrack, kFlow, /*bind_enclosing_slice=*/false,
            /*close_flow=*/false);
  tracker().Begin(100, kOtherTrack, kNullStringId, StringId::Raw(2));
  auto later =
      tracker().Begin(200, TrackId{3u}, kNullStringId, StringId::Raw(3));
  flows.Step(*later, kFlow);

  const auto& table = context_.storage->flow_table();
  ASSERT_EQ(table.row_count(), 2u);
  EXPECT_EQ(table[1].slice_out(), *later);
  EXPECT_EQ(table[1].slice_in(), *zero);
}

// Without a table slice 0's start is unknown: the flow keeps existing->new
// and is counted as having no direction, instead of reading a missing row.
TEST_F(SliceSinkTest, FlowFromPendingEndWithoutTableHasNoDirection) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  FlowTracker flows(&context_);
  tracker().SetOnSliceBeginCallback([&flows](TrackId track, SliceId slice) {
    flows.ClosePendingEventsOnTrack(track, slice);
  });
  constexpr FlowId kFlow = 7;
  auto zero =
      tracker().Scoped(1000, kTrack, kNullStringId, StringId::Raw(1), 1);
  ASSERT_EQ(zero->value, 0u);
  flows.End(kOtherTrack, kFlow, /*bind_enclosing_slice=*/false,
            /*close_flow=*/false);
  tracker().Begin(100, kOtherTrack, kNullStringId, StringId::Raw(2));
  auto later =
      tracker().Begin(200, TrackId{3u}, kNullStringId, StringId::Raw(3));
  flows.Step(*later, kFlow);

  const auto& table = context_.storage->flow_table();
  ASSERT_EQ(table.row_count(), 2u);
  EXPECT_EQ(table[1].slice_out(), *zero);
  EXPECT_EQ(table[1].slice_in(), *later);
  EXPECT_EQ(context_.stats_tracker->GetStats(stats::flow_without_direction), 1);
  tracker().SetOnSliceBeginCallback(nullptr);
}

TEST_F(SliceSinkTest, StartTsOfAnOldSliceWithoutTableIsFatal) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  auto old = tracker().Scoped(10, kTrack, kNullStringId, StringId::Raw(1), 5);
  tracker().Scoped(20, kOtherTrack, kNullStringId, StringId::Raw(1), 5);
  tracker().End(30, kOtherTrack);
  EXPECT_DEATH(tracker().StartTsOf(*old), "WritesTable");
}

TEST_F(SliceSinkTest, SinkCallingBackIntoTheTrackerIsFatal) {
  ReentrantSink sink(&tracker());
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  tracker().Scoped(10, kTrack, kNullStringId, StringId::Raw(1), 5);
  tracker().End(20, kTrack);
  EXPECT_DEATH(tracker().Begin(30, kTrack, kNullStringId, StringId::Raw(1)),
               "draining_");
  sink.armed = false;
  tracker().SetSliceSink(nullptr);
}

// Slices still open at flush used to be handed to the sink outside draining_,
// so a sink calling back in could open slices mid-flush.
TEST_F(SliceSinkTest, AlongsideTableSinkCallingBackAtFlushIsFatal) {
  ReentrantSink sink(&tracker());
  tracker().SetSliceSink(&sink, SliceSink::Mode::kAlongsideTable);
  tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1));
  EXPECT_DEATH(tracker().FlushPendingSlices(), "draining_");
  sink.armed = false;
  tracker().SetSliceSink(nullptr);
}

TEST_F(SliceSinkTest, InsteadOfTableOpenSlicesAreDeliveredTopDownAtFlush) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  auto parent = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1));
  auto child = tracker().Begin(20, kTrack, kNullStringId, StringId::Raw(2));
  tracker().FlushPendingSlices();
  ASSERT_EQ(sink.records.size(), 2u);
  EXPECT_EQ(sink.records[0].id, *child);
  EXPECT_EQ(sink.records[1].id, *parent);
  EXPECT_EQ(sink.records[0].dur, -1);
  EXPECT_EQ(sink.records[1].dur, -1);
}

// A slice popped just before flush, then the open ones top-down, each once.
TEST_F(SliceSinkTest, AlongsideTableFlushDeliversEverySliceOnceTopDown) {
  RecordingSink& sink = sink_;
  sink.storage = context_.storage.get();
  tracker().SetSliceSink(&sink, SliceSink::Mode::kAlongsideTable);
  auto parent = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1));
  auto child = tracker().Begin(20, kTrack, kNullStringId, StringId::Raw(2));
  auto popped =
      tracker().Begin(30, kOtherTrack, kNullStringId, StringId::Raw(3));
  tracker().End(35, kOtherTrack);
  tracker().FlushPendingSlices();
  EXPECT_EQ(sink.ids, (std::vector<SliceId>{*popped, *child, *parent}));
  EXPECT_EQ(sink.durs, (std::vector<int64_t>{5, -1, -1}));
}

// With a table, thread timing goes to the row by id as upstream does,
// whichever slice was begun last.
TEST_F(SliceSinkTest, TableThreadTimingTargetsAnyRow) {
  auto a = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1));
  tracker().Begin(20, kOtherTrack, kNullStringId, StringId::Raw(2));
  SliceTracker::ThreadTiming timing;
  timing.ts = 5;
  tracker().SetThreadTiming(*a, timing);
  EXPECT_EQ(context_.storage->slice_table()[*a].thread_ts(), 5);
}

TEST_F(SliceSinkTest, TableThreadDeltasTargetAnyRow) {
  auto a = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1));
  SliceTracker::ThreadTiming timing;
  timing.ts = 5;
  tracker().SetThreadTiming(*a, timing);
  tracker().End(30, kTrack);
  tracker().Begin(40, kOtherTrack, kNullStringId, StringId::Raw(2));
  auto start = tracker().ThreadTimingOfRecentlyEnded(*a);
  ASSERT_TRUE(start);
  EXPECT_EQ(start->ts, 5);
  tracker().SetThreadDeltas(*a, 7, std::nullopt);
  EXPECT_EQ(context_.storage->slice_table()[*a].thread_dur(), 7);
}

// End closed P below the still-open C, so P's deltas land on its stack entry.
TEST_F(SliceSinkTest, InsteadOfTableThreadDeltasReachASliceClosedBelowTheTop) {
  RecordingSink& sink = sink_;
  tracker().SetSliceSink(&sink, SliceSink::Mode::kInsteadOfTable);
  auto p = tracker().Begin(10, kTrack, kNullStringId, StringId::Raw(1));
  auto c = tracker().Begin(20, kTrack, kNullStringId, StringId::Raw(2));
  EXPECT_EQ(tracker().End(30, kTrack, kNullStringId, StringId::Raw(1)), p);
  tracker().SetThreadDeltas(*p, 3, std::nullopt);
  tracker().SetThreadDeltas(*c, std::nullopt, std::nullopt);
  tracker().FlushPendingSlices();
  ASSERT_EQ(sink.records.size(), 2u);
  const FinalizedSlice& r =
      sink.records[0].id == *p ? sink.records[0] : sink.records[1];
  EXPECT_EQ(r.id, *p);
  EXPECT_EQ(r.thread.dur, 3);
}

}  // namespace
}  // namespace perfetto::trace_processor
