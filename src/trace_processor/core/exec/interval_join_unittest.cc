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

#include "src/trace_processor/core/exec/interval_join.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/batch_order.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using R = IntervalRelationship;

constexpr uint32_t kTs = 0;
constexpr uint32_t kDur = 1;
constexpr uint32_t kKey = 2;
constexpr uint32_t kTag = 3;
constexpr uint32_t kColumns = 4;

struct Row {
  std::optional<int64_t> ts;
  std::optional<int64_t> dur;
  std::optional<int64_t> key;
  // Identifies the row in the output.
  int64_t tag;
};

// Emits ts, dur, key and tag, `chunk_rows` rows at a time.
class IntervalSource final : public Source {
 public:
  explicit IntervalSource(std::vector<Row> rows,
                          uint32_t chunk_rows = kMaxBatchRows)
      : rows_(std::move(rows)), chunk_rows_(chunk_rows) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().offset = 0;
  }

  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto total = static_cast<uint32_t>(rows_.size());
    if (s.offset == total) {
      return false;
    }
    uint32_t count = std::min(chunk_rows_, total - s.offset);
    out.Reset();
    for (uint32_t column = 0; column < kColumns; ++column) {
      s.values[column].assign(count, 0);
      s.validity[column] = BitVector::CreateWithSize(count);
      for (uint32_t i = 0; i < count; ++i) {
        const Row& row = rows_[s.offset + i];
        std::optional<int64_t> cells[] = {row.ts, row.dur, row.key, row.tag};
        if (cells[column]) {
          s.values[column][i] = *cells[column];
          s.validity[column].set(i);
        }
      }
      out.AddColumn(ColumnView::Reference(
          StorageType{Int64{}}, s.values[column].data(), &s.validity[column]));
    }
    out.Compose(RowSelection::Range(0), count);
    out.SetCardinality(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    ~State() override;
    uint32_t offset = 0;
    std::vector<int64_t> values[kColumns];
    BitVector validity[kColumns];
  };

  std::vector<Row> rows_;
  uint32_t chunk_rows_;
};

IntervalSource::State::~State() = default;

IntervalJoinSpec Spec(R relationship, bool per_key, bool keep_unmatched) {
  IntervalJoinSpec spec;
  spec.input.ts_column = spec.operand.ts_column = kTs;
  spec.input.dur_column = spec.operand.dur_column = kDur;
  if (per_key) {
    spec.input.key_columns = spec.operand.key_columns = {kKey};
  }
  spec.relationship = relationship;
  spec.operand_column_count = kColumns;
  spec.keep_unmatched = keep_unmatched;
  return spec;
}

// An (input tag, operand tag) pair; the operand tag is null when unmatched.
using Pair = std::pair<int64_t, std::optional<int64_t>>;

std::vector<Pair> Drain(const Source& source, OperatorState& state) {
  std::vector<Pair> pairs;
  RowBatch batch;
  while (source.GetData(batch, state)) {
    EXPECT_EQ(batch.column_count(), 2 * kColumns);
    std::vector<int64_t> input = test::ReadColumn<int64_t>(batch, kTag);
    std::vector<std::optional<int64_t>> operand =
        test::ReadNullableColumn<int64_t>(batch, kColumns + kTag);
    for (uint32_t i = 0; i < batch.size(); ++i) {
      pairs.emplace_back(input[i], operand[i]);
    }
  }
  EXPECT_TRUE(source.status(state).ok()) << source.status(state).message();
  return pairs;
}

// The operators lowering puts in front of a join: the order it requires.
std::vector<std::unique_ptr<Operator>> JoinOperators(const Source& operand,
                                                     IntervalJoinSpec spec) {
  std::vector<uint32_t> order = spec.input.key_columns;
  order.push_back(spec.input.ts_column);
  std::vector<std::unique_ptr<Operator>> operators;
  operators.push_back(std::make_unique<BatchOrder>(std::move(order)));
  operators.push_back(std::make_unique<IntervalJoin>(operand, std::move(spec)));
  return operators;
}

std::vector<Pair> Join(std::vector<Row> input,
                       std::vector<Row> operand,
                       IntervalJoinSpec spec,
                       uint32_t chunk_rows = kMaxBatchRows) {
  IntervalSource input_source(std::move(input), chunk_rows);
  IntervalSource operand_source(std::move(operand), chunk_rows);
  Pipeline pipeline(input_source,
                    JoinOperators(operand_source, std::move(spec)));
  std::unique_ptr<OperatorState> state = pipeline.MakeState();
  return Drain(pipeline, *state);
}

Row Interval(int64_t ts, int64_t dur, int64_t tag, int64_t key = 0) {
  return Row{ts, dur, key, tag};
}

// The operand intervals [0,10) [5,15) [20,30), and a point at 10.
std::vector<Row> Operand() {
  return {Interval(0, 10, 100), Interval(5, 10, 101), Interval(20, 10, 102),
          Interval(10, 0, 103)};
}

TEST(IntervalJoin, OverlappingBounds) {
  std::vector<Pair> pairs =
      Join({Interval(8, 4, 1), Interval(15, 5, 2), Interval(10, 0, 3)},
           Operand(), Spec(R::kOverlappingBounds, false, false));
  // [8,12) shares time with both early intervals and holds the point; [15,20)
  // only touches its neighbours; the point 10 is in [5,15) and is the point.
  EXPECT_THAT(pairs, ElementsAre(Pair(1, 100), Pair(1, 101), Pair(1, 103),
                                 Pair(3, 101), Pair(3, 103)));
}

TEST(IntervalJoin, CoveringBegin) {
  std::vector<Pair> pairs =
      Join({Interval(5, 100, 1), Interval(10, 1, 2), Interval(30, 1, 3)},
           Operand(), Spec(R::kCoveringBegin, false, false));
  // An end is exclusive, so [0,10) does not cover 10 and [20,30) not 30.
  EXPECT_THAT(pairs, ElementsAre(Pair(1, 100), Pair(1, 101), Pair(2, 101),
                                 Pair(2, 103)));
}

TEST(IntervalJoin, CoveringEnd) {
  std::vector<Pair> pairs =
      Join({Interval(-50, 60, 1), Interval(0, 5, 2), Interval(12, 18, 3)},
           Operand(), Spec(R::kCoveringEnd, false, false));
  // Ending exactly where the operand does is still covered; starting where
  // the input ends is not.
  EXPECT_THAT(pairs, ElementsAre(Pair(1, 100), Pair(1, 101), Pair(2, 100),
                                 Pair(3, 102)));
}

TEST(IntervalJoin, CoveringBounds) {
  std::vector<Pair> pairs =
      Join({Interval(5, 5, 1), Interval(5, 6, 2), Interval(20, 10, 3)},
           Operand(), Spec(R::kCoveringBounds, false, false));
  EXPECT_THAT(pairs, ElementsAre(Pair(1, 100), Pair(1, 101), Pair(2, 101),
                                 Pair(3, 102)));
}

TEST(IntervalJoin, WithinBounds) {
  std::vector<Pair> pairs =
      Join({Interval(0, 15, 1), Interval(5, 5, 2), Interval(10, 0, 3)},
           Operand(), Spec(R::kWithinBounds, false, false));
  // The point at 10 is outside [5,10) but is all of the point 10.
  EXPECT_THAT(pairs, ElementsAre(Pair(1, 100), Pair(1, 101), Pair(1, 103),
                                 Pair(3, 103)));
}

TEST(IntervalJoin, NegativeDurationNeverEnds) {
  std::vector<Pair> pairs = Join({Interval(1000, 1, 1), Interval(3, -1, 2)},
                                 {Interval(0, -1, 100), Interval(5, 1, 101)},
                                 Spec(R::kOverlappingBounds, false, false));
  // Rows come out by their ts.
  EXPECT_THAT(pairs, ElementsAre(Pair(2, 100), Pair(2, 101), Pair(1, 100)));
}

TEST(IntervalJoin, KeysMustMatch) {
  std::vector<Pair> pairs =
      Join({Interval(0, 10, 1, 7), Interval(0, 10, 2, 8), Interval(0, 10, 3, 9),
            Row{0, 10, std::nullopt, 4}},
           {Interval(0, 10, 100, 8), Interval(0, 10, 101, 7),
            Interval(0, 10, 102, 8), Row{0, 10, std::nullopt, 103}},
           Spec(R::kOverlappingBounds, true, false));
  // A null key is equal to nothing, itself included.
  EXPECT_THAT(pairs, ElementsAre(Pair(1, 101), Pair(2, 100), Pair(2, 102)));
}

TEST(IntervalJoin, KeysOfSeveralColumns) {
  IntervalJoinSpec spec = Spec(R::kOverlappingBounds, false, false);
  spec.input.key_columns = spec.operand.key_columns = {kKey, kDur};
  std::vector<Pair> pairs =
      Join({Interval(0, 10, 1, 7), Interval(0, 20, 2, 7)},
           {Interval(0, 20, 100, 7), Interval(0, 10, 101, 7),
            Interval(0, 10, 102, 8)},
           std::move(spec));
  EXPECT_THAT(pairs, ElementsAre(Pair(1, 101), Pair(2, 100)));
}

TEST(IntervalJoin, NullBoundsJoinNothing) {
  std::vector<Pair> pairs =
      Join({Row{std::nullopt, 10, 0, 1}, Row{0, std::nullopt, 0, 2},
            Interval(0, 10, 3)},
           {Row{std::nullopt, 10, 0, 100}, Row{0, std::nullopt, 0, 101},
            Interval(0, 10, 102)},
           Spec(R::kOverlappingBounds, false, false));
  EXPECT_THAT(pairs, ElementsAre(Pair(3, 102)));
}

TEST(IntervalJoin, KeepUnmatched) {
  std::vector<Pair> pairs =
      Join({Interval(100, 1, 1), Interval(0, 1, 2), Row{std::nullopt, 1, 0, 3}},
           Operand(), Spec(R::kOverlappingBounds, false, true));
  // Rows come out by their ts, a null ts first.
  EXPECT_THAT(pairs, ElementsAre(Pair(3, std::nullopt), Pair(2, 100),
                                 Pair(1, std::nullopt)));
}

TEST(IntervalJoin, KeepUnmatchedWithEmptyOperand) {
  IntervalSource input({Interval(0, 1, 1), Interval(5, 1, 2)});
  IntervalSource operand({});
  std::vector<std::unique_ptr<Operator>> operators;
  operators.push_back(std::make_unique<IntervalJoin>(
      operand, Spec(R::kOverlappingBounds, false, true)));
  Pipeline pipeline(input, std::move(operators));
  std::unique_ptr<OperatorState> state = pipeline.MakeState();

  RowBatch batch;
  ASSERT_TRUE(pipeline.GetData(batch, *state));
  ASSERT_EQ(batch.column_count(), 2 * kColumns);
  EXPECT_THAT(test::ReadColumn<int64_t>(batch, kTag), ElementsAre(1, 2));
  // The operand never said what its columns hold, so they carry no type.
  const ColumnView& tag = batch.column(kColumns + kTag);
  ASSERT_EQ(tag.kind(), ColumnView::Kind::kVariant);
  EXPECT_EQ(tag.Value<Variant>(0).type, Variant::Type::kNull);
  EXPECT_EQ(tag.Value<Variant>(1).type, Variant::Type::kNull);
  EXPECT_FALSE(pipeline.GetData(batch, *state));
}

TEST(IntervalJoin, EmptyOperandJoinsNothing) {
  EXPECT_THAT(
      Join({Interval(0, 1, 1)}, {}, Spec(R::kOverlappingBounds, false, false)),
      IsEmpty());
  EXPECT_THAT(
      Join({Interval(0, 1, 1)}, {}, Spec(R::kOverlappingBounds, true, false)),
      IsEmpty());
}

TEST(IntervalJoin, OneRowWithMoreMatchesThanABatchHolds) {
  constexpr int64_t kOperandRows = 3 * kMaxBatchRows + 17;
  std::vector<Row> operand;
  for (int64_t i = 0; i < kOperandRows; ++i) {
    operand.push_back(Interval(i, 1, i));
  }
  std::vector<Pair> pairs = Join(
      {Interval(-5, 1, 1), Interval(0, kOperandRows, 2), Interval(10, 2, 3)},
      std::move(operand), Spec(R::kOverlappingBounds, false, true));
  ASSERT_EQ(pairs.size(), static_cast<size_t>(1 + kOperandRows + 2));
  EXPECT_EQ(pairs.front(), Pair(1, std::nullopt));
  for (int64_t i = 0; i < kOperandRows; ++i) {
    ASSERT_EQ(pairs[1 + static_cast<size_t>(i)], Pair(2, i));
  }
  EXPECT_EQ(pairs[pairs.size() - 2], Pair(3, 10));
  EXPECT_EQ(pairs.back(), Pair(3, 11));
}

TEST(IntervalJoin, RewindRunsTheJoinAgain) {
  IntervalSource input({Interval(8, 4, 1)});
  IntervalSource operand(Operand());
  std::vector<std::unique_ptr<Operator>> operators;
  operators.push_back(std::make_unique<IntervalJoin>(
      operand, Spec(R::kCoveringBegin, false, false)));
  Pipeline pipeline(input, std::move(operators));
  std::unique_ptr<OperatorState> state = pipeline.MakeState();

  EXPECT_THAT(Drain(pipeline, *state), ElementsAre(Pair(1, 100), Pair(1, 101)));
  pipeline.Rewind(*state);
  EXPECT_THAT(Drain(pipeline, *state), ElementsAre(Pair(1, 100), Pair(1, 101)));
}

TEST(IntervalJoin, ColumnsOfTheWrongTypeAreAnError) {
  test::ArraySource input(test::Sequence(4));
  IntervalSource operand(Operand());
  IntervalJoinSpec spec = Spec(R::kOverlappingBounds, false, false);
  // Column 0 of an ArraySource is an Id column.
  spec.input.ts_column = 0;
  spec.input.dur_column = 1;
  std::vector<std::unique_ptr<Operator>> operators;
  operators.push_back(std::make_unique<IntervalJoin>(operand, std::move(spec)));
  Pipeline pipeline(input, std::move(operators));
  std::unique_ptr<OperatorState> state = pipeline.MakeState();

  RowBatch batch;
  EXPECT_FALSE(pipeline.GetData(batch, *state));
  EXPECT_FALSE(pipeline.status(*state).ok());
}

// Whether the operand interval [a, b) relates to the input interval [s, e),
// written out from the definitions.
bool Relates(R relationship, int64_t s, int64_t e, int64_t a, int64_t b) {
  auto contains = [&](int64_t p) { return a == b ? a == p : a <= p && p < b; };
  if (s == e && relationship != R::kWithinBounds) {
    return contains(s);
  }
  switch (relationship) {
    case R::kOverlappingBounds:
      return a == b ? s <= a && a < e : a < e && s < b;
    case R::kCoveringBegin:
      return contains(s);
    case R::kCoveringEnd:
      return a < e && e <= b;
    case R::kCoveringBounds:
      return a <= s && e <= b;
    case R::kWithinBounds:
      return s <= a && b <= e && (a < e || s == e);
  }
  return false;
}

// The pairs the definitions say `input` and `operand` make, in the order the
// join emits them: by input row as BatchOrder leaves them within a batch of
// `chunk_rows`, then operand start, then operand row. With `keep_unmatched`
// a row which makes no pair makes one with a null.
std::vector<Pair> Expected(R relationship,
                           const std::vector<Row>& input,
                           const std::vector<Row>& operand,
                           uint32_t chunk_rows = kMaxBatchRows,
                           bool keep_unmatched = false) {
  std::vector<const Row*> probes;
  for (const Row& row : input) {
    probes.push_back(&row);
  }
  auto less = [](const Row* a, const Row* b) {
    if (a->key != b->key) {
      return !a->key || (b->key && *a->key < *b->key);
    }
    return a->ts != b->ts ? !a->ts || (b->ts && *a->ts < *b->ts) : false;
  };
  for (size_t at = 0; at < probes.size(); at += chunk_rows) {
    auto end = probes.begin() + static_cast<long>(std::min<size_t>(
                                    at + chunk_rows, probes.size()));
    std::stable_sort(probes.begin() + static_cast<long>(at), end, less);
  }
  std::vector<Pair> expected;
  for (const Row* probe : probes) {
    const Row& in = *probe;
    std::vector<const Row*> matches;
    for (const Row& op : operand) {
      if (*in.key == *op.key && Relates(relationship, *in.ts, *in.ts + *in.dur,
                                        *op.ts, *op.ts + *op.dur)) {
        matches.push_back(&op);
      }
    }
    std::stable_sort(
        matches.begin(), matches.end(),
        [](const Row* a, const Row* b) { return *a->ts < *b->ts; });
    for (const Row* op : matches) {
      expected.emplace_back(in.tag, op->tag);
    }
    if (matches.empty() && keep_unmatched) {
      expected.emplace_back(in.tag, std::nullopt);
    }
  }
  return expected;
}

// Short, long and zero durations, packed tightly enough that starts and ends
// coincide often.
std::vector<Row> RandomRows(std::minstd_rand& rng,
                            uint32_t count,
                            int64_t first_tag,
                            uint32_t keys) {
  std::vector<Row> rows;
  for (uint32_t i = 0; i < count; ++i) {
    int64_t ts = static_cast<int64_t>(rng() % 200);
    int64_t dur = rng() % 4 == 0 ? 0 : static_cast<int64_t>(rng() % 60);
    rows.push_back(
        Interval(ts, dur, first_tag + i, static_cast<int64_t>(rng() % keys)));
  }
  return rows;
}

// Rows of one key none of which starts before another ends, with gaps, rows
// which touch, points between and at the ends of intervals, and repeated
// points. They arrive in no order.
std::vector<Row> RandomDisjointRows(std::minstd_rand& rng,
                                    uint32_t count,
                                    int64_t first_tag,
                                    int64_t key) {
  std::vector<Row> rows;
  int64_t ts = 0;
  bool after_point = false;
  for (uint32_t i = 0; i < count; ++i) {
    ts += rng() % 3 == 0 ? static_cast<int64_t>(rng() % 4) : 0;
    int64_t dur = rng() % 3 == 0 ? 0 : static_cast<int64_t>(1 + rng() % 5);
    // An interval starting at a point contains it.
    if (after_point && dur != 0 && *rows.back().ts == ts) {
      ++ts;
    }
    rows.push_back(Interval(ts, dur, first_tag + i, key));
    after_point = dur == 0;
    ts += dur;
  }
  std::shuffle(rows.begin(), rows.end(), rng);
  return rows;
}

constexpr R kRelationships[] = {R::kOverlappingBounds, R::kCoveringBegin,
                                R::kCoveringEnd, R::kCoveringBounds,
                                R::kWithinBounds};

TEST(IntervalJoin, MatchesTheDefinitionsOnRandomIntervals) {
  std::minstd_rand rng(42);
  for (R relationship : kRelationships) {
    std::vector<Row> input = RandomRows(rng, 300, 0, 3);
    std::vector<Row> operand = RandomRows(rng, 400, 1000, 3);
    // Small chunks, so the operand spans batches and the output more.
    EXPECT_EQ(Join(input, operand, Spec(relationship, true, false), 64),
              Expected(relationship, input, operand, 64))
        << "relationship " << static_cast<int>(relationship);
  }
}

// The join needs its probes grouped by key and in time order, which the
// BatchOrder in front of it establishes from any order; LEFT, so that every
// probe appears.
TEST(IntervalJoin, UnorderedInputIsOrderedFirst) {
  std::minstd_rand rng(7);
  std::vector<Row> input = RandomRows(rng, 500, 0, 4);
  std::vector<Row> operand = RandomRows(rng, 300, 1000, 4);
  for (R relationship : kRelationships) {
    EXPECT_EQ(Join(input, operand, Spec(relationship, true, true), 128),
              Expected(relationship, input, operand, 128, true))
        << "relationship " << static_cast<int>(relationship);
  }
}

// Without the order in front of it, the join reports probes it cannot
// follow rather than dropping their matches.
TEST(IntervalJoin, UnorderedProbesAreAnError) {
  IntervalSource input({Interval(10, 1, 1), Interval(5, 1, 2)});
  IntervalSource operand(Operand());
  std::vector<std::unique_ptr<Operator>> operators;
  operators.push_back(std::make_unique<IntervalJoin>(
      operand, Spec(R::kCoveringBegin, false, false)));
  Pipeline pipeline(input, std::move(operators));
  std::unique_ptr<OperatorState> state = pipeline.MakeState();

  RowBatch batch;
  EXPECT_FALSE(pipeline.GetData(batch, *state));
  EXPECT_THAT(pipeline.status(*state).message(),
              testing::HasSubstr("not ordered by PER keys then ts"));
}

TEST(IntervalJoin, MatchesTheDefinitionsOnDisjointOperands) {
  std::minstd_rand rng(7);
  for (R relationship : kRelationships) {
    std::vector<Row> input = RandomRows(rng, 400, 0, 1);
    std::vector<Row> operand = RandomDisjointRows(rng, 120, 1000, 0);
    EXPECT_EQ(Join(input, operand, Spec(relationship, true, false), 64),
              Expected(relationship, input, operand, 64))
        << "relationship " << static_cast<int>(relationship);
    // Without a key there is one lane, which is the disjoint one.
    EXPECT_EQ(Join(input, operand, Spec(relationship, false, false), 64),
              Expected(relationship, input, operand, 64))
        << "relationship " << static_cast<int>(relationship);
  }
}

TEST(IntervalJoin, MatchesTheDefinitionsOnDisjointAndOverlappingLanes) {
  std::minstd_rand rng(11);
  for (R relationship : kRelationships) {
    std::vector<Row> input = RandomRows(rng, 500, 0, 3);
    // Key 0 and 2 are disjoint lanes, key 1 is not.
    std::vector<Row> operand = RandomDisjointRows(rng, 100, 1000, 0);
    for (const Row& row : RandomRows(rng, 150, 2000, 1)) {
      operand.push_back(Interval(*row.ts, *row.dur, row.tag, 1));
    }
    for (const Row& row : RandomDisjointRows(rng, 100, 3000, 2)) {
      operand.push_back(row);
    }
    std::shuffle(operand.begin(), operand.end(), rng);
    EXPECT_EQ(Join(input, operand, Spec(relationship, true, false), 64),
              Expected(relationship, input, operand, 64))
        << "relationship " << static_cast<int>(relationship);
  }
}

TEST(IntervalJoin, ADisjointLaneCanEndInARowWhichNeverEnds) {
  std::vector<Pair> pairs =
      Join({Interval(3, 0, 1), Interval(12, 100, 2), Interval(9, 2, 3)},
           {Interval(0, 5, 100), Interval(5, 5, 101), Interval(10, -1, 102)},
           Spec(R::kOverlappingBounds, false, false));
  EXPECT_THAT(pairs, ElementsAre(Pair(1, 100), Pair(3, 101), Pair(3, 102),
                                 Pair(2, 102)));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
