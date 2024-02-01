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

#include "src/trace_processor/db/column/null_overlay.h"

#include <memory>
#include <vector>

#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column/fake_storage.h"
#include "src/trace_processor/db/column/numeric_storage.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

TEST(NullOverlay, SearchInputInsideBoundary) {
  BitVector bv{0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  NullOverlay storage(FakeStorage::SearchAll(4u), &bv);

  auto res = storage.Search(FilterOp::kGt, SqlValue::Long(0), Range(1, 6));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4));
}

TEST(NullOverlay, SearchInputOutsideBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  NullOverlay storage(FakeStorage::SearchAll(5u), &bv);

  auto res = storage.Search(FilterOp::kGt, SqlValue::Long(0), Range(3, 8));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 7));
}

TEST(NullOverlay, SubsetResultOutsideBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  NullOverlay storage(FakeStorage::SearchSubset(5u, Range(1, 3)), &bv);

  auto res = storage.Search(FilterOp::kGt, SqlValue::Long(0), Range(0, 11));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4));
}

TEST(NullOverlay, SubsetResultOnBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  NullOverlay storage(FakeStorage::SearchAll(5u), &bv);

  auto res = storage.Search(FilterOp::kGt, SqlValue::Long(0), Range(0, 11));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3, 4, 7, 8));
}

TEST(NullOverlay, BitVectorSubset) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay storage(FakeStorage::SearchSubset(4u, BitVector{0, 1, 0, 1}),
                      &bv);

  auto res = storage.Search(FilterOp::kGt, SqlValue::Long(0), Range(0, 8));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 6));
}

TEST(NullOverlay, BitVectorSubsetIsNull) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay storage(FakeStorage::SearchSubset(4u, BitVector{0, 1, 0, 1}),
                      &bv);

  auto res = storage.Search(FilterOp::kIsNull, SqlValue(), Range(0, 8));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2, 3, 4, 6, 7));
}

TEST(NullOverlay, IndexSearchAllElements) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay storage(FakeStorage::SearchAll(4u), &bv);

  std::vector<uint32_t> table_idx{1, 5, 2};
  auto res =
      storage.IndexSearch(FilterOp::kGt, SqlValue::Long(0),
                          Indices{table_idx.data(), uint32_t(table_idx.size()),
                                  Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2));
}

TEST(NullOverlay, IndexSearchPartialElements) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay storage(FakeStorage::SearchAll(4u), &bv);

  std::vector<uint32_t> table_idx{1, 4, 2};
  auto res =
      storage.IndexSearch(FilterOp::kGt, SqlValue::Long(0),
                          Indices{table_idx.data(), uint32_t(table_idx.size()),
                                  Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2));
}

TEST(NullOverlay, IndexSearchIsNullOpEmptyRes) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay storage(FakeStorage::SearchNone(4u), &bv);

  std::vector<uint32_t> table_idx{0, 3, 5, 4, 2};
  auto res =
      storage.IndexSearch(FilterOp::kIsNull, SqlValue(),
                          Indices{table_idx.data(), uint32_t(table_idx.size()),
                                  Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 3));
}

TEST(NullOverlay, IndexSearchIsNullOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay storage(FakeStorage::SearchSubset(4u, Range(2, 3)), &bv);

  std::vector<uint32_t> table_idx{0, 3, 2, 4, 5};
  auto res =
      storage.IndexSearch(FilterOp::kIsNull, SqlValue(),
                          Indices{table_idx.data(), uint32_t(table_idx.size()),
                                  Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 3, 4));
}

TEST(NullOverlay, IndexSearchIsNotNullOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  NullOverlay storage(FakeStorage::SearchAll(4u), &bv);

  std::vector<uint32_t> table_idx{0, 3, 4};
  auto res =
      storage.IndexSearch(FilterOp::kIsNotNull, SqlValue(),
                          Indices{table_idx.data(), uint32_t(table_idx.size()),
                                  Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());
}

TEST(NullOverlay, OrderedIndexSearch) {
  BitVector bv{0, 1, 1, 1, 0, 1};
  // Passing values in final storage (on normal operations)
  // 0, 1, 0, 1, 0, 0
  NullOverlay storage(FakeStorage::SearchSubset(4, BitVector{1, 0, 1, 0}), &bv);

  // Passing values on final data
  // NULL, NULL, 0, 1, 1
  std::vector<uint32_t> table_idx{0, 4, 5, 1, 3};
  Indices indices{table_idx.data(), uint32_t(table_idx.size()),
                  Indices::State::kNonmonotonic};

  Range res =
      storage.OrderedIndexSearch(FilterOp::kIsNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 2u);

  res = storage.OrderedIndexSearch(FilterOp::kIsNotNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = storage.OrderedIndexSearch(FilterOp::kEq, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = storage.OrderedIndexSearch(FilterOp::kGt, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = storage.OrderedIndexSearch(FilterOp::kGe, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = storage.OrderedIndexSearch(FilterOp::kLt, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = storage.OrderedIndexSearch(FilterOp::kLe, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);
}

}  // namespace
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
