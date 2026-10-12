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

#include "src/trace_processor/core/exec/key_encoder.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// The keys of the `rows` rows of `column`, or of those `kept` keeps.
std::optional<std::vector<std::string>> Keys(
    const ColumnView& column,
    uint32_t rows,
    const std::vector<uint32_t>& kept = {}) {
  RowBatch batch;
  batch.AddBorrowedColumn(column);
  batch.SetRowCount(rows);
  if (!kept.empty()) {
    batch.mutable_selection().Keep(kept);
  }
  KeyEncoder encoder;
  if (encoder.Encode(batch, {0})) {
    return std::nullopt;
  }
  std::vector<std::string> keys;
  for (uint32_t row = 0; row < batch.size(); ++row) {
    keys.emplace_back(encoder.Key(row));
  }
  return keys;
}

TEST(KeyEncoderTest, IntegersOfEveryWidthAgree) {
  int32_t i32[] = {-1, 7};
  uint32_t u32[] = {0, 7};
  int64_t i64[] = {-1, 7};
  auto a = *Keys(ColumnView::Reference(StorageType{Int32{}}, i32), 2);
  auto b = *Keys(ColumnView::Reference(StorageType{Uint32{}}, u32), 2);
  auto c = *Keys(ColumnView::Reference(StorageType{Int64{}}, i64), 2);
  auto ids = *Keys(ColumnView::Reference(StorageType{Id{}}, nullptr), 8);
  EXPECT_EQ(a[1], b[1]);
  EXPECT_EQ(a[1], c[1]);
  EXPECT_EQ(a[0], c[0]);
  EXPECT_EQ(ids[7], c[1]);
}

TEST(KeyEncoderTest, NullsAgreeHoweverTheyAreMarked) {
  StringPool pool;
  StringPool::Id strings[] = {pool.InternString("a"), StringPool::Id::Null()};
  BitVector validity = BitVector::CreateWithSize(2);
  validity.set(1);
  auto by_validity = *Keys(
      ColumnView::Reference(StorageType{String{}}, strings, &validity), 2);
  auto by_id = *Keys(ColumnView::Reference(StorageType{String{}}, strings), 2);
  EXPECT_NE(by_id[0], by_id[1]);
  EXPECT_EQ(by_validity[0], by_id[1]);
  // The null id is 0, so a string needs no null byte.
  EXPECT_EQ(by_id[0].size(), sizeof(uint32_t));
}

TEST(KeyEncoderTest, SelectedRowsKeepValuesAndNulls) {
  BitVector validity = BitVector::CreateWithSize(3);
  validity.set(0);
  validity.set(2);
  auto check = [](ColumnView column) {
    auto original = Keys(column, 3);
    ASSERT_TRUE(original.has_value());
    EXPECT_NE((*original)[0], (*original)[1]);
    EXPECT_NE((*original)[0], (*original)[2]);

    // Keeping some of the rows of a window onto the values. Validity must be
    // checked where the value is stored, not at the row kept.
    auto kept = Keys(column, 3, {0, 2});
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ(*kept,
              (std::vector<std::string>{(*original)[0], (*original)[2]}));
    column.set_start(1);
    auto windowed = Keys(column, 2, {1});
    ASSERT_TRUE(windowed.has_value());
    EXPECT_EQ(*windowed, (std::vector<std::string>{(*original)[2]}));
  };
  int32_t i32[] = {-1, 99, 7};
  uint32_t u32[] = {3, 99, 7};
  int64_t i64[] = {-1, 99, 7};
  double doubles[] = {1.5, 99.0, 7.5};
  StringPool pool;
  StringPool::Id strings[] = {pool.InternString("a"), pool.InternString("b"),
                              pool.InternString("c")};
  check(ColumnView::Reference(StorageType{Int32{}}, i32, &validity));
  check(ColumnView::Reference(StorageType{Uint32{}}, u32, &validity));
  check(ColumnView::Reference(StorageType{Int64{}}, i64, &validity));
  check(ColumnView::Reference(StorageType{Double{}}, doubles, &validity));
  check(ColumnView::Reference(StorageType{String{}}, strings, &validity));
  check(ColumnView::Reference(StorageType{Id{}}, nullptr, &validity));
}

TEST(KeyEncoderTest, OnlyColumnsOfOneTypeAreKeys) {
  double doubles[] = {1.5};
  EXPECT_TRUE(Keys(ColumnView::Reference(StorageType{Double{}}, doubles), 1));
  Variant variants[] = {Variant{}};
  EXPECT_FALSE(Keys(ColumnView::Variants(variants), 1));
}

TEST(KeyEncoderTest, AColumnKeepsItsType) {
  int64_t ints[] = {1};
  double doubles[] = {1.0};
  RowBatch batch;
  batch.AddBorrowedColumn(ColumnView::Reference(StorageType{Int64{}}, ints));
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Double{}}, doubles));
  batch.SetRowCount(1);
  KeyEncoder encoder;
  EXPECT_EQ(encoder.Encode(batch, {0}), std::nullopt);
  EXPECT_EQ(encoder.Encode(batch, {1}), 0u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
