/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace column {

namespace {

TEST(StorageUnittest, StableSortTrivial) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {0, 1, 2, 3, 4, 5, 6, 7, 8};

  NumericStorage storage(data_vec.data(), ColumnType::kUint32);
  RowMap rm(0, 9);
  storage.StableSort(out);

  std::vector<uint32_t> stable_out{0, 3, 6, 1, 4, 7, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(StorageUnittest, StableSort) {
  std::vector<uint32_t> data_vec{0, 1, 2, 0, 1, 2, 0, 1, 2};
  std::vector<uint32_t> out = {1, 7, 4, 0, 6, 3, 2, 5, 8};

  NumericStorage storage(data_vec.data(), ColumnType::kUint32);
  RowMap rm(0, 9);
  storage.StableSort(out);

  std::vector<uint32_t> stable_out{0, 6, 3, 1, 7, 4, 2, 5, 8};
  ASSERT_EQ(out, stable_out);
}

TEST(StorageUnittest, CompareSlow) {
  uint32_t size = 10;
  std::vector<uint32_t> data_vec(size);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), ColumnType::kUint32);
  BitVector::Builder builder(size);
  storage.CompareSlow(FilterOp::kGe, SqlValue::Long(5), data_vec.data(), size,
                      builder);
  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.CountSetBits(), 5u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 5u);
}

TEST(StorageUnittest, CompareSlowLarge) {
  uint32_t size = 1025;
  std::vector<uint32_t> data_vec(size);
  std::iota(data_vec.begin(), data_vec.end(), 0);
  NumericStorage storage(data_vec.data(), ColumnType::kUint32);
  BitVector::Builder builder(size);
  storage.CompareSlow(FilterOp::kGe, SqlValue::Long(5), data_vec.data(), size,
                      builder);
  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.CountSetBits(), 1020u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 5u);
}

TEST(StorageUnittest, CompareFast) {
  std::vector<uint32_t> data_vec;
  for (uint32_t i = 0; i < 128; ++i) {
    data_vec.push_back(i);
  }
  NumericStorage storage(data_vec.data(), ColumnType::kUint32);
  BitVector::Builder builder(128);
  storage.CompareFast(FilterOp::kGe, SqlValue::Long(100), data_vec.data(), 128,
                      builder);
  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.CountSetBits(), 28u);
  ASSERT_EQ(bv.IndexOfNthSet(0), 100u);
}

TEST(StorageUnittest, CompareSorted) {
  std::vector<uint32_t> data_vec;
  for (uint32_t i = 0; i < 128; ++i) {
    data_vec.push_back(i);
  }
  NumericStorage storage(data_vec.data(), ColumnType::kUint32);
  RowMap rm(0, 128);
  storage.CompareSorted(FilterOp::kGe, SqlValue::Long(100), data_vec.data(),
                        128, rm);

  ASSERT_EQ(rm.size(), 28u);
  ASSERT_EQ(rm.Get(0), 100u);
}

}  // namespace
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
