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

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/from.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

// A source shaped like a dataframe scan: an identity column so every batch
// carries row indices, plus the values themselves.
class TestSource {
 public:
  explicit TestSource(std::vector<int64_t> values)
      : values_(std::move(values)) {}

  std::unique_ptr<From> Create() {
    std::vector<ColumnView> columns;
    columns.push_back(
        ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr));
    columns.push_back(
        ColumnView::Reference(StorageType{Int64{}}, values_.data(), nullptr));
    return std::make_unique<From>(std::move(columns), RowSelection::Range(0),
                                  static_cast<uint32_t>(values_.size()));
  }

 private:
  std::vector<int64_t> values_;
};

// Reads a pipeline the way a consumer would, a row at a time.
std::vector<uint32_t> Drain(PullPipeline& pipeline) {
  std::vector<uint32_t> rows;
  pipeline.Reset();
  RowCursor cursor(pipeline);
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
    rows.push_back(cursor.row(0));
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
  OpResult Execute(RowBatch& batch) override {
    uint32_t count = 0;
    for (uint32_t row = 0; row < batch.size(); row += 2) {
      selected_[count++] = row;
    }
    return batch.Slice(RowSelection::Indices(
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
  std::unique_ptr<From> from = source.Create();
  PullPipeline pipeline(*from, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

TEST(OperatorTest, SourceSplitsIntoBatches) {
  TestSource source(Sequence(kMaxBatchRows * 2 + 3));
  std::unique_ptr<From> from = source.Create();
  PullPipeline pipeline(*from, {});

  std::vector<uint32_t> rows = Drain(pipeline);
  ASSERT_EQ(rows.size(), kMaxBatchRows * 2u + 3u);
  EXPECT_EQ(rows.front(), 0u);
  EXPECT_EQ(rows.back(), kMaxBatchRows * 2u + 2u);
}

TEST(OperatorTest, SourceIsReplayable) {
  TestSource source({10, 20, 30});
  std::unique_ptr<From> from = source.Create();
  PullPipeline pipeline(*from, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

// A source owns one batch and refills it, so a long pipeline allocates no
// per-batch state at all.
TEST(OperatorTest, SourceReusesOneBatch) {
  TestSource source(Sequence(kMaxBatchRows * 20));
  std::unique_ptr<From> from = source.Create();
  PullPipeline pipeline(*from, {});

  pipeline.Reset();
  RowBatch* first = pipeline.Next();
  ASSERT_NE(first, nullptr);
  uint32_t batches = 1;
  while (RowBatch* batch = pipeline.Next()) {
    EXPECT_EQ(batch, first) << "source handed out a different batch";
    ++batches;
  }
  EXPECT_EQ(batches, 20u);
}

TEST(OperatorTest, OperatorNarrowsTheBatch) {
  TestSource source({10, 20, 30, 40, 50});
  std::unique_ptr<From> from = source.Create();
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<DropOddRows>());
  PullPipeline pipeline(*from, std::move(ops));

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 2, 4));
}

TEST(OperatorTest, OperatorsComposeWithinABatch) {
  TestSource source({0, 1, 2, 3, 4, 5, 6, 7, 8});
  std::unique_ptr<From> from = source.Create();
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<DropOddRows>());
  ops.push_back(std::make_unique<DropOddRows>());
  PullPipeline pipeline(*from, std::move(ops));

  // Keeping every second row twice keeps every fourth.
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 4, 8));
}

TEST(OperatorTest, NarrowingAComposedViewKeepsIt) {
  TestSource source(Sequence(17));
  std::unique_ptr<From> from = source.Create();
  std::vector<std::unique_ptr<Operator>> ops;
  for (int i = 0; i < 3; ++i) {
    ops.push_back(std::make_unique<DropOddRows>());
  }
  PullPipeline pipeline(*from, std::move(ops));

  // Keeping every second row three times keeps every eighth. The third
  // operator narrows a view the batch already composed, onto itself.
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 8, 16));
}

// The storage a batch composes its views into belongs to the batch and is
// handed back for the next one, so a pipeline stops allocating once it is
// running.
TEST(OperatorTest, ComposedViewsReuseTheBatchesStorage) {
  TestSource source(Sequence(kMaxBatchRows * 4));
  std::unique_ptr<From> from = source.Create();
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<DropOddRows>());
  ops.push_back(std::make_unique<DropOddRows>());
  PullPipeline pipeline(*from, std::move(ops));

  pipeline.Reset();
  RowBatch* batch = pipeline.Next();
  ASSERT_NE(batch, nullptr);
  const uint32_t* block = batch->column(0).selection().data();
  ASSERT_NE(block, nullptr) << "expected a composed view";
  uint32_t batches = 1;
  while ((batch = pipeline.Next()) != nullptr) {
    EXPECT_EQ(batch->column(0).selection().data(), block);
    ++batches;
  }
  EXPECT_EQ(batches, 4u);
}

TEST(SinkTest, ReadsEveryRowOfEveryBatch) {
  TestSource source(Sequence(kMaxBatchRows * 2 + 7));
  std::unique_ptr<From> from = source.Create();
  PullPipeline pipeline(*from, {});

  std::vector<uint32_t> rows = Drain(pipeline);
  ASSERT_EQ(rows.size(), kMaxBatchRows * 2u + 7u);
  EXPECT_EQ(rows.front(), 0u);
  EXPECT_EQ(rows[kMaxBatchRows], kMaxBatchRows);
  EXPECT_EQ(rows.back(), kMaxBatchRows * 2u + 6u);
}

// An operator which adds a computed column cannot put it in the index space
// its input arrived in, so it adds it in its own. A cursor has to follow each
// column's own view to read either of them.
TEST(SinkTest, ReadsColumnsWhichDoNotShareARowView) {
  std::vector<int64_t> payload = {10, 11, 12, 13};
  std::vector<int64_t> computed = {90, 91};

  class TwoViewSource final : public Source {
   public:
    TwoViewSource(const std::vector<int64_t>* payload,
                  const std::vector<int64_t>* computed)
        : payload_(payload), computed_(computed) {}

    RowBatch* Next() override {
      if (done_) {
        return nullptr;
      }
      done_ = true;
      batch_.AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, payload_->data()));
      // The payload is read from half way in; the computed column is its own
      // array read from the start.
      batch_.Compose(RowSelection::Range(2), 2);
      batch_.AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, computed_->data()));
      batch_.SetCardinality(2);
      return &batch_;
    }

   private:
    const std::vector<int64_t>* payload_;
    const std::vector<int64_t>* computed_;
    RowBatch batch_;
    bool done_ = false;
  };

  TwoViewSource source(&payload, &computed);
  RowCursor cursor(source);
  std::vector<int64_t> read_payload;
  std::vector<int64_t> read_computed;
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
    read_payload.push_back(static_cast<const int64_t*>(
        cursor.batch().column(0).data())[cursor.row(0)]);
    read_computed.push_back(static_cast<const int64_t*>(
        cursor.batch().column(1).data())[cursor.row(1)]);
  }
  EXPECT_THAT(read_payload, ElementsAre(12, 13));
  EXPECT_THAT(read_computed, ElementsAre(90, 91));
}

TEST(SinkTest, ReportsEofWithoutOpen) {
  TestSource source({1, 2, 3});
  std::unique_ptr<From> from = source.Create();
  PullPipeline pipeline(*from, {});
  RowCursor cursor(pipeline);

  EXPECT_TRUE(cursor.eof());
}

TEST(SinkTest, ReopeningRereadsFromTheStart) {
  TestSource source({4, 5, 6});
  std::unique_ptr<From> from = source.Create();
  PullPipeline pipeline(*from, {});

  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
  EXPECT_THAT(Drain(pipeline), ElementsAre(0, 1, 2));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
