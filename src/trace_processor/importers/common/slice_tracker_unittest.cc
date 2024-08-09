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

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <tuple>
#include <vector>

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;

struct SliceInfo {
  int64_t start;
  int64_t duration;

  bool operator==(const SliceInfo& other) const {
    return std::tie(start, duration) == std::tie(other.start, other.duration);
  }
};

inline void PrintTo(const SliceInfo& info, ::std::ostream* os) {
  *os << "SliceInfo{" << info.start << ", " << info.duration << "}";
}

std::vector<SliceInfo> ToSliceInfo(const tables::SliceTable& slices) {
  std::vector<SliceInfo> infos;
  for (auto it = slices.IterateRows(); it; ++it) {
    infos.emplace_back(SliceInfo{it.ts(), it.dur()});
  }
  return infos;
}

class SliceTrackerTest : public ::testing::Test {
 public:
  SliceTrackerTest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.args_translation_table =
        std::make_unique<ArgsTranslationTable>(context_.storage.get());
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(context_.storage.get());
  }

 protected:
  TraceProcessorContext context_;
};

TEST_F(SliceTrackerTest, OneSliceDetailed) {
  SliceTracker tracker(&context_);

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, kNullStringId /*cat*/,
                StringId::Raw(1) /*name*/);
  tracker.End(10 /*ts*/, track, kNullStringId /*cat*/,
              StringId::Raw(1) /*name*/);

  const auto& slices = context_.storage->slice_table();
  EXPECT_EQ(slices.row_count(), 1u);
  EXPECT_EQ(slices[0].ts(), 2);
  EXPECT_EQ(slices[0].dur(), 8);
  EXPECT_EQ(slices[0].track_id(), track);
  EXPECT_EQ(slices[0].category().value_or(kNullStringId).raw_id(), 0u);
  EXPECT_EQ(slices[0].name().value_or(kNullStringId).raw_id(), 1u);
  EXPECT_EQ(slices[0].depth(), 0u);
  EXPECT_EQ(slices[0].arg_set_id(), kInvalidArgSetId);
}

TEST_F(SliceTrackerTest, OneSliceDetailedWithTranslatedName) {
  SliceTracker tracker(&context_);

  const StringId raw_name = context_.storage->InternString("raw_name");
  const StringId mapped_name = context_.storage->InternString("mapped_name");
  context_.slice_translation_table->AddNameTranslationRule("raw_name",
                                                           "mapped_name");

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, kNullStringId /*cat*/, raw_name /*name*/);
  tracker.End(10 /*ts*/, track, kNullStringId /*cat*/, raw_name /*name*/);

  const auto& slices = context_.storage->slice_table();
  EXPECT_EQ(slices.row_count(), 1u);
  EXPECT_EQ(slices[0].ts(), 2);
  EXPECT_EQ(slices[0].dur(), 8);
  EXPECT_EQ(slices[0].track_id(), track);
  EXPECT_EQ(slices[0].category().value_or(kNullStringId).raw_id(), 0u);
  EXPECT_EQ(slices[0].name().value_or(kNullStringId).raw_id(),
            mapped_name.raw_id());
  EXPECT_EQ(slices[0].depth(), 0u);
  EXPECT_EQ(slices[0].arg_set_id(), kInvalidArgSetId);
}

TEST_F(SliceTrackerTest, NegativeTimestamps) {
  SliceTracker tracker(&context_);

  constexpr TrackId track{22u};
  tracker.Begin(-1000 /*ts*/, track, kNullStringId /*cat*/,
                StringId::Raw(1) /*name*/);
  tracker.End(-501 /*ts*/, track, kNullStringId /*cat*/,
              StringId::Raw(1) /*name*/);

  const auto& slices = context_.storage->slice_table();
  EXPECT_EQ(slices.row_count(), 1u);

  auto rr = slices[0];
  EXPECT_EQ(rr.ts(), -1000);
  EXPECT_EQ(rr.dur(), 499);
  EXPECT_EQ(rr.track_id(), track);
  EXPECT_EQ(rr.category().value_or(kNullStringId).raw_id(), 0u);
  EXPECT_EQ(rr.name().value_or(kNullStringId).raw_id(), 1u);
  EXPECT_EQ(rr.depth(), 0u);
  EXPECT_EQ(rr.arg_set_id(), kInvalidArgSetId);
}

TEST_F(SliceTrackerTest, OneSliceWithArgs) {
  SliceTracker tracker(&context_);

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, kNullStringId /*cat*/,
                StringId::Raw(1) /*name*/,
                [](ArgsTracker::BoundInserter* inserter) {
                  inserter->AddArg(/*flat_key=*/StringId::Raw(1),
                                   /*key=*/StringId::Raw(2),
                                   /*v=*/Variadic::Integer(10));
                });
  tracker.End(10 /*ts*/, track, kNullStringId /*cat*/,
              StringId::Raw(1) /*name*/,
              [](ArgsTracker::BoundInserter* inserter) {
                inserter->AddArg(/*flat_key=*/StringId::Raw(3),
                                 /*key=*/StringId::Raw(4),
                                 /*v=*/Variadic::Integer(20));
              });

  const auto& slices = context_.storage->slice_table();
  EXPECT_EQ(slices.row_count(), 1u);

  auto sr = slices[0];
  EXPECT_EQ(sr.ts(), 2);
  EXPECT_EQ(sr.dur(), 8);
  EXPECT_EQ(sr.track_id(), track);
  EXPECT_EQ(sr.category().value_or(kNullStringId).raw_id(), 0u);
  EXPECT_EQ(sr.name().value_or(kNullStringId).raw_id(), 1u);
  EXPECT_EQ(sr.depth(), 0u);
  auto set_id = sr.arg_set_id();

  const auto& args = context_.storage->arg_table();
  auto ar0 = args[0];
  auto ar1 = args[1];
  EXPECT_EQ(ar0.arg_set_id(), set_id);
  EXPECT_EQ(ar0.flat_key().raw_id(), 1u);
  EXPECT_EQ(ar0.key().raw_id(), 2u);
  EXPECT_EQ(ar0.int_value(), 10);
  EXPECT_EQ(ar1.arg_set_id(), set_id);
  EXPECT_EQ(ar1.flat_key().raw_id(), 3u);
  EXPECT_EQ(ar1.key().raw_id(), 4u);
  EXPECT_EQ(ar1.int_value(), 20);
}

TEST_F(SliceTrackerTest, OneSliceWithArgsWithTranslatedName) {
  SliceTracker tracker(&context_);

  const StringId raw_name = context_.storage->InternString("raw_name");
  const StringId mapped_name = context_.storage->InternString("mapped_name");
  context_.slice_translation_table->AddNameTranslationRule("raw_name",
                                                           "mapped_name");

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, kNullStringId /*cat*/, raw_name /*name*/,
                [](ArgsTracker::BoundInserter* inserter) {
                  inserter->AddArg(/*flat_key=*/StringId::Raw(1),
                                   /*key=*/StringId::Raw(2),
                                   /*v=*/Variadic::Integer(10));
                });
  tracker.End(10 /*ts*/, track, kNullStringId /*cat*/, raw_name /*name*/,
              [](ArgsTracker::BoundInserter* inserter) {
                inserter->AddArg(/*flat_key=*/StringId::Raw(3),
                                 /*key=*/StringId::Raw(4),
                                 /*v=*/Variadic::Integer(20));
              });

  const auto& slices = context_.storage->slice_table();
  EXPECT_EQ(slices.row_count(), 1u);

  auto sr = slices[0];
  EXPECT_EQ(sr.ts(), 2);
  EXPECT_EQ(sr.dur(), 8);
  EXPECT_EQ(sr.track_id(), track);
  EXPECT_EQ(sr.category().value_or(kNullStringId).raw_id(), 0u);
  EXPECT_EQ(sr.name().value_or(kNullStringId).raw_id(), mapped_name.raw_id());
  EXPECT_EQ(sr.depth(), 0u);
  auto set_id = sr.arg_set_id();

  const auto& args = context_.storage->arg_table();
  auto ar0 = args[0];
  auto ar1 = args[1];
  EXPECT_EQ(ar0.arg_set_id(), set_id);
  EXPECT_EQ(ar0.flat_key().raw_id(), 1u);
  EXPECT_EQ(ar0.key().raw_id(), 2u);
  EXPECT_EQ(ar0.int_value(), 10);
  EXPECT_EQ(ar1.arg_set_id(), set_id);
  EXPECT_EQ(ar1.flat_key().raw_id(), 3u);
  EXPECT_EQ(ar1.key().raw_id(), 4u);
  EXPECT_EQ(ar1.int_value(), 20);
}

TEST_F(SliceTrackerTest, TwoSliceDetailed) {
  SliceTracker tracker(&context_);

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, kNullStringId /*cat*/,
                StringId::Raw(1) /*name*/);
  tracker.Begin(3 /*ts*/, track, kNullStringId /*cat*/,
                StringId::Raw(2) /*name*/);
  tracker.End(5 /*ts*/, track);
  tracker.End(10 /*ts*/, track);

  const auto& slices = context_.storage->slice_table();

  EXPECT_EQ(slices.row_count(), 2u);

  auto sr0 = slices[0];
  EXPECT_EQ(sr0.ts(), 2);
  EXPECT_EQ(sr0.dur(), 8);
  EXPECT_EQ(sr0.track_id(), track);
  EXPECT_EQ(sr0.category().value_or(kNullStringId).raw_id(), 0u);
  EXPECT_EQ(sr0.name().value_or(kNullStringId).raw_id(), 1u);
  EXPECT_EQ(sr0.depth(), 0u);
  EXPECT_EQ(sr0.parent_stack_id(), 0);

  auto sr1 = slices[1];
  EXPECT_EQ(sr1.ts(), 3);
  EXPECT_EQ(sr1.dur(), 2);
  EXPECT_EQ(sr1.track_id(), track);
  EXPECT_EQ(sr1.category().value_or(kNullStringId).raw_id(), 0u);
  EXPECT_EQ(sr1.name().value_or(kNullStringId).raw_id(), 2u);
  EXPECT_EQ(sr1.depth(), 1u);
  EXPECT_NE(sr1.stack_id(), 0);

  EXPECT_EQ(sr0.stack_id(), sr1.parent_stack_id());
}

TEST_F(SliceTrackerTest, Scoped) {
  SliceTracker tracker(&context_);

  constexpr TrackId track{22u};
  tracker.Begin(0 /*ts*/, track, kNullStringId, kNullStringId);
  tracker.Begin(1 /*ts*/, track, kNullStringId, kNullStringId);
  tracker.Scoped(2 /*ts*/, track, kNullStringId, kNullStringId, 6);
  tracker.End(9 /*ts*/, track);
  tracker.End(10 /*ts*/, track);

  auto slices = ToSliceInfo(context_.storage->slice_table());
  EXPECT_THAT(slices,
              ElementsAre(SliceInfo{0, 10}, SliceInfo{1, 8}, SliceInfo{2, 6}));
}

TEST_F(SliceTrackerTest, ScopedWithTranslatedName) {
  SliceTracker tracker(&context_);

  const StringId raw_name = context_.storage->InternString("raw_name");
  context_.slice_translation_table->AddNameTranslationRule("raw_name",
                                                           "mapped_name");

  constexpr TrackId track{22u};
  tracker.Begin(0 /*ts*/, track, kNullStringId, raw_name);
  tracker.Begin(1 /*ts*/, track, kNullStringId, raw_name);
  tracker.Scoped(2 /*ts*/, track, kNullStringId, raw_name, 6);
  tracker.End(9 /*ts*/, track);
  tracker.End(10 /*ts*/, track);

  auto slices = ToSliceInfo(context_.storage->slice_table());
  EXPECT_THAT(slices,
              ElementsAre(SliceInfo{0, 10}, SliceInfo{1, 8}, SliceInfo{2, 6}));
}

TEST_F(SliceTrackerTest, ParentId) {
  SliceTracker tracker(&context_);

  constexpr TrackId track{22u};
  tracker.Begin(100, track, kNullStringId, kNullStringId);
  tracker.Begin(101, track, kNullStringId, kNullStringId);
  tracker.Begin(102, track, kNullStringId, kNullStringId);
  tracker.End(103, track);
  tracker.End(150, track);
  tracker.End(200, track);

  SliceId parent = context_.storage->slice_table()[0].id();
  SliceId child = context_.storage->slice_table()[1].id();
  EXPECT_THAT(context_.storage->slice_table().parent_id().ToVectorForTesting(),
              ElementsAre(std::nullopt, parent, child));
}

TEST_F(SliceTrackerTest, IgnoreMismatchedEnds) {
  SliceTracker tracker(&context_);

  constexpr TrackId track{22u};
  tracker.Begin(2 /*ts*/, track, StringId::Raw(5) /*cat*/,
                StringId::Raw(1) /*name*/);
  tracker.End(3 /*ts*/, track, StringId::Raw(1) /*cat*/,
              StringId::Raw(1) /*name*/);
  tracker.End(4 /*ts*/, track, kNullStringId /*cat*/,
              StringId::Raw(2) /*name*/);
  tracker.End(5 /*ts*/, track, StringId::Raw(5) /*cat*/,
              StringId::Raw(1) /*name*/);

  auto slices = ToSliceInfo(context_.storage->slice_table());
  EXPECT_THAT(slices, ElementsAre(SliceInfo{2, 3}));
}

TEST_F(SliceTrackerTest, ZeroLengthScoped) {
  SliceTracker tracker(&context_);

  // Bug scenario: the second zero-length scoped slice prevents the first slice
  // from being closed, leading to an inconsistency when we try to insert the
  // final slice and it doesn't intersect with the still pending first slice.
  constexpr TrackId track{22u};
  tracker.Scoped(2 /*ts*/, track, kNullStringId /*cat*/,
                 StringId::Raw(1) /*name*/, 10 /* dur */);
  tracker.Scoped(2 /*ts*/, track, kNullStringId /*cat*/,
                 StringId::Raw(1) /*name*/, 0 /* dur */);
  tracker.Scoped(12 /*ts*/, track, kNullStringId /*cat*/,
                 StringId::Raw(1) /*name*/, 1 /* dur */);
  tracker.Scoped(13 /*ts*/, track, kNullStringId /*cat*/,
                 StringId::Raw(1) /*name*/, 1 /* dur */);

  auto slices = ToSliceInfo(context_.storage->slice_table());
  EXPECT_THAT(slices, ElementsAre(SliceInfo{2, 10}, SliceInfo{2, 0},
                                  SliceInfo{12, 1}, SliceInfo{13, 1}));
}

TEST_F(SliceTrackerTest, DifferentTracks) {
  SliceTracker tracker(&context_);

  constexpr TrackId track_a{22u};
  constexpr TrackId track_b{23u};
  tracker.Begin(0 /*ts*/, track_a, kNullStringId, kNullStringId);
  tracker.Scoped(2 /*ts*/, track_b, kNullStringId, kNullStringId, 6);
  tracker.Scoped(3 /*ts*/, track_b, kNullStringId, kNullStringId, 4);
  tracker.End(10 /*ts*/, track_a);
  tracker.FlushPendingSlices();

  const auto& table = context_.storage->slice_table();
  auto slices = ToSliceInfo(table);
  EXPECT_THAT(slices,
              ElementsAre(SliceInfo{0, 10}, SliceInfo{2, 6}, SliceInfo{3, 4}));

  EXPECT_EQ(table[0].track_id(), track_a);
  EXPECT_EQ(table[1].track_id(), track_b);
  EXPECT_EQ(table[2].track_id(), track_b);
  EXPECT_EQ(table[0].depth(), 0u);
  EXPECT_EQ(table[1].depth(), 0u);
  EXPECT_EQ(table[2].depth(), 1u);
}

TEST_F(SliceTrackerTest, EndEventOutOfOrder) {
  SliceTracker tracker(&context_);

  constexpr TrackId track{22u};
  tracker.Scoped(50 /*ts*/, track, StringId::Raw(11) /*cat*/,
                 StringId::Raw(21) /*name*/, 100 /*dur*/);
  tracker.Begin(100 /*ts*/, track, StringId::Raw(12) /*cat*/,
                StringId::Raw(22) /*name*/);

  // This slice should now have depth 0.
  tracker.Scoped(450 /*ts*/, track, StringId::Raw(12) /*cat*/,
                 StringId::Raw(22) /*name*/, 100 /*dur*/);

  // This slice should be ignored.
  tracker.End(500 /*ts*/, track, StringId::Raw(12) /*cat*/,
              StringId::Raw(22) /*name*/);

  tracker.Begin(800 /*ts*/, track, StringId::Raw(13) /*cat*/,
                StringId::Raw(23) /*name*/);
  // Null cat and name matches everything.
  tracker.End(1000 /*ts*/, track, kNullStringId /*cat*/,
              kNullStringId /*name*/);

  // Slice will not close if category is different.
  tracker.Begin(1100 /*ts*/, track, StringId::Raw(11) /*cat*/,
                StringId::Raw(21) /*name*/);
  tracker.End(1200 /*ts*/, track, StringId::Raw(12) /*cat*/,
              StringId::Raw(21) /*name*/);

  // Slice will not close if name is different.
  tracker.Begin(1300 /*ts*/, track, StringId::Raw(11) /*cat*/,
                StringId::Raw(21) /*name*/);
  tracker.End(1400 /*ts*/, track, StringId::Raw(11) /*cat*/,
              StringId::Raw(22) /*name*/);

  tracker.FlushPendingSlices();

  const auto& st = context_.storage->slice_table();
  auto slices = ToSliceInfo(st);
  EXPECT_THAT(slices, ElementsAre(SliceInfo{50, 100}, SliceInfo{100, 50},
                                  SliceInfo{450, 100}, SliceInfo{800, 200},
                                  SliceInfo{1100, -1}, SliceInfo{1300, 0 - 1}));

  EXPECT_EQ(st[0].depth(), 0u);
  EXPECT_EQ(st[1].depth(), 1u);
  EXPECT_EQ(st[2].depth(), 0u);
  EXPECT_EQ(st[3].depth(), 0u);
}

TEST_F(SliceTrackerTest, GetTopmostSliceOnTrack) {
  SliceTracker tracker(&context_);

  TrackId track{1u};
  TrackId track2{2u};

  EXPECT_EQ(tracker.GetTopmostSliceOnTrack(track), std::nullopt);

  tracker.Begin(100, track, StringId::Raw(11), StringId::Raw(11));
  SliceId slice1 = context_.storage->slice_table()[0].id();

  EXPECT_EQ(tracker.GetTopmostSliceOnTrack(track).value(), slice1);

  tracker.Begin(120, track, StringId::Raw(22), StringId::Raw(22));
  SliceId slice2 = context_.storage->slice_table()[1].id();

  EXPECT_EQ(tracker.GetTopmostSliceOnTrack(track).value(), slice2);

  EXPECT_EQ(tracker.GetTopmostSliceOnTrack(track2), std::nullopt);

  tracker.End(140, track, StringId::Raw(22), StringId::Raw(22));

  EXPECT_EQ(tracker.GetTopmostSliceOnTrack(track).value(), slice1);

  tracker.End(330, track, StringId::Raw(11), StringId::Raw(11));

  EXPECT_EQ(tracker.GetTopmostSliceOnTrack(track), std::nullopt);
}

TEST_F(SliceTrackerTest, OnSliceBeginCallback) {
  SliceTracker tracker(&context_);

  TrackId track1{1u};
  TrackId track2{2u};

  std::vector<TrackId> track_records;
  std::vector<SliceId> slice_records;
  tracker.SetOnSliceBeginCallback([&](TrackId track_id, SliceId slice_id) {
    track_records.emplace_back(track_id);
    slice_records.emplace_back(slice_id);
  });

  EXPECT_TRUE(track_records.empty());
  EXPECT_TRUE(slice_records.empty());

  tracker.Begin(100, track1, StringId::Raw(11), StringId::Raw(11));
  SliceId slice1 = context_.storage->slice_table()[0].id();
  EXPECT_THAT(track_records, ElementsAre(TrackId{1u}));
  EXPECT_THAT(slice_records, ElementsAre(slice1));

  tracker.Begin(120, track2, StringId::Raw(22), StringId::Raw(22));
  SliceId slice2 = context_.storage->slice_table()[1].id();
  EXPECT_THAT(track_records, ElementsAre(TrackId{1u}, TrackId{2u}));
  EXPECT_THAT(slice_records, ElementsAre(slice1, slice2));

  tracker.Begin(330, track1, StringId::Raw(33), StringId::Raw(33));
  SliceId slice3 = context_.storage->slice_table()[2].id();
  EXPECT_THAT(track_records,
              ElementsAre(TrackId{1u}, TrackId{2u}, TrackId{1u}));
  EXPECT_THAT(slice_records, ElementsAre(slice1, slice2, slice3));
}

}  // namespace
}  // namespace perfetto::trace_processor
