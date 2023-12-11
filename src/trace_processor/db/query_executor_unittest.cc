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

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/storage/arrangement_storage.h"
#include "src/trace_processor/db/storage/fake_storage.h"
#include "src/trace_processor/db/storage/id_storage.h"
#include "src/trace_processor/db/storage/null_storage.h"
#include "src/trace_processor/db/storage/numeric_storage.h"
#include "src/trace_processor/db/storage/selector_storage.h"
#include "src/trace_processor/db/storage/set_id_storage.h"
#include "src/trace_processor/db/storage/storage.h"
#include "src/trace_processor/db/storage/string_storage.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using testing::ElementsAre;

using IdStorage = storage::IdStorage;
using SetIdStorage = storage::SetIdStorage;
using StringStorage = storage::StringStorage;
using NullStorage = storage::NullStorage;
using ArrangementStorage = storage::ArrangementStorage;
using SelectorStorage = storage::SelectorStorage;

TEST(QueryExecutor, OnlyStorageRange) {
  std::vector<int64_t> storage_data{1, 2, 3, 4, 5};
  storage::NumericStorage<int64_t> storage(&storage_data, ColumnType::kInt64);

  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  RowMap rm(0, storage.size());
  QueryExecutor::BoundedColumnFilterForTesting(c, storage, &rm);

  ASSERT_EQ(rm.size(), 3u);
  ASSERT_EQ(rm.Get(0), 2u);
}

TEST(QueryExecutor, OnlyStorageRangeIsNull) {
  std::vector<int64_t> storage_data{1, 2, 3, 4, 5};
  storage::NumericStorage<int64_t> storage(&storage_data, ColumnType::kInt64);

  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  RowMap rm(0, 5);
  QueryExecutor::BoundedColumnFilterForTesting(c, storage, &rm);

  ASSERT_EQ(rm.size(), 0u);
}

TEST(QueryExecutor, OnlyStorageIndex) {
  // Setup storage
  std::vector<int64_t> storage_data(10);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  std::transform(storage_data.begin(), storage_data.end(), storage_data.begin(),
                 [](int64_t n) { return n % 5; });
  storage::NumericStorage<int64_t> storage(&storage_data, ColumnType::kInt64);

  Constraint c{0, FilterOp::kLt, SqlValue::Long(2)};
  RowMap rm(0, 10);
  QueryExecutor::IndexedColumnFilterForTesting(c, storage, &rm);

  ASSERT_EQ(rm.size(), 4u);
  ASSERT_EQ(rm.Get(0), 0u);
  ASSERT_EQ(rm.Get(1), 1u);
  ASSERT_EQ(rm.Get(2), 5u);
  ASSERT_EQ(rm.Get(3), 6u);
}

TEST(QueryExecutor, OnlyStorageIndexIsNull) {
  std::vector<int64_t> storage_data{1, 2, 3, 4, 5};
  storage::NumericStorage<int64_t> storage(&storage_data, ColumnType::kInt64);

  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  RowMap rm(0, 5);
  QueryExecutor::IndexedColumnFilterForTesting(c, storage, &rm);

  ASSERT_EQ(rm.size(), 0u);
}

TEST(QueryExecutor, NullBounds) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);
  BitVector bv{1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
  storage::NullStorage storage(std::move(numeric), &bv);

  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  RowMap rm(0, 10);
  QueryExecutor::BoundedColumnFilterForTesting(c, storage, &rm);

  ASSERT_EQ(rm.size(), 2u);
  ASSERT_EQ(rm.Get(0), 4u);
  ASSERT_EQ(rm.Get(1), 8u);
}

TEST(QueryExecutor, NullRangeIsNull) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  BitVector bv{1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
  storage::NullStorage storage(std::move(numeric), &bv);

  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  RowMap rm(0, storage.size());
  QueryExecutor::BoundedColumnFilterForTesting(c, storage, &rm);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(0), 2u);
  ASSERT_EQ(rm.Get(1), 5u);
  ASSERT_EQ(rm.Get(2), 6u);
  ASSERT_EQ(rm.Get(3), 7u);
  ASSERT_EQ(rm.Get(4), 9u);
}

TEST(QueryExecutor, NullIndex) {
  std::vector<int64_t> storage_data(6);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  std::transform(storage_data.begin(), storage_data.end(), storage_data.begin(),
                 [](int64_t n) { return n % 3; });
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  BitVector bv{1, 1, 0, 1, 1, 0, 1, 0, 0, 1};
  storage::NullStorage storage(std::move(numeric), &bv);

  Constraint c{0, FilterOp::kGe, SqlValue::Long(1)};
  RowMap rm(0, 10);
  QueryExecutor::IndexedColumnFilterForTesting(c, storage, &rm);

  ASSERT_EQ(rm.size(), 4u);
  ASSERT_EQ(rm.Get(0), 1u);
  ASSERT_EQ(rm.Get(1), 3u);
  ASSERT_EQ(rm.Get(2), 6u);
  ASSERT_EQ(rm.Get(3), 9u);
}

TEST(QueryExecutor, NullIndexIsNull) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  BitVector bv{1, 1, 0, 1, 1, 0, 0, 0, 1, 0};
  storage::NullStorage storage(std::move(numeric), &bv);

  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  RowMap rm(0, 10);
  QueryExecutor::IndexedColumnFilterForTesting(c, storage, &rm);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(0), 2u);
  ASSERT_EQ(rm.Get(1), 5u);
  ASSERT_EQ(rm.Get(2), 6u);
  ASSERT_EQ(rm.Get(3), 7u);
  ASSERT_EQ(rm.Get(4), 9u);
}

TEST(QueryExecutor, SelectorStorageBounds) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  BitVector bv{1, 1, 0, 0, 1};
  SelectorStorage storage(std::move(numeric), &bv);

  Constraint c{0, FilterOp::kGt, SqlValue::Long(1)};
  RowMap rm(0, 3);
  QueryExecutor::BoundedColumnFilterForTesting(c, storage, &rm);

  ASSERT_THAT(rm.GetAllIndices(), ElementsAre(2u));
}

TEST(QueryExecutor, SelectorStorageIndex) {
  std::vector<int64_t> storage_data(10);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  std::transform(storage_data.begin(), storage_data.end(), storage_data.begin(),
                 [](int64_t n) { return n % 5; });
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  BitVector bv{1, 1, 0, 1, 1, 0, 1, 0, 0, 1};
  SelectorStorage storage(std::move(numeric), &bv);

  Constraint c{0, FilterOp::kGe, SqlValue::Long(2)};
  RowMap rm(0, 6);
  QueryExecutor::IndexedColumnFilterForTesting(c, storage, &rm);

  ASSERT_THAT(rm.GetAllIndices(), ElementsAre(2u, 3u, 5u));
}

TEST(QueryExecutor, ArrangementStorageBounds) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  std::vector<uint32_t> arrangement{4, 1, 2, 2, 3};
  storage::ArrangementStorage storage(std::move(numeric), &arrangement);

  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  RowMap rm(0, 5);
  QueryExecutor::BoundedColumnFilterForTesting(c, storage, &rm);

  ASSERT_THAT(rm.GetAllIndices(), ElementsAre(0u, 4u));
}

TEST(QueryExecutor, ArrangementStorageSubsetInputRange) {
  std::unique_ptr<storage::Storage> fake =
      storage::FakeStorage::SearchSubset(5u, RowMap::Range(2u, 4u));

  std::vector<uint32_t> arrangement{4, 1, 2, 2, 3};
  storage::ArrangementStorage storage(std::move(fake), &arrangement);

  Constraint c{0, FilterOp::kGe, SqlValue::Long(0u)};
  RowMap rm(1, 3);
  QueryExecutor::BoundedColumnFilterForTesting(c, storage, &rm);

  ASSERT_THAT(rm.GetAllIndices(), ElementsAre(2u));
}

TEST(QueryExecutor, ArrangementStorageSubsetInputBitvector) {
  std::unique_ptr<storage::Storage> fake =
      storage::FakeStorage::SearchSubset(5u, BitVector({0, 0, 1, 1, 0}));

  std::vector<uint32_t> arrangement{4, 1, 2, 2, 3};
  storage::ArrangementStorage storage(std::move(fake), &arrangement);

  Constraint c{0, FilterOp::kGe, SqlValue::Long(0u)};
  RowMap rm(1, 3);
  QueryExecutor::BoundedColumnFilterForTesting(c, storage, &rm);

  ASSERT_THAT(rm.GetAllIndices(), ElementsAre(2u));
}

TEST(QueryExecutor, ArrangementStorageIndex) {
  std::vector<int64_t> storage_data(5);
  std::iota(storage_data.begin(), storage_data.end(), 0);
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  std::vector<uint32_t> arrangement{4, 1, 2, 2, 3};
  storage::ArrangementStorage storage(std::move(numeric), &arrangement);

  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  RowMap rm(0, 5);
  QueryExecutor::IndexedColumnFilterForTesting(c, storage, &rm);

  ASSERT_THAT(rm.GetAllIndices(), ElementsAre(0u, 4u));
}

TEST(QueryExecutor, MismatchedTypeNullWithOtherOperations) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 0, 1, 2, 3};
  storage::NumericStorage<int64_t> storage(&storage_data, ColumnType::kInt64);

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue()};
  QueryExecutor exec({&storage}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_TRUE(res.empty());
}

TEST(QueryExecutor, SingleConstraintWithNullAndSelector) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 0, 1, 2, 3};
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  // Current vector
  // 0, 1, NULL, 2, 3, 0, NULL, NULL, 1, 2, 3, NULL
  BitVector null_bv{1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
  auto null =
      std::make_unique<storage::NullStorage>(std::move(numeric), &null_bv);

  // Final vector
  // 0, NULL, 3, NULL, 1, 3
  BitVector selector_bv{1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
  SelectorStorage storage(std::move(null), &selector_bv);

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue::Long(2)};
  QueryExecutor exec({&storage}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 2u);
  ASSERT_EQ(res.Get(1), 5u);
}

TEST(QueryExecutor, SingleConstraintWithNullAndArrangement) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 0, 1, 2, 3};
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  // Current vector
  // 0, 1, NULL, 2, 3, 0, NULL, NULL, 1, 2, 3, NULL
  BitVector null_bv{1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
  auto null =
      std::make_unique<storage::NullStorage>(std::move(numeric), &null_bv);

  // Final vector
  // NULL, 3, NULL, NULL, 3, NULL
  std::vector<uint32_t> arrangement{2, 4, 6, 2, 4, 6};
  ArrangementStorage storage(std::move(null), &arrangement);

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue::Long(1)};
  QueryExecutor exec({&storage}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 1u);
  ASSERT_EQ(res.Get(1), 4u);
}

TEST(QueryExecutor, IsNullWithSelector) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 0, 1, 2, 3};
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64);

  // Current vector
  // 0, 1, NULL, 2, 3, 0, NULL, NULL, 1, 2, 3, NULL
  BitVector null_bv{1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
  auto null =
      std::make_unique<storage::NullStorage>(std::move(numeric), &null_bv);

  // Final vector
  // 0, NULL, 3, NULL, 1, 3
  BitVector selector_bv{1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
  SelectorStorage storage(std::move(null), &selector_bv);

  // Filter.
  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  QueryExecutor exec({&storage}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 1u);
  ASSERT_EQ(res.Get(1), 3u);
}

TEST(QueryExecutor, BinarySearch) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 4, 5, 6};
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64, true);

  // Add nulls - {0, 1, NULL, NULL, 2, 3, NULL, NULL, 4, 5, 6, NULL}
  BitVector null_bv{1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0};
  auto null =
      std::make_unique<storage::NullStorage>(std::move(numeric), &null_bv);

  // Final vector {1, NULL, 3, NULL, 5, NULL}.
  BitVector selector_bv{0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
  SelectorStorage storage(std::move(null), &selector_bv);

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue::Long(3)};
  QueryExecutor exec({&storage}, 6);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 2u);
  ASSERT_EQ(res.Get(1), 4u);
}

TEST(QueryExecutor, BinarySearchIsNull) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  auto numeric = std::make_unique<storage::NumericStorage<int64_t>>(
      &storage_data, ColumnType::kInt64, true);

  // Select 6 elements from storage, resulting in a vector {0, 1, 3, 4, 6, 7}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0, 1, 1, 0, 0};
  auto selector =
      std::make_unique<SelectorStorage>(std::move(numeric), &selector_bv);

  // Add nulls, final vector {NULL, NULL, NULL 0, 1, 3, 4, 6, 7}.
  BitVector null_bv{0, 0, 0, 1, 1, 1, 1, 1, 1};
  storage::NullStorage storage(std::move(selector), &null_bv);

  // Filter.
  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  QueryExecutor exec({&storage}, 9);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 3u);
  ASSERT_EQ(res.Get(0), 0u);
  ASSERT_EQ(res.Get(1), 1u);
  ASSERT_EQ(res.Get(2), 2u);
}

TEST(QueryExecutor, SetIdStorage) {
  std::vector<uint32_t> storage_data{0, 0, 0, 3, 3, 3, 6, 6, 6, 9, 9, 9};
  auto numeric = std::make_unique<storage::SetIdStorage>(&storage_data);

  // Select 6 elements from storage, resulting in a vector {0, 3, 3, 6, 9, 9}.
  BitVector selector_bv{0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
  auto selector =
      std::make_unique<SelectorStorage>(std::move(numeric), &selector_bv);

  // Add nulls - vector (size 10) {NULL, 0, 3, NULL, 3, 6, NULL, 9, 9, NULL}.
  BitVector null_bv{0, 1, 1, 0, 1, 1, 0, 1, 1, 0};
  storage::NullStorage storage(std::move(selector), &null_bv);

  // Filter.
  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  QueryExecutor exec({&storage}, 10);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 4u);
  ASSERT_EQ(res.Get(0), 0u);
  ASSERT_EQ(res.Get(1), 3u);
  ASSERT_EQ(res.Get(2), 6u);
  ASSERT_EQ(res.Get(3), 9u);
}

TEST(QueryExecutor, BinarySearchNotEq) {
  std::vector<int64_t> storage_data{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  storage::NumericStorage<int64_t> storage(&storage_data, ColumnType::kInt64,
                                           true);

  // Filter.
  Constraint c{0, FilterOp::kNe, SqlValue::Long(5)};
  QueryExecutor exec({&storage}, 10);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 9u);
}

TEST(QueryExecutor, IdSearchIsNull) {
  IdStorage storage(5);

  // Filter.
  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  QueryExecutor exec({&storage}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 0u);
}

TEST(QueryExecutor, IdSearchIsNotNull) {
  IdStorage storage(5);

  // Filter.
  Constraint c{0, FilterOp::kIsNotNull, SqlValue()};
  QueryExecutor exec({&storage}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 5u);
}

TEST(QueryExecutor, IdSearchNotEq) {
  IdStorage storage(5);

  // Filter.
  Constraint c{0, FilterOp::kNe, SqlValue::Long(3)};
  QueryExecutor exec({&storage}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 4u);
}

TEST(QueryExecutor, StringSearchIsNull) {
  StringPool pool;
  std::vector<std::string> strings{"cheese",  "pasta", "pizza",
                                   "pierogi", "onion", "fries"};
  std::vector<StringPool::Id> ids;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  ids.insert(ids.begin() + 3, StringPool::Id::Null());
  auto string = std::make_unique<StringStorage>(&pool, &ids);

  // Final vec {"cheese", "pasta", "NULL", "pierogi", "fries"}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0, 1};
  SelectorStorage storage(std::move(string), &selector_bv);

  // Filter.
  Constraint c{0, FilterOp::kIsNull, SqlValue()};
  QueryExecutor exec({&storage}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 1u);
  ASSERT_EQ(res.Get(0), 2u);
}

TEST(QueryExecutor, StringSearchGtSorted) {
  StringPool pool;
  std::vector<std::string> strings{"apple",    "burger",   "cheese",
                                   "doughnut", "eggplant", "fries"};
  std::vector<StringPool::Id> ids;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  auto string = std::make_unique<StringStorage>(&pool, &ids, true);

  // Final vec {"apple", "burger", "doughnut", "eggplant"}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0};
  SelectorStorage storage(std::move(string), &selector_bv);

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue::String("camembert")};
  QueryExecutor exec({&storage}, 4);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0), 2u);
}

TEST(QueryExecutor, StringSearchNeSorted) {
  StringPool pool;
  std::vector<std::string> strings{"apple",    "burger",   "cheese",
                                   "doughnut", "eggplant", "fries"};
  std::vector<StringPool::Id> ids;
  for (const auto& string : strings) {
    ids.push_back(pool.InternString(base::StringView(string)));
  }
  auto string = std::make_unique<StringStorage>(&pool, &ids, true);

  // Final vec {"apple", "burger", "doughnut", "eggplant"}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0};
  SelectorStorage storage(std::move(string), &selector_bv);

  // Filter.
  Constraint c{0, FilterOp::kNe, SqlValue::String("doughnut")};
  QueryExecutor exec({&storage}, 4);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 3u);
  ASSERT_EQ(res.Get(0), 0u);
}

TEST(QueryExecutor, MismatchedTypeIdWithString) {
  IdStorage storage(5);

  // Filter.
  Constraint c{0, FilterOp::kGe, SqlValue::String("cheese")};
  QueryExecutor exec({&storage}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 0u);
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
  auto string = std::make_unique<StringStorage>(&pool, &ids);

  // Final vec {"cheese", "pasta", "NULL", "pierogi", "fries"}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0, 1};
  SelectorStorage storage(std::move(string), &selector_bv);

  // Filter.
  Constraint c{0, FilterOp::kRegex, SqlValue::String("p.*")};
  QueryExecutor exec({&storage}, 5);
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
  auto string = std::make_unique<StringStorage>(&pool, &ids);

  // Final vec {"cheese", "pasta", "NULL", "pierogi", "fries"}.
  BitVector selector_bv{1, 1, 0, 1, 1, 0, 1};
  SelectorStorage storage(std::move(string), &selector_bv);

  // Filter.
  Constraint c{0, FilterOp::kRegex, SqlValue::Long(4)};
  QueryExecutor exec({&storage}, 5);
  RowMap res = exec.Filter({c});

  ASSERT_EQ(res.size(), 0u);
}
#endif

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
