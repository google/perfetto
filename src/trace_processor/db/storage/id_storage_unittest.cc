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

#include "src/trace_processor/db/storage/types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace storage {
namespace {

using Range = RowMap::Range;

TEST(IdStorageUnittest, BinarySearchIntrinsicEqSimple) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(15), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 1u);
  ASSERT_EQ(range.start, 15u);
  ASSERT_EQ(range.end, 16u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicEqOnRangeBoundary) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(20), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicEqOutsideRange) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kEq, SqlValue::Long(25), Range(10, 20))
                    .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicEqTooBig) {
  IdStorage storage(100);
  Range range =
      storage.Search(FilterOp::kEq, SqlValue::Long(125), Range(10, 20))
          .TakeIfRange();
  ASSERT_EQ(range.size(), 0u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicLe) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kLe, SqlValue::Long(50), Range(30, 70))
                    .TakeIfRange();
  ASSERT_EQ(range.start, 30u);
  ASSERT_EQ(range.end, 51u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicLt) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kLt, SqlValue::Long(50), Range(30, 70))
                    .TakeIfRange();
  ASSERT_EQ(range.start, 30u);
  ASSERT_EQ(range.end, 50u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicGe) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kGe, SqlValue::Long(40), Range(30, 70))
                    .TakeIfRange();
  ASSERT_EQ(range.start, 40u);
  ASSERT_EQ(range.end, 70u);
}

TEST(IdStorageUnittest, BinarySearchIntrinsicGt) {
  IdStorage storage(100);
  Range range = storage.Search(FilterOp::kGt, SqlValue::Long(40), Range(30, 70))
                    .TakeIfRange();
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
