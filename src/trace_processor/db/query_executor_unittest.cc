/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/db/query_executor.h"
#include "src/trace_processor/db/overlays/arrangement_overlay.h"
#include "src/trace_processor/db/overlays/null_overlay.h"
#include "src/trace_processor/db/overlays/selector_overlay.h"
#include "src/trace_processor/db/storage/numeric_storage.h"
#include "src/trace_processor/db/storage/string_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using OverlaysVec = base::SmallVector<const overlays::StorageOverlay*,
                                      QueryExecutor::kMaxOverlayCount>;
using NumericStorage = storage::NumericStorage;
using StringStorage = storage::StringStorage;
using SimpleColumn = QueryExecutor::SimpleColumn;
using ArrangementOverlay = overlays::ArrangementOverlay;
using NullOverlay = overlays::NullOverlay;
using SelectorOverlay = overlays::SelectorOverlay;

TEST(QueryExecutor, OnlyStorageRange) {
  std::vector<int64_t> storage_data{1, 2, 3, 4, 5};
  NumericStorage storage(storage_data.data(), 5, ColumnType::kInt64);
  SimpleColumn col{OverlaysVec(), &storage};

  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  RowMap rm(0, 5);
  QueryExecutor::BoundedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(rm.size(), 3u);
  ASSERT_EQ(rm.Get(0), 2u);
}

TEST(QueryExecutor, OnlyStorageRangeIsNull) {
  std::vector<int64_t> storage_data{1, 2, 3, 4, 5};
  NumericStorage storage(storage_data.data(), 5, ColumnType::kInt64);
  SimpleColumn col{OverlaysVec(), &storage};

  Constraint c{0, FilterOp::kIsNull, SqlValue::Long(3)};
  RowMap rm(0, 5);
  QueryExecutor::BoundedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(rm.size(), 0u);
}

TEST(QueryExecutor, OnlyStorageIndex) {
  // Setup storage
  std::vector<int64_t> storage_data(10);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  std::transform(storage_data.begin(), storage_data.end(), storage_data.begin(),
                 [](int64_t n) { return n % 5; });
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64);

  SimpleColumn col{OverlaysVec(), &storage};
  Constraint c{0, FilterOp::kLt, SqlValue::Long(2)};
  RowMap rm(0, 10);
  RowMap res = QueryExecutor::IndexedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(res.size(), 4u);
  ASSERT_EQ(res.Get(0), 0u);
  ASSERT_EQ(res.Get(1), 1u);
  ASSERT_EQ(res.Get(2), 5u);
  ASSERT_EQ(res.Get(3), 6u);
}

TEST(QueryExecutor, OnlyStorageIndexIsNull) {
  std::vector<int64_t> storage_data{1, 2, 3, 4, 5};
  NumericStorage storage(storage_data.data(), 5, ColumnType::kInt64);
  SimpleColumn col{OverlaysVec(), &storage};

  Constraint c{0, FilterOp::kIsNull, SqlValue::Long(3)};
  RowMap rm(0, 5);
  RowMap res = QueryExecutor::IndexedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(res.size(), 0u);
}

TEST(QueryExecutor, NullOverlayBounds) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64);
  BitVector bv{1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
  overlays::NullOverlay overlay(&bv);
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&overlay);

  SimpleColumn col{overlays_vec, &storage};

  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  RowMap rm(0, 10);
  QueryExecutor::BoundedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(rm.size(), 2u);
  ASSERT_EQ(rm.Get(0), 4u);
  ASSERT_EQ(rm.Get(1), 8u);
}

TEST(QueryExecutor, NullOverlayRangeIsNull) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64);
  BitVector bv{1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
  overlays::NullOverlay overlay(&bv);
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&overlay);

  SimpleColumn col{overlays_vec, &storage};

  Constraint c{0, FilterOp::kIsNull, SqlValue::Long(3)};
  RowMap rm(0, 10);
  QueryExecutor::BoundedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(0), 2u);
  ASSERT_EQ(rm.Get(1), 5u);
  ASSERT_EQ(rm.Get(2), 6u);
  ASSERT_EQ(rm.Get(3), 7u);
  ASSERT_EQ(rm.Get(4), 9u);
}

TEST(QueryExecutor, NullOverlayIndex) {
  std::vector<int64_t> storage_data(6);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  std::transform(storage_data.begin(), storage_data.end(), storage_data.begin(),
                 [](int64_t n) { return n % 3; });
  NumericStorage storage(storage_data.data(), 6, ColumnType::kInt64);

  BitVector bv{1, 1, 0, 1, 1, 0, 1, 0, 0, 1};
  NullOverlay overlay(&bv);
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&overlay);

  SimpleColumn col{overlays_vec, &storage};

  Constraint c{0, FilterOp::kGe, SqlValue::Long(1)};
  RowMap rm(0, 10);
  RowMap res = QueryExecutor::IndexedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(res.size(), 4u);
  ASSERT_EQ(res.Get(0), 1u);
  ASSERT_EQ(res.Get(1), 3u);
  ASSERT_EQ(res.Get(2), 6u);
  ASSERT_EQ(res.Get(3), 9u);
}

TEST(QueryExecutor, NullOverlayIndexIsNull) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64);
  BitVector bv{1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
  overlays::NullOverlay overlay(&bv);
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&overlay);

  SimpleColumn col{overlays_vec, &storage};

  Constraint c{0, FilterOp::kIsNull, SqlValue::Long(3)};
  RowMap rm(0, 10);
  RowMap res = QueryExecutor::IndexedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(res.size(), 5u);
  ASSERT_EQ(res.Get(0), 2u);
  ASSERT_EQ(res.Get(1), 5u);
  ASSERT_EQ(res.Get(2), 6u);
  ASSERT_EQ(res.Get(3), 7u);
  ASSERT_EQ(res.Get(4), 9u);
}

TEST(QueryExecutor, SelectorOverlayBounds) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  NumericStorage storage(storage_data.data(), 5, ColumnType::kInt64);

  BitVector bv{1, 1, 0, 0, 1};
  SelectorOverlay overlay(&bv);
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&overlay);

  SimpleColumn col{overlays_vec, &storage};

  Constraint c{0, FilterOp::kGt, SqlValue::Long(1)};
  RowMap rm(0, 3);
  QueryExecutor::BoundedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(rm.size(), 1u);
  ASSERT_EQ(rm.Get(0), 2u);
}

TEST(QueryExecutor, SelectorOverlayIndex) {
  std::vector<int64_t> storage_data(10);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  std::transform(storage_data.begin(), storage_data.end(), storage_data.begin(),
                 [](int64_t n) { return n % 5; });
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64);

  BitVector bv{1, 1, 0, 1, 1, 0, 1, 0, 0, 1};
  SelectorOverlay overlay(&bv);
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&overlay);

  SimpleColumn col{overlays_vec, &storage};

  Constraint c{0, FilterOp::kGe, SqlValue::Long(2)};
  RowMap rm(0, 6);
  RowMap res = QueryExecutor::IndexedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(res.size(), 3u);
  ASSERT_EQ(res.Get(0), 2u);
  ASSERT_EQ(res.Get(1), 3u);
  ASSERT_EQ(res.Get(2), 5u);
}

TEST(QueryExecutor, ArrangementOverlayBounds) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  NumericStorage storage(storage_data.data(), 5, ColumnType::kInt64);

  std::vector<uint32_t> arrangement{4, 1, 2, 2, 3};
  overlays::ArrangementOverlay overlay(&arrangement);
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&overlay);

  SimpleColumn col{overlays_vec, &storage};

  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  RowMap rm(0, 5);
  QueryExecutor::BoundedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(rm.size(), 2u);
  ASSERT_EQ(rm.Get(0), 0u);
  ASSERT_EQ(rm.Get(1), 4u);
}

TEST(QueryExecutor, ArrangmentOverlayIndex) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  NumericStorage storage(storage_data.data(), 5, ColumnType::kInt64);

  std::vector<uint32_t> arrangement{4, 1, 2, 2, 3};
  overlays::ArrangementOverlay overlay(&arrangement);
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&overlay);

  SimpleColumn col{overlays_vec, &storage};

  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  RowMap rm(0, 5);
  RowMap res = QueryExecutor::IndexedColumnFilterForTesting(c, col, &rm);

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 0u);
  ASSERT_EQ(res.Get(1), 4u);
}

TEST(QueryExecutor, SingleConstraintWithNullAndSelector) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 0, 1, 2, 3};
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64);

  // Current vector
  // 0, 1, NULL, 2, 3, 0, NULL, NULL, 1, 2, 3, NULL
  BitVector null_bv{1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
  NullOverlay null_overlay(&null_bv);

  // Final vector
  // 0, NULL, 3, NULL, 1, 3
  BitVector selector_bv{1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
  SelectorOverlay selector_overlay(&selector_bv);

  // Create the column.
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&selector_overlay);
  overlays_vec.emplace_back(&null_overlay);
  SimpleColumn col{overlays_vec, &storage};

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue::Long(2)};
  QueryExecutor exec({col}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 2u);
  ASSERT_EQ(res.Get(1), 5u);
}

TEST(QueryExecutor, SingleConstraintWithNullAndArrangement) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 0, 1, 2, 3};
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64);

  // Current vector
  // 0, 1, NULL, 2, 3, 0, NULL, NULL, 1, 2, 3, NULL
  BitVector null_bv{1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
  NullOverlay null_overlay(&null_bv);

  // Final vector
  // NULL, 3, NULL, NULL, 3, NULL
  std::vector<uint32_t> arrangement{2, 4, 6, 2, 4, 6};
  ArrangementOverlay arrangement_overlay(&arrangement);

  // Create the column.
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&arrangement_overlay);
  overlays_vec.emplace_back(&null_overlay);
  SimpleColumn col{overlays_vec, &storage};

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue::Long(1)};
  QueryExecutor exec({col}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 1u);
  ASSERT_EQ(res.Get(1), 4u);
}

TEST(QueryExecutor, IsNullWithSelector) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 0, 1, 2, 3};
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64);

  // Current vector
  // 0, 1, NULL, 2, 3, 0, NULL, NULL, 1, 2, 3, NULL
  BitVector null_bv{1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
  NullOverlay null_overlay(&null_bv);

  // Final vector
  // 0, NULL, 3, NULL, 1, 3
  BitVector selector_bv{1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
  SelectorOverlay selector_overlay(&selector_bv);

  // Create the column.
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&selector_overlay);
  overlays_vec.emplace_back(&null_overlay);
  SimpleColumn col{overlays_vec, &storage};

  // Filter.
  Constraint c{0, FilterOp::kIsNull, SqlValue::Long(0)};
  QueryExecutor exec({col}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 1u);
  ASSERT_EQ(res.Get(1), 3u);
}

TEST(QueryExecutor, BinarySearch) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 4, 5, 6};
  NumericStorage storage(storage_data.data(), 7, ColumnType::kInt64, true);

  // Add nulls - {0, 1, NULL, NULL, 2, 3, NULL, NULL, 4, 5, 6, NULL}
  BitVector null_bv{1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0};
  NullOverlay null_overlay(&null_bv);

  // Final vector {1, NULL, 3, NULL, 5, NULL}.
  BitVector selector_bv{0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
  SelectorOverlay selector_overlay(&selector_bv);

  // Create the column.
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&selector_overlay);
  overlays_vec.emplace_back(&null_overlay);
  SimpleColumn col{overlays_vec, &storage};

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  QueryExecutor exec({col}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 2u);
  ASSERT_EQ(res.Get(1), 4u);
}

TEST(QueryExecutor, BinarySearchIsNull) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  NumericStorage storage(storage_data.data(), 10, ColumnType::kInt64, true);

  // Select 6 elements from storage, resulting in a vector {0, 1, 3, 4, 6, 7}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0, 1, 1, 0, 0};
  SelectorOverlay selector_overlay(&selector_bv);

  // Add nulls, final vector {NULL, NULL, NULL 0, 1, 3, 4, 6, 7}.
  BitVector null_bv{0, 0, 0, 1, 1, 1, 1, 1, 1};
  NullOverlay null_overlay(&null_bv);

  // Create the column.
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&null_overlay);
  overlays_vec.emplace_back(&selector_overlay);
  SimpleColumn col{overlays_vec, &storage};

  // Filter.
  Constraint c{0, FilterOp::kIsNull, SqlValue::Long(0)};
  QueryExecutor exec({col}, 9);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 3u);
  ASSERT_EQ(res.Get(0), 0u);
  ASSERT_EQ(res.Get(1), 1u);
  ASSERT_EQ(res.Get(2), 2u);
}

TEST(QueryExecutor, StringBinarySearchIsNull) {
  StringPool pool;
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());
  StringStorage storage(&pool, ids.data(), 7);

  // Final vec {"cheese", "pasta", "NULL", "pierogi", "fries"}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0, 1};
  SelectorOverlay selector_overlay(&selector_bv);

  // Create the column.
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&selector_overlay);
  SimpleColumn col{overlays_vec, &storage};

  // Filter.
  Constraint c{0, FilterOp::kIsNull, SqlValue::Long(0)};
  QueryExecutor exec({col}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 1u);
  ASSERT_EQ(res.Get(0), 2u);
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
TEST(QueryExecutor, StringBinarySearchRegex) {
  StringPool pool;
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());
  StringStorage storage(&pool, ids.data(), 7);

  // Final vec {"cheese", "pasta", "NULL", "pierogi", "fries"}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0, 1};
  SelectorOverlay selector_overlay(&selector_bv);

  // Create the column.
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&selector_overlay);
  SimpleColumn col{overlays_vec, &storage};

  // Filter.
  Constraint c{0, FilterOp::kRegex, SqlValue::String("p.*")};
  QueryExecutor exec({col}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 1u);
  ASSERT_EQ(res.Get(1), 3u);
}

TEST(QueryExecutor, StringBinarySearchRegexWithNum) {
  StringPool pool;
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());
  StringStorage storage(&pool, ids.data(), 7);

  // Final vec {"cheese", "pasta", "NULL", "pierogi", "fries"}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0, 1};
  SelectorOverlay selector_overlay(&selector_bv);

  // Create the column.
  OverlaysVec overlays_vec;
  overlays_vec.emplace_back(&selector_overlay);
  SimpleColumn col{overlays_vec, &storage};

  // Filter.
  Constraint c{0, FilterOp::kRegex, SqlValue::Long(4)};
  QueryExecutor exec({col}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 0u);
}
#endif

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
