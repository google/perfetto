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

#include "src/trace_processor/core/exec/filter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// A value as SQLite would pass it.
using Value = std::variant<std::nullptr_t, int64_t, double, const char*>;

// Hands values to the casters, as the SQLite layer hands them sqlite3_values.
struct Values : ValueFetcher {
  using Type = size_t;
  static constexpr Type kNull = base::variant_index<Value, std::nullptr_t>();
  static constexpr Type kInt64 = base::variant_index<Value, int64_t>();
  static constexpr Type kDouble = base::variant_index<Value, double>();
  static constexpr Type kString = base::variant_index<Value, const char*>();

  int64_t GetInt64Value(uint32_t i) const { return std::get<int64_t>(v[i]); }
  double GetDoubleValue(uint32_t i) const { return std::get<double>(v[i]); }
  const char* GetStringValue(uint32_t i) const {
    return std::get<const char*>(v[i]);
  }
  Type GetValueType(uint32_t i) const { return v[i].index(); }

  std::vector<Value> v;
};

// Hands one IN list to the casters, as SQLite iterates the values of an IN.
struct List : ValueFetcher {
  using Type = size_t;
  static constexpr Type kNull = Values::kNull;
  static constexpr Type kInt64 = Values::kInt64;
  static constexpr Type kDouble = Values::kDouble;
  static constexpr Type kString = Values::kString;

  int64_t GetInt64Value(uint32_t) const { return std::get<int64_t>(v[at]); }
  double GetDoubleValue(uint32_t) const { return std::get<double>(v[at]); }
  const char* GetStringValue(uint32_t) const {
    return std::get<const char*>(v[at]);
  }
  Type GetValueType(uint32_t) const { return v[at].index(); }
  bool IteratorInit(uint32_t) {
    at = 0;
    return !v.empty();
  }
  bool IteratorNext(uint32_t) { return ++at < v.size(); }

  std::vector<Value> v;
  size_t at = 0;
};

// The values of column 1 in the rows of `batch` whose column 0 is in `list`.
std::vector<int64_t> KeptIn(const RowBatch& batch,
                            StorageType type,
                            std::vector<Value> list,
                            const StringPool& pool) {
  Filter filter({{0, In{}, 0}}, &pool);
  Context context;
  std::unique_ptr<OperatorState> state = filter.MakeState(context);
  List fetcher;
  fetcher.v = std::move(list);
  FilterValue cast[1];
  GetFilterValueListCaster<List>(type)(fetcher, 0, pool, &cast[0]);
  context.SetFilterValues(Span<const FilterValue>(cast));
  RowBatch out;
  out.CopyFrom(batch);
  EXPECT_TRUE(filter.Process(out, *state));
  return test::ReadColumn<int64_t>(out, 1);
}

// Most of Filter's meaning is the interpreter's casts and kernels, checked end
// to end by diff tests. These cover what those can't easily reach: batches
// whose rows are already narrowed or windowed, and a plan run again with new
// parameters.
//
// A batch of five rows: `n` holds 10, 20, null, 40, 50 and `s` holds "b",
// "a", "c", "a", "d".
class FilterTest : public ::testing::Test {
 protected:
  FilterTest() {
    for (uint32_t i = 0; i < 5; ++i) {
      if (i != 2) {
        validity_.set(i);
      }
    }
    for (const char* s : {"b", "a", "c", "a", "d"}) {
      strings_.push_back(pool_.InternString(s));
    }
    batch_.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Int64{}}, numbers_, &validity_));
    batch_.AddBorrowedColumn(
        ColumnView::Reference(StorageType{String{}}, strings_.data(), nullptr));
    batch_.SetRowCount(5);
  }

  // A condition on `n` or `s`, compared with value `index`.
  struct On {
    uint32_t column;
    Op op;
    uint32_t index = 0;
  };

  // Casts `values` for `on` into `cast`, as the SQLite layer would with the
  // casters it picked for each condition.
  void CastAll(const std::vector<On>& on,
               const std::vector<Value>& values,
               std::vector<FilterValue>* cast) {
    Values fetcher;
    fetcher.v = values;
    cast->resize(values.size());
    for (const On& c : on) {
      if (c.op.IsAnyOf<NullOp>()) {
        continue;
      }
      StorageType type =
          c.column == 0 ? StorageType{Int64{}} : StorageType{String{}};
      GetFilterValueCaster<Values>(type, c.op)(fetcher, c.index, pool_,
                                               &(*cast)[c.index]);
    }
  }

  static std::vector<Filter::Condition> Conditions(const std::vector<On>& on) {
    std::vector<Filter::Condition> conditions;
    for (const On& c : on) {
      conditions.push_back({c.column, c.op, c.index});
    }
    return conditions;
  }

  // The values of `n` in the rows of `in` every condition keeps.
  std::vector<int64_t> Kept(const std::vector<On>& on,
                            const std::vector<Value>& values,
                            const RowBatch* in = nullptr) {
    Filter filter(Conditions(on), &pool_);
    Context context;
    std::unique_ptr<OperatorState> state = filter.MakeState(context);
    std::vector<FilterValue> cast;
    CastAll(on, values, &cast);
    context.SetFilterValues(Span<const FilterValue>(cast));
    RowBatch batch;
    batch.CopyFrom(in ? *in : batch_);
    EXPECT_TRUE(filter.Process(batch, *state));
    return test::ReadColumn<int64_t>(batch, 0);
  }

  StringPool pool_;
  int64_t numbers_[5] = {10, 20, 0, 40, 50};
  BitVector validity_ = BitVector::CreateWithSize(5);
  std::vector<StringPool::Id> strings_;
  RowBatch batch_;
};

// As in SQL, no comparison is true of a null: only IS NULL keeps one.
TEST_F(FilterTest, KeepsTheRowsEveryConditionHoldsFor) {
  std::vector<Value> twenty = {int64_t{20}};
  EXPECT_THAT(Kept({{0, Eq{}}}, twenty), ElementsAre(20));
  EXPECT_THAT(Kept({{0, Ne{}}}, twenty), ElementsAre(10, 40, 50));
  EXPECT_THAT(Kept({{0, Le{}}}, twenty), ElementsAre(10, 20));
  EXPECT_THAT(Kept({{0, Gt{}}}, twenty), ElementsAre(40, 50));
  EXPECT_THAT(Kept({{0, IsNotNull{}}}, {}), ElementsAre(10, 20, 40, 50));
  EXPECT_EQ(Kept({{0, IsNull{}}}, {}).size(), 1u);

  std::vector<Value> a = {"a"};
  EXPECT_THAT(Kept({{1, Eq{}}}, a), ElementsAre(20, 40));
  EXPECT_THAT(Kept({{1, Ne{}}}, a), ElementsAre(10, 0, 50));
  EXPECT_THAT(Kept({{1, Gt{}}}, {"b"}), ElementsAre(0, 50));
  EXPECT_THAT(Kept({{1, Glob{}}}, {"[ab]"}), ElementsAre(10, 20, 40));
  EXPECT_THAT(Kept({{1, Glob{}}}, {"a"}), ElementsAre(20, 40));

  // Every condition must hold, each with its own value.
  EXPECT_THAT(Kept({{1, Eq{}, 0}, {0, Gt{}, 1}}, {"a", int64_t{20}}),
              ElementsAre(40));
}

// A batch whose rows are already narrowed keeps only rows among them, and
// rows of a window are read where the window starts.
TEST_F(FilterTest, NarrowsTheRowsKeptOfAWindow) {
  std::vector<Value> ten = {int64_t{10}};

  RowBatch narrowed;
  narrowed.CopyFrom(batch_);
  uint32_t positions[] = {0, 3, 4};
  narrowed.mutable_selection().Keep(Span<const uint32_t>(positions));
  EXPECT_THAT(Kept({{0, Gt{}}}, ten, &narrowed), ElementsAre(40, 50));

  // Rows 1 to 4: 20, null, 40, 50.
  RowBatch window;
  window.CopyFrom(batch_);
  test::Window(&window, 1, 4);
  EXPECT_THAT(Kept({{0, Gt{}}}, ten, &window), ElementsAre(20, 40, 50));
}

// A plan runs again with new values cast into the same ones, so strings of
// any length, and ones the pool does not hold, keep the right rows.
TEST_F(FilterTest, RunsAgainWithNewValues) {
  std::vector<On> on = {{1, Lt{}, 0}, {1, Ne{}, 1}};
  Filter filter(Conditions(on), &pool_);
  Context context;
  std::unique_ptr<OperatorState> state = filter.MakeState(context);
  std::vector<FilterValue> cast;
  auto run = [&](const char* lt, const char* ne) {
    CastAll(on, {lt, ne}, &cast);
    context.SetFilterValues(Span<const FilterValue>(cast));
    RowBatch batch;
    batch.CopyFrom(batch_);
    EXPECT_TRUE(filter.Process(batch, *state));
    return test::ReadColumn<int64_t>(batch, 0);
  };
  EXPECT_THAT(run("c", "a"), ElementsAre(10));
  EXPECT_THAT(run("a long string sorting after a", "z"), ElementsAre(20, 40));
  EXPECT_THAT(run("d", "b"), ElementsAre(20, 0, 40));
}

// An Id column holds the row each value is stored at, from where the window
// starts.
TEST(FilterIdTest, ComparesIdsByWhereTheyAre) {
  StringPool pool;
  Filter filter({{0, Ge{}, 0}}, &pool);
  Context context;
  std::unique_ptr<OperatorState> state = filter.MakeState(context);
  Values fetcher;
  fetcher.v = {int64_t{12}};
  FilterValue cast[1];
  GetFilterValueCaster<Values>(StorageType{Id{}}, Ge{})(fetcher, 0, pool,
                                                        &cast[0]);
  context.SetFilterValues(Span<const FilterValue>(cast));
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr, 10));
  batch.SetRowCount(4);
  ASSERT_TRUE(filter.Process(batch, *state));
  EXPECT_THAT(test::ReadColumn<uint32_t>(batch, 0), ElementsAre(12u, 13u));
}

// IN keeps the rows whose value is listed, and never a null. Ids are read from
// where the window starts, whether the list is dense or not.
TEST(FilterInTest, KeepsTheRowsWhoseValueIsListed) {
  StringPool pool;
  int64_t numbers[] = {10, 20, 0, 40, 50};
  auto validity = BitVector::CreateWithSize(5);
  std::vector<StringPool::Id> strings;
  for (uint32_t i = 0; i < 5; ++i) {
    if (i != 2) {
      validity.set(i);
    }
  }
  for (const char* s : {"b", "a", "c", "a", "d"}) {
    strings.push_back(pool.InternString(s));
  }
  auto batch = [&](ColumnView first) {
    RowBatch b;
    b.AddBorrowedColumn(first);
    b.AddBorrowedColumn(
        ColumnView::Reference(StorageType{Int64{}}, numbers, &validity));
    b.SetRowCount(5);
    return b;
  };

  RowBatch by_number =
      batch(ColumnView::Reference(StorageType{Int64{}}, numbers, &validity));
  EXPECT_THAT(KeptIn(by_number, StorageType{Int64{}},
                     {int64_t{40}, nullptr, int64_t{0}, 20.0}, pool),
              ElementsAre(20, 40));

  RowBatch by_string = batch(
      ColumnView::Reference(StorageType{String{}}, strings.data(), nullptr));
  EXPECT_THAT(KeptIn(by_string, StorageType{String{}}, {"a", "zz"}, pool),
              ElementsAre(20, 40));

  RowBatch dense =
      batch(ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr, 2));
  EXPECT_THAT(KeptIn(dense, StorageType{Id{}}, {int64_t{3}, int64_t{5}}, pool),
              ElementsAre(20, 40));
  RowBatch sparse =
      batch(ColumnView::Reference(StorageType{Id{}}, nullptr, nullptr, 1000));
  EXPECT_THAT(KeptIn(sparse, StorageType{Id{}},
                     {int64_t{1001}, int64_t{1003}, int64_t{7}}, pool),
              ElementsAre(20, 40));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
