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

#include <numeric>
#include "src/trace_processor/db/numeric_storage.h"

#include "src/trace_processor/db/null_overlay.h"
#include "src/trace_processor/db/storage_overlay.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace column {

namespace {

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

TEST(NumericStorageUnittest, CompareSlow) {
  uint32_t size = 10;
  std::vector<uint32_t> data_vec(size);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), size, ColumnType::kUint32);
  BitVector::Builder builder(size);
  storage.CompareSlow(FilterOp::kGe, SqlValue::Long(5), 0, size, builder);
  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.CountSetBits(), 5u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 5u);
}

TEST(NumericStorageUnittest, CompareSlowLarge) {
  uint32_t size = 1025;
  std::vector<uint32_t> data_vec(size);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), size, ColumnType::kUint32);
  BitVector::Builder builder(size);
  storage.CompareSlow(FilterOp::kGe, SqlValue::Long(5), 0, size, builder);
  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.CountSetBits(), 1020u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 5u);
}

TEST(NumericStorageUnittest, CompareFast) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 128, ColumnType::kUint32);
  BitVector::Builder builder(128);
  storage.CompareFast(FilterOp::kGe, SqlValue::Long(100), 0, 128, builder);
  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.CountSetBits(), 28u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 100u);
}

TEST(NumericStorageUnittest, CompareSorted) {
  std::vector<uint32_t> data_vec(128);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 128, ColumnType::kUint32);
  RowMap rm(0, 128);
  storage.CompareSorted(FilterOp::kGe, SqlValue::Long(100), rm);

  ASSERT_EQ(rm.size(), 28u);
  ASSERT_EQ(rm.Get(0), 100u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesGreaterEqual) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);
  RowMap rm(0, 10);

  storage.CompareSortedIndexes(FilterOp::kGe, SqlValue::Long(60),
                               sorted_order.data(), rm);

  ASSERT_EQ(rm.size(), 4u);
  ASSERT_EQ(rm.Get(0), 3u);
  ASSERT_EQ(rm.Get(1), 6u);
  ASSERT_EQ(rm.Get(2), 5u);
  ASSERT_EQ(rm.Get(3), 4u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesLess) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);
  RowMap rm(0, 10);

  storage.CompareSortedIndexes(FilterOp::kLt, SqlValue::Long(60),
                               sorted_order.data(), rm);

  ASSERT_EQ(rm.size(), 6u);
  ASSERT_EQ(rm.Get(0), 7u);
}

TEST(NumericStorageUnittest, CompareSortedIndexesEqual) {
  std::vector<uint32_t> data_vec{30, 40, 50, 60, 90, 80, 70, 0, 10, 20};
  std::vector<uint32_t> sorted_order{7, 8, 9, 0, 1, 2, 3, 6, 5, 4};

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);
  RowMap rm(0, 10);

  storage.CompareSortedIndexes(FilterOp::kEq, SqlValue::Long(60),
                               sorted_order.data(), rm);

  ASSERT_EQ(rm.size(), 1u);
  ASSERT_EQ(rm.Get(0), 3u);
}

TEST(StorageOverlayUnittests, FilterIsNull) {
  std::vector<uint32_t> data_vec(1025);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 1025, ColumnType::kUint32);
  StorageOverlay overlay(&storage);

  RowMap rm(0, 1025);
  overlay.Filter(FilterOp::kIsNull, SqlValue::Long(0), rm);

  ASSERT_EQ(rm.size(), 0u);
}

TEST(StorageOverlayUnittests, FilterIsNotNull) {
  std::vector<uint32_t> data_vec(1025);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 1025, ColumnType::kUint32);
  StorageOverlay overlay(&storage);

  RowMap rm(0, 1025);
  overlay.Filter(FilterOp::kIsNotNull, SqlValue::Long(0), rm);

  ASSERT_EQ(rm.size(), 1025u);
}

TEST(StorageOverlayUnittests, Filter) {
  std::vector<uint32_t> data_vec(1025);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 1025, ColumnType::kUint32);
  StorageOverlay overlay(&storage);

  RowMap rm(0, 1025);
  overlay.Filter(FilterOp::kGe, SqlValue::Long(200), rm);

  ASSERT_EQ(rm.size(), 825u);
  ASSERT_EQ(rm.Get(0), 200u);
}

TEST(StorageOverlayUnittests, Sort) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {1, 7, 4, 0, 6, 3, 2, 5, 8};

  NumericStorage storage(data_vec.data(), 9, ColumnType::kUint32);
  StorageOverlay overlay(&storage);

  overlay.StableSort(out.data(), 9);

  std::vector<uint32_t> stable_out{0, 6, 3, 1, 7, 4, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(NullOverlayUnittest, FilterIsNull) {
  std::vector<uint32_t> data_vec(10);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  BitVector bv = BitVector::Range(0, 20, [](uint32_t t) { return t % 2 == 0; });

  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);
  std::unique_ptr<ColumnOverlayOld> storage_overlay(
      new StorageOverlay(&storage));
  NullOverlay overlay(std::move(storage_overlay), &bv);

  RowMap rm(0, 10);
  overlay.Filter(FilterOp::kIsNull, SqlValue::Long(5), rm);
  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(0), 1u);
}

TEST(NullOverlayUnittest, FilterIsNotNull) {
  std::vector<uint32_t> data_vec(10);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), 10, ColumnType::kUint32);
  BitVector bv = BitVector::Range(0, 20, [](uint32_t t) { return t % 2 == 0; });
  std::unique_ptr<ColumnOverlayOld> storage_overlay(
      new StorageOverlay(&storage));
  NullOverlay overlay(std::move(storage_overlay), &bv);

  RowMap rm(0, 10);
  overlay.Filter(FilterOp::kIsNotNull, SqlValue::Long(5), rm);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(0), 0u);
}

TEST(NullOverlayUnittest, Filter) {
  uint32_t bv_size = 20;
  uint32_t data_size = 10;

  // Prepare storage
  std::vector<uint32_t> data_vec(data_size);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), data_size, ColumnType::kUint32);

  // Prepare NullOverlay
  BitVector bv =
      BitVector::Range(0, bv_size, [](uint32_t t) { return t % 2 == 0; });
  std::unique_ptr<ColumnOverlayOld> storage_overlay(
      new StorageOverlay(&storage));
  NullOverlay overlay(std::move(storage_overlay), &bv);

  RowMap rm(0, bv_size);
  overlay.Filter(FilterOp::kGe, SqlValue::Long(5), rm);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(0), 10u);
}

TEST(NullOverlayUnittest, FilterLarge) {
  uint32_t bv_size = 1000;
  uint32_t data_size = 100;

  // Prepare storage
  std::vector<uint32_t> data_vec(data_size);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), data_size, ColumnType::kUint32);

  // Prepare NullOverlay
  BitVector bv =
      BitVector::Range(800, bv_size, [](uint32_t t) { return t % 2 == 0; });
  std::unique_ptr<ColumnOverlayOld> storage_overlay(
      new StorageOverlay(&storage));
  NullOverlay overlay(std::move(storage_overlay), &bv);

  RowMap rm(0, bv_size);
  overlay.Filter(FilterOp::kGe, SqlValue::Long(50), rm);

  ASSERT_EQ(rm.size(), 50u);
  ASSERT_EQ(rm.Get(0), 900u);
}

TEST(NullOverlayUnittest, Sort) {
  // Prepare storage
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out(18);
  std::iota(out.begin(), out.end(), 0);
  NumericStorage storage(data_vec.data(), 9, ColumnType::kUint32);

  // Prepare NullOverlay
  BitVector bv = BitVector::Range(0, 18, [](uint32_t t) { return t % 2 == 0; });
  std::unique_ptr<ColumnOverlayOld> storage_overlay(
      new StorageOverlay(&storage));
  NullOverlay overlay(std::move(storage_overlay), &bv);

  overlay.StableSort(out.data(), 18);

  std::vector<uint32_t> stable_out{1, 3, 5,  7, 9, 11, 13, 15, 17,
                                   0, 6, 12, 2, 8, 14, 4,  10, 16};
  ASSERT_EQ(out, stable_out);
}

}  // namespace
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
