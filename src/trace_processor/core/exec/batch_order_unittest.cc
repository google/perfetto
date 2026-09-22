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

#include "src/trace_processor/core/exec/batch_order.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

using Cell = std::optional<int64_t>;
using Row = std::vector<Cell>;

// One batch of the given rows, column by column, with an id column first so
// the order rows come out in can be read back.
class Batch {
 public:
  explicit Batch(const std::vector<Row>& rows) {
    auto count = static_cast<uint32_t>(rows.size());
    size_t width = rows.empty() ? 0 : rows[0].size();
    values_.resize(width, std::vector<int64_t>(count));
    validity_.resize(width);
    for (size_t c = 0; c < width; ++c) {
      validity_[c] = BitVector::CreateWithSize(count);
      for (uint32_t r = 0; r < count; ++r) {
        if (rows[r][c]) {
          values_[c][r] = *rows[r][c];
          validity_[c].set(r);
        }
      }
    }
    batch_.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
    for (size_t c = 0; c < width; ++c) {
      bool nullable = validity_[c].CountSetBits() != count;
      batch_.AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, values_[c].data(),
                                nullable ? &validity_[c] : nullptr));
    }
    batch_.Compose(RowSelection::Range(0), count);
    batch_.SetCardinality(count);
  }
  const RowBatch& batch() const { return batch_; }

 private:
  std::vector<std::vector<int64_t>> values_;
  std::vector<BitVector> validity_;
  RowBatch batch_;
};

// The ids of `rows` in the order BatchOrder on `columns` leaves them.
std::vector<uint32_t> Order(const std::vector<Row>& rows,
                            std::vector<uint32_t> columns,
                            uint32_t ordered_by_last = 0) {
  for (uint32_t& c : columns) {
    ++c;  // Past the id column.
  }
  Batch in(rows);
  BatchOrder order(std::move(columns), ordered_by_last);
  std::unique_ptr<OperatorState> state = order.MakeState();
  RowBatch out;
  EXPECT_EQ(order.Execute(in.batch(), out, *state), OpResult::kNeedMoreInput);
  EXPECT_EQ(out.column_count(), in.batch().column_count());
  return test::ReadColumn<uint32_t>(out, 0);
}

TEST(BatchOrder, OrdersByOneColumn) {
  EXPECT_THAT(Order({{3}, {1}, {2}}, {0}), ElementsAre(1, 2, 0));
}

TEST(BatchOrder, TiesKeepTheirInputOrder) {
  EXPECT_THAT(Order({{2}, {1}, {2}, {1}}, {0}), ElementsAre(1, 3, 0, 2));
}

TEST(BatchOrder, LaterColumnsBreakTies) {
  EXPECT_THAT(Order({{1, 9}, {0, 5}, {1, 2}, {0, 7}}, {0, 1}),
              ElementsAre(1, 3, 2, 0));
  // The same rows on the second column alone.
  EXPECT_THAT(Order({{1, 9}, {0, 5}, {1, 2}, {0, 7}}, {1}),
              ElementsAre(2, 1, 3, 0));
}

TEST(BatchOrder, NullsComeFirst) {
  EXPECT_THAT(Order({{2}, {std::nullopt}, {1}, {std::nullopt}}, {0}),
              ElementsAre(1, 3, 2, 0));
  EXPECT_THAT(Order({{1, 1}, {std::nullopt, 0}, {1, std::nullopt}}, {0, 1}),
              ElementsAre(1, 2, 0));
}

TEST(BatchOrder, AnOrderedBatchPassesThrough) {
  std::vector<Row> rows = {{0, 5}, {0, 7}, {1, 2}, {1, 2}};
  Batch in(rows);
  BatchOrder order({1, 2});
  std::unique_ptr<OperatorState> state = order.MakeState();
  RowBatch out;
  ASSERT_EQ(order.Execute(in.batch(), out, *state), OpResult::kNeedMoreInput);
  // The columns still point at the rows as a range.
  EXPECT_TRUE(out.column(1).selection().is_range());
  EXPECT_THAT(test::ReadColumn<uint32_t>(out, 0), ElementsAre(0, 1, 2, 3));
}

TEST(BatchOrder, AFullBatchOfRandomRows) {
  std::minstd_rand rng(3);
  std::vector<Row> rows;
  for (uint32_t i = 0; i < kMaxBatchRows; ++i) {
    rows.push_back({static_cast<int64_t>(rng() % 50),
                    rng() % 10 == 0 ? Cell() : Cell(rng() % 1000)});
  }
  std::vector<uint32_t> order = Order(rows, {0, 1});
  ASSERT_EQ(order.size(), kMaxBatchRows);
  for (uint32_t i = 1; i < order.size(); ++i) {
    const Row& a = rows[order[i - 1]];
    const Row& b = rows[order[i]];
    bool before = a[0] < b[0] || (a[0] == b[0] && a[1] < b[1]) ||
                  (a == b && order[i - 1] < order[i]);
    EXPECT_TRUE(before) << "at " << i;
  }
}

TEST(BatchOrder, NegativeAndWideValues) {
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  // Values whose bytes differ at every position, including the sign.
  EXPECT_THAT(Order({{0},
                     {kMax},
                     {-1},
                     {kMin},
                     {1},
                     {int64_t{1} << 40},
                     {-(int64_t{1} << 40)}},
                    {0}),
              ElementsAre(3, 6, 2, 0, 4, 5, 1));
  // A column of large values which agree on their upper bytes.
  constexpr int64_t kBase = int64_t{1} << 50;
  EXPECT_THAT(
      Order({{kBase + 300}, {kBase + 2}, {kBase + 70000}, {kBase}}, {0}),
      ElementsAre(3, 1, 0, 2));
}

TEST(BatchOrder, EqualRowsKeepTheirOrder) {
  std::vector<Row> rows(kMaxBatchRows, Row{7, 7});
  std::vector<uint32_t> order = Order(rows, {0, 1});
  for (uint32_t i = 0; i < order.size(); ++i) {
    ASSERT_EQ(order[i], i);
  }
}

// Only the leading column is sorted when the rows already arrive ordered by
// the trailing one, and that order survives among equal leading values.
TEST(BatchOrder, TrailingColumnsAlreadyOrderedAreLeftAlone) {
  std::vector<Row> rows = {{1, 1}, {0, 2}, {1, 3}, {0, 4}, {std::nullopt, 5}};
  EXPECT_THAT(Order(rows, {0, 1}, 1), ElementsAre(4, 1, 3, 0, 2));
  // Rows which are not in fact ordered by the trailing column keep their
  // input order within a leading value: the trailing column is not consulted.
  EXPECT_THAT(Order({{0, 9}, {0, 1}, {0, 5}}, {0, 1}, 1), ElementsAre(0, 1, 2));
}

// Whether row `a` comes before row `b` in the order BatchOrder defines: by
// the leading `width - ordered_by_last` columns with a null first, then, for
// rows equal on those, by their input order. The trailing columns are the
// caller's promise, not compared.
bool Before(const std::vector<Row>& rows,
            uint32_t a,
            uint32_t b,
            size_t width,
            uint32_t ordered_by_last) {
  for (size_t c = 0; c + ordered_by_last < width; ++c) {
    if (rows[a][c] != rows[b][c]) {
      return rows[a][c] < rows[b][c];
    }
  }
  return a < b;
}

// Rows of `width` columns whose values sit in a window near 2^(bits - 1),
// so they are negative for wide `bits` and share their upper bytes, with a
// null now and then, and, when `ordered_by_last`, already ordered by the last
// column.
std::vector<Row> RandomRows(std::minstd_rand& rng,
                            uint32_t count,
                            size_t width,
                            uint32_t bits,
                            bool ordered_by_last) {
  std::vector<Row> rows;
  for (uint32_t i = 0; i < count; ++i) {
    Row row;
    for (size_t c = 0; c < width; ++c) {
      int64_t value = static_cast<int64_t>(rng() % (uint64_t{1} << bits)) -
                      (int64_t{1} << (bits - 1));
      if (ordered_by_last && c + 1 == width) {
        value = i;
      }
      row.push_back(!ordered_by_last && rng() % 20 == 0 ? Cell() : Cell(value));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

TEST(BatchOrder, MatchesTheDefinitionOnRandomRows) {
  std::minstd_rand rng(11);
  for (size_t width : {1u, 2u, 3u}) {
    for (uint32_t bits : {4u, 20u, 44u, 63u}) {
      std::vector<Row> rows =
          RandomRows(rng, kMaxBatchRows, width, bits, false);
      std::vector<uint32_t> columns(width);
      std::iota(columns.begin(), columns.end(), 0u);
      std::vector<uint32_t> order = Order(rows, columns);
      ASSERT_EQ(order.size(), rows.size());
      for (uint32_t i = 1; i < order.size(); ++i) {
        ASSERT_TRUE(Before(rows, order[i - 1], order[i], width, 0))
            << "width " << width << " bits " << bits << " at " << i;
      }
    }
  }
}

TEST(BatchOrder, GroupsStablyWhenTheTrailingColumnIsOrdered) {
  std::minstd_rand rng(12);
  for (size_t width : {2u, 3u}) {
    // Few distinct leading values, so the groups are long and the trailing
    // order within them is what the test sees.
    std::vector<Row> rows = RandomRows(rng, kMaxBatchRows, width, 5, true);
    std::vector<uint32_t> columns(width);
    std::iota(columns.begin(), columns.end(), 0u);
    std::vector<uint32_t> order = Order(rows, columns, 1);
    ASSERT_EQ(order.size(), rows.size());
    for (uint32_t i = 1; i < order.size(); ++i) {
      ASSERT_TRUE(Before(rows, order[i - 1], order[i], width, 1))
          << "width " << width << " at " << i;
    }
  }
}

TEST(BatchOrder, TheWrongTypeIsAnError) {
  Batch in({{1}, {2}});
  BatchOrder order({0});  // The id column is not Int64.
  std::unique_ptr<OperatorState> state = order.MakeState();
  RowBatch out;
  EXPECT_EQ(order.Execute(in.batch(), out, *state), OpResult::kError);
  EXPECT_FALSE(order.status(*state).ok());
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
