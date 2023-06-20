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
#include "src/trace_processor/db/storage/id_storage.h"
#include "src/trace_processor/db/storage/numeric_storage.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace storage {
namespace {

using Range = RowMap::Range;

TEST(NumericStorageUnittest, StableSortTrivial) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {0, 1, 2, 3, 4, 5, 6, 7, 8};

  NumericStorage storage(data_vec.data(), 9, ColumnType::kUint32);
  RowMap rm(0, 9);
  storage.StableSort(out.data(), 9);

  std::vector<uint32_t> stable_out{0, 3, 6, 1, 4, 7, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(NumericStorageUnittest, StableSort) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {1, 7, 4, 0, 6, 3, 2, 5, 8};

  NumericStorage storage(data_vec.data(), 9, ColumnType::kUint32);
  RowMap rm(0, 9);
  storage.StableSort(out.data(), 9);

  std::vector<uint32_t> stable_out{0, 6, 3, 1, 7, 4, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(NumericStorageUnittest, CompareFast) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 128, ColumnType::kUint32);
  BitVector bv =
      storage.LinearSearch(FilterOp::kGe, SqlValue::Long(100), Range(0, 128));

  ASSERT_EQ(bv.CountSetBits(), 28u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 100u);
}

TEST(NumericStorageUnittest, CompareSorted) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 128, ColumnType::kUint32);
  Range range = storage.BinarySearchIntrinsic(
      FilterOp::kGe, SqlValue::Long(100), Range(0, 128));

  ASSERT_EQ(range.size(), 28u);
  ASSERT_EQ(range.start, 100u);
  ASSERT_EQ(range.end, 128u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesGreaterEqual) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);

  Range range = storage.BinarySearchExtrinsic(FilterOp::kGe, SqlValue::Long(60),
                                              sorted_order.data(), 10);

  ASSERT_EQ(range.size(), 4u);
  ASSERT_EQ(range.start, 6u);
  ASSERT_EQ(range.end, 10u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesLess) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);

  Range range = storage.BinarySearchExtrinsic(FilterOp::kLt, SqlValue::Long(60),
                                              sorted_order.data(), 10);

  ASSERT_EQ(range.size(), 6u);
  ASSERT_EQ(range.start, 0u);
  ASSERT_EQ(range.end, 6u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesEqual) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);

  Range range = storage.BinarySearchExtrinsic(FilterOp::kEq, SqlValue::Long(60),
                                              sorted_order.data(), 10);

  ASSERT_EQ(range.size(), 1u);
  ASSERT_EQ(range.start, 6u);
  ASSERT_EQ(range.end, 7u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicEqSimple) {
  IdStorage storage(100);
  Range range = storage.BinarySearchIntrinsic(FilterOp::kEq, SqlValue::Long(15),
                                              Range(10, 20));
  ASSERT_EQ(range.size(), 1u);
  ASSERT_EQ(range.start, 15u);
  ASSERT_EQ(range.end, 16u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicEqOnRangeBoundary) {
  IdStorage storage(100);
  Range range = storage.BinarySearchIntrinsic(FilterOp::kEq, SqlValue::Long(20),
                                              Range(10, 20));
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicEqOutsideRange) {
  IdStorage storage(100);
  Range range = storage.BinarySearchIntrinsic(FilterOp::kEq, SqlValue::Long(25),
                                              Range(10, 20));
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicEqTooBig) {
  IdStorage storage(100);
  Range range = storage.BinarySearchIntrinsic(
      FilterOp::kEq, SqlValue::Long(125), Range(10, 20));
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicLe) {
  IdStorage storage(100);
  Range range = storage.BinarySearchIntrinsic(FilterOp::kLe, SqlValue::Long(50),
                                              Range(30, 70));
  ASSERT_EQ(range.start, 30u);
  ASSERT_EQ(range.end, 51u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicLt) {
  IdStorage storage(100);
  Range range = storage.BinarySearchIntrinsic(FilterOp::kLt, SqlValue::Long(50),
                                              Range(30, 70));
  ASSERT_EQ(range.start, 30u);
  ASSERT_EQ(range.end, 50u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicGe) {
  IdStorage storage(100);
  Range range = storage.BinarySearchIntrinsic(FilterOp::kGe, SqlValue::Long(40),
                                              Range(30, 70));
  ASSERT_EQ(range.start, 40u);
  ASSERT_EQ(range.end, 70u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicGt) {
  IdStorage storage(100);
  Range range = storage.BinarySearchIntrinsic(FilterOp::kGt, SqlValue::Long(40),
                                              Range(30, 70));
  ASSERT_EQ(range.start, 41u);
  ASSERT_EQ(range.end, 70u);
}

TEST(IdStorageUnittest, Sort) {
  std::vector<uint32_t> order{4, 3, 6, 1, 5};
  IdStorage storage(10);
  storage.Sort(order.data(), 5);

  std::vector<uint32_t> sorted_order{1, 3, 4, 5, 6};
  ASSERT_EQ(order, sorted_order);
}

}  // namespace
}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto
