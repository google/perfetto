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

#include "src/trace_processor/core/exec/sort.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

class KeysSource final : public Source {
 public:
  KeysSource(std::vector<std::optional<int64_t>> first,
             std::vector<double> second,
             uint32_t chunk)
      : second_(std::move(second)), chunk_(chunk) {
    validity_ = BitVector::CreateWithSize(first.size());
    for (uint32_t i = 0; i < first.size(); ++i) {
      first_.push_back(first[i].value_or(0));
      if (first[i]) {
        validity_.set(i);
      }
    }
  }

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(first_.size());
    if (s.emitted == rows) {
      return false;
    }
    uint32_t count = std::min(chunk_, rows - s.emitted);
    out.Reset();
    out.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, first_.data(), &validity_));
    out.AddColumn(ColumnView::Reference(StorageType{Double{}}, second_.data()));
    test::Window(&out, s.emitted, count);
    s.emitted += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t emitted = 0;
  };

  std::vector<int64_t> first_;
  BitVector validity_;
  std::vector<double> second_;
  uint32_t chunk_;
};

std::vector<uint32_t> Ids(const Source& source, base::Status* status) {
  std::unique_ptr<OperatorState> state = source.MakeState();
  RowBatch batch;
  std::vector<uint32_t> ids;
  while (source.GetData(batch, *state)) {
    for (uint32_t id : test::ReadColumn<uint32_t>(batch, 0)) {
      ids.push_back(id);
    }
  }
  *status = source.status(*state);
  return ids;
}

std::vector<Pipeline::Step> SortBy(std::vector<SortSpec::Key> keys) {
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<Sort>(SortSpec{std::move(keys)}));
  return ops;
}

TEST(SortTest, OrdersManyBatchesStably) {
  // Few distinct values across several batches, so most rows tie.
  std::vector<int64_t> values(3 * kMaxBatchRows + 17);
  std::mt19937 rng(7);
  for (int64_t& value : values) {
    value = static_cast<int64_t>(rng() % 50) - 25;
  }
  test::ArraySource source(values);
  Pipeline sorted(source, SortBy({{1, false}}), {});

  base::Status status;
  std::vector<uint32_t> ids = Ids(sorted, &status);
  ASSERT_TRUE(status.ok()) << status.message();
  std::vector<uint32_t> expected(values.size());
  std::iota(expected.begin(), expected.end(), 0u);
  std::stable_sort(
      expected.begin(), expected.end(),
      [&](uint32_t a, uint32_t b) { return values[a] < values[b]; });
  EXPECT_EQ(ids, expected);
}

TEST(SortTest, NullsSortAsInSqlite) {
  KeysSource source({3, std::nullopt, 1, std::nullopt}, {0, 0, 0, 0}, 3);
  base::Status status;
  {
    Pipeline sorted(source, SortBy({{1, false}}), {});
    EXPECT_THAT(Ids(sorted, &status), ElementsAre(1, 3, 2, 0));
  }
  {
    Pipeline sorted(source, SortBy({{1, true}}), {});
    EXPECT_THAT(Ids(sorted, &status), ElementsAre(0, 2, 1, 3));
  }
}

TEST(SortTest, RewindAfterInvalidKeysAndCompletedSort) {
  Sort sort(SortSpec{{{1, false}, {2, true}}});
  auto state = sort.MakeState();
  std::vector<int64_t> first = {2, 1, 2, 1};
  std::vector<int32_t> narrower = {2, 1, 2, 1};
  std::vector<double> second = {0.5, -1.5, -0.5, 2.5};
  RowBatch in;
  RowBatch out;
  in.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
  in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, first.data()));
  // The first key is valid, but the second cannot be used in a row layout.
  in.AddColumn(ColumnView::Variants(nullptr));
  in.SetRowCount(4);
  ASSERT_EQ(sort.Execute(in, out, *state), OpResult::kError);
  EXPECT_THAT(sort.status(*state).message(), testing::HasSubstr("key 2"));

  for (bool change_type : {false, true}) {
    // Recover from the rejected batch, then reuse a completed sort with a
    // different key width. Both runs must establish their own row layout.
    state->Reset();
    ASSERT_TRUE(sort.status(*state).ok());
    if (change_type) {
      in.SetColumn(
          1, ColumnView::Reference(StorageType{Int32{}}, narrower.data()));
    }
    in.SetColumn(2,
                 ColumnView::Reference(StorageType{Double{}}, second.data()));
    ASSERT_EQ(sort.Execute(in, out, *state), OpResult::kNeedMoreInput);
    ASSERT_EQ(sort.Finish(out, *state), OpResult::kHaveMoreOutput);
    // Equal first keys are ordered by the descending second key.
    EXPECT_THAT(test::ReadColumn<uint32_t>(out, 0), ElementsAre(3, 1, 0, 2));
    EXPECT_EQ(sort.Finish(out, *state), OpResult::kNeedMoreInput);
    EXPECT_TRUE(sort.status(*state).ok());
  }
}

TEST(SortTest, IdsSortAsTheirValues) {
  test::ArraySource source({5, 4, 3});
  Pipeline sorted(source, SortBy({{0, true}}), {});
  base::Status status;
  EXPECT_THAT(Ids(sorted, &status), ElementsAre(2, 1, 0));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
