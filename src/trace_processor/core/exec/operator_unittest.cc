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
#include "src/trace_processor/core/exec/from.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/exec/transient_column.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

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
std::vector<uint32_t> Drain(PullPipeline& pipeline) {
  std::vector<uint32_t> rows;
  pipeline.Reset();
  Sink sink(pipeline);
  for (sink.Open(); !sink.eof(); sink.Next()) {
    rows.push_back(sink.row());
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
  Sink sink(pipeline);

  EXPECT_TRUE(sink.eof());
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
