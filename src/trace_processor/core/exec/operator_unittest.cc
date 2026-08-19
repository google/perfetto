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

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/from.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/transient_column.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// A source shaped like a dataframe scan: an identity column so every chunk
// carries row indices, plus the values themselves.
class TestSource {
 public:
  explicit TestSource(std::vector<int64_t> values)
      : values_(std::move(values)) {}

  std::unique_ptr<From> Make() {
    std::vector<TransientColumn> columns;
    columns.push_back(
        TransientColumn::Reference(StorageType{Id{}}, nullptr, nullptr));
    columns.push_back(TransientColumn::Reference(StorageType{Int64{}},
                                                 values_.data(), nullptr));
    return std::make_unique<From>(std::move(columns), RowSelection::Range(0),
                                  static_cast<uint32_t>(values_.size()));
  }

 private:
  std::vector<int64_t> values_;
};

// Reads a pipeline the way a consumer would, a row at a time.
// A pipeline with no comparisons in it needs no values to be armed with.
ValueFetcher& NoValues() {
  static ErrorValueFetcher* fetcher = new ErrorValueFetcher();
  return *fetcher;
}

std::vector<uint32_t> Drain(PullPipeline& pipeline,
                            ValueFetcher* values = nullptr) {
  std::vector<uint32_t> rows;
  pipeline.Reset();
  pipeline.Open(values ? *values : NoValues());
  RowCursor cursor(pipeline);
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
    rows.push_back(cursor.row());
  }
  return rows;
}

std::vector<int64_t> Sequence(uint32_t count) {
  std::vector<int64_t> values(count);
  for (uint32_t i = 0; i < count; ++i) {
    values[i] = i;
  }
  return values;
}

// Keeps every second row, standing in for a real operator.
class DropOddRows final : public Operator {
 public:
  OpResult Execute(RowBatch& chunk) override {
    uint32_t count = 0;
    for (uint32_t row = 0; row < chunk.size(); row += 2) {
      selected_[count++] = row;
    }
    return chunk.Slice(RowSelection::Indices(
                           Span<const uint32_t>(selected_, selected_ + count)),
                       count)
               ? OpResult::kContinue
               : OpResult::kDrop;
  }

 private:
  uint32_t selected_[kMaxBatchRows];
};

TEST(OperatorTest, SourceEmitsEveryRow) {
  TestSource source({10, 20, 30});
  std::unique_ptr<From> from = source.Make();
  PullPipeline pipeline(*from, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

TEST(OperatorTest, SourceSplitsIntoChunks) {
  TestSource source(Sequence(kMaxBatchRows * 2 + 3));
  std::unique_ptr<From> from = source.Make();
  PullPipeline pipeline(*from, {});

  std::vector<uint32_t> rows = Drain(pipeline);
  ASSERT_EQ(rows.size(), kMaxBatchRows * 2u + 3u);
  EXPECT_EQ(rows.front(), 0u);
  EXPECT_EQ(rows.back(), kMaxBatchRows * 2u + 2u);
}

TEST(OperatorTest, SourceIsReplayable) {
  TestSource source({10, 20, 30});
  std::unique_ptr<From> from = source.Make();
  PullPipeline pipeline(*from, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

// A source owns one batch and refills it, so a long pipeline allocates no
// per-chunk state at all.
TEST(OperatorTest, SourceReusesOneBatch) {
  TestSource source(Sequence(kMaxBatchRows * 20));
  std::unique_ptr<From> from = source.Make();
  PullPipeline pipeline(*from, {});

  pipeline.Reset();
  RowBatch* first = pipeline.Next();
  ASSERT_NE(first, nullptr);
  uint32_t chunks = 1;
  while (RowBatch* chunk = pipeline.Next()) {
    EXPECT_EQ(chunk, first) << "source handed out a different batch";
    ++chunks;
  }
  EXPECT_EQ(chunks, 20u);
}

// Scratch the filters select into. Chained filters need separate buffers.
std::array<std::array<uint32_t, kMaxBatchRows>, 2>& Scratch() {
  static auto* scratch =
      new std::array<std::array<uint32_t, kMaxBatchRows>, 2>();
  return *scratch;
}

// Hands out one value per index, as the caller of a query would.
class TestValues final : public ValueFetcher {
 public:
  void Set(uint32_t index, int64_t value) {
    values_[index] = value;
    types_[index] = Type::kInt64;
  }
  // A value outside the column's range, which is how a comparison ends up
  // matching every row or none of them.
  void SetDouble(uint32_t index, double value) {
    doubles_[index] = value;
    types_[index] = Type::kDouble;
  }

  Type GetValueType(uint32_t index) override { return types_[index]; }
  int64_t GetInt64Value(uint32_t index) override { return values_[index]; }
  double GetDoubleValue(uint32_t index) override { return doubles_[index]; }
  const char* GetStringValue(uint32_t) override { PERFETTO_FATAL("Not used"); }
  bool IteratorInit(uint32_t) override { PERFETTO_FATAL("Not used"); }
  bool IteratorNext(uint32_t) override { PERFETTO_FATAL("Not used"); }

 private:
  std::array<int64_t, 4> values_{};
  std::array<double, 4> doubles_{};
  std::array<Type, 4> types_{};
};

std::unique_ptr<Operator> GtFilter(uint32_t index = 0) {
  return MakeFilter(1, StorageType{Int64{}}, Op{Gt{}}, index,
                    MakeMutableSpan(Scratch()[0]),
                    /*contiguous_input=*/true);
}

TEST(OperatorTest, FilterSelectsMatchingRows) {
  TestSource source({10, 20, 30, 40});
  std::unique_ptr<From> from = source.Make();
  TestValues values;
  values.Set(0, 20);
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(GtFilter());
  PullPipeline pipeline(*from, std::move(ops));

  EXPECT_THAT(Drain(pipeline, &values), ElementsAre(2, 3));
}

TEST(OperatorTest, AValueEveryRowMatchesSkipsTheComparison) {
  TestSource source({10, 20, 30});
  std::unique_ptr<From> from = source.Make();
  TestValues values;
  // Below every int64, so every row is greater than it.
  values.SetDouble(0, -1e300);
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(GtFilter());
  PullPipeline pipeline(*from, std::move(ops));

  EXPECT_THAT(Drain(pipeline, &values), ElementsAre(0, 1, 2));
}

TEST(OperatorTest, FilterMatchingNothingProducesNoRows) {
  TestSource source({1, 2, 3});
  std::unique_ptr<From> from = source.Make();
  TestValues values;
  values.Set(0, 100);
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(GtFilter());
  PullPipeline pipeline(*from, std::move(ops));

  EXPECT_THAT(Drain(pipeline, &values), IsEmpty());
}

// A value no row could ever match stops the query before a chunk is read.
TEST(OperatorTest, AValueNoRowCanMatchReadsNothing) {
  TestSource source({1, 2, 3});
  std::unique_ptr<From> from = source.Make();
  TestValues values;
  // Above every int64, so no row can be greater than it.
  values.SetDouble(0, 1e300);
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(GtFilter());
  PullPipeline pipeline(*from, std::move(ops));

  EXPECT_THAT(Drain(pipeline, &values), IsEmpty());
}

// Re-running must not leak state between executions, which is the whole point
// of building the tree once and re-arming it.
TEST(OperatorTest, PipelineIsReusableAcrossExecutions) {
  TestSource source({10, 20, 30, 40});
  std::unique_ptr<From> from = source.Make();
  TestValues values;
  values.Set(0, 20);
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(GtFilter());
  PullPipeline pipeline(*from, std::move(ops));

  EXPECT_THAT(Drain(pipeline, &values), ElementsAre(2, 3));
  values.Set(0, 5);
  EXPECT_THAT(Drain(pipeline, &values), ElementsAre(0, 1, 2, 3));
  values.SetDouble(0, -1e300);
  EXPECT_THAT(Drain(pipeline, &values), ElementsAre(0, 1, 2, 3));
}

TEST(OperatorTest, ChainedFiltersCompose) {
  TestSource source({1, 2, 3, 4, 5, 6});
  std::unique_ptr<From> from = source.Make();
  TestValues values;
  values.Set(0, 2);
  values.Set(1, 5);
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(GtFilter());
  ops.push_back(MakeFilter(1, StorageType{Int64{}}, Op{Lt{}}, 1,
                           MakeMutableSpan(Scratch()[1]),
                           /*contiguous_input=*/false));
  PullPipeline pipeline(*from, std::move(ops));

  EXPECT_THAT(Drain(pipeline, &values), ElementsAre(2, 3));
}

TEST(OperatorTest, OperatorNarrowsTheChunk) {
  TestSource source({10, 20, 30, 40, 50});
  std::unique_ptr<From> from = source.Make();
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<DropOddRows>());
  PullPipeline pipeline(*from, std::move(ops));

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 2, 4));
}

TEST(OperatorTest, OperatorsComposeWithinAChunk) {
  TestSource source({0, 1, 2, 3, 4, 5, 6, 7, 8});
  std::unique_ptr<From> from = source.Make();
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<DropOddRows>());
  ops.push_back(std::make_unique<DropOddRows>());
  PullPipeline pipeline(*from, std::move(ops));

  // Keeping every second row twice keeps every fourth.
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 4, 8));
}

TEST(SinkTest, ReadsEveryRowOfEveryChunk) {
  TestSource source(Sequence(kMaxBatchRows * 2 + 7));
  std::unique_ptr<From> from = source.Make();
  PullPipeline pipeline(*from, {});

  std::vector<uint32_t> rows = Drain(pipeline);
  ASSERT_EQ(rows.size(), kMaxBatchRows * 2u + 7u);
  EXPECT_EQ(rows.front(), 0u);
  EXPECT_EQ(rows[kMaxBatchRows], kMaxBatchRows);
  EXPECT_EQ(rows.back(), kMaxBatchRows * 2u + 6u);
}

TEST(SinkTest, ReportsEofWithoutOpen) {
  TestSource source({1, 2, 3});
  std::unique_ptr<From> from = source.Make();
  PullPipeline pipeline(*from, {});
  RowCursor cursor(pipeline);

  EXPECT_TRUE(cursor.eof());
}

TEST(SinkTest, ReopeningRereadsFromTheStart) {
  TestSource source({4, 5, 6});
  std::unique_ptr<From> from = source.Make();
  PullPipeline pipeline(*from, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
