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

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/fake_storage.h"
#include "src/trace_processor/db/column/numeric_storage.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

TEST(NullOverlay, SingleSearch) {
  BitVector bv{0, 1, 0, 1, 1, 1};
  auto fake = FakeStorageChain::SearchSubset(4, std::vector<uint32_t>{1, 2});
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0u), 3),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0u), 1),
            SingleSearchResult::kNoMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0u), 2),
            SingleSearchResult::kNoMatch);
}

TEST(NullOverlay, SingleSearchIsNull) {
  BitVector bv{0, 1, 0, 1, 1, 1};
  auto fake = FakeStorageChain::SearchNone(4);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNull, SqlValue(), 0),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNull, SqlValue(), 1),
            SingleSearchResult::kNoMatch);
}

TEST(NullOverlay, SingleSearchIsNotNull) {
  BitVector bv{0, 1, 0, 1, 1, 1};
  auto fake = FakeStorageChain::SearchAll(4);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNotNull, SqlValue(), 1),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNotNull, SqlValue(), 0),
            SingleSearchResult::kNoMatch);
}

TEST(NullOverlay, UniqueSearch) {
  BitVector bv{0, 0, 0, 1, 1};
  NullOverlay storage(&bv);
  auto fake = FakeStorageChain::SearchSubset(5, Range(1, 2));
  auto chain = storage.MakeChain(std::move(fake));

  uint32_t row = std::numeric_limits<uint32_t>::max();
  ASSERT_EQ(chain->UniqueSearch(FilterOp::kIsNotNull, SqlValue(), &row),
            UniqueSearchResult::kMatch);
  ASSERT_EQ(row, 4u);
}

TEST(NullOverlay, UniqueSearchOutOfBounds) {
  BitVector bv{0, 0, 0, 1, 1};
  NullOverlay storage(&bv);
  auto fake = FakeStorageChain::SearchSubset(5, Range(4, 5));
  auto chain = storage.MakeChain(std::move(fake));

  uint32_t row = std::numeric_limits<uint32_t>::max();
  ASSERT_EQ(chain->UniqueSearch(FilterOp::kIsNotNull, SqlValue(), &row),
            UniqueSearchResult::kNoMatch);
}

TEST(NullOverlay, SearchInputInsideBoundary) {
  BitVector bv{0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  auto fake = FakeStorageChain::SearchAll(4u);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGt, SqlValue::Long(0), Range(1, 6));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4));
}

TEST(NullOverlay, SearchInputOutsideBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  auto fake = FakeStorageChain::SearchAll(5u);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGt, SqlValue::Long(0), Range(3, 8));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4, 7));
}

TEST(NullOverlay, SubsetResultOutsideBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  auto fake = FakeStorageChain::SearchSubset(5u, Range(1, 3));
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGt, SqlValue::Long(0), Range(0, 11));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(3, 4));
}

TEST(NullOverlay, SubsetResultOnBoundary) {
  BitVector bv{0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0};
  auto fake = FakeStorageChain::SearchAll(5u);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGt, SqlValue::Long(0), Range(0, 11));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3, 4, 7, 8));
}

TEST(NullOverlay, BitVectorSubset) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  auto fake = FakeStorageChain::SearchSubset(4u, BitVector{0, 1, 0, 1});
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGt, SqlValue::Long(0), Range(0, 8));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2, 6));
}

TEST(NullOverlay, BitVectorSubsetIsNull) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  auto fake = FakeStorageChain::SearchSubset(4u, BitVector{0, 1, 0, 1});
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kIsNull, SqlValue(), Range(0, 8));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2, 3, 4, 6, 7));
}

TEST(NullOverlay, IndexSearchAllElements) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  auto fake = FakeStorageChain::SearchAll(4u);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  std::vector<uint32_t> table_idx{1, 5, 2};
  auto res =
      chain->IndexSearch(FilterOp::kGt, SqlValue::Long(0),
                         Indices{table_idx.data(), uint32_t(table_idx.size()),
                                 Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2));
}

TEST(NullOverlay, IndexSearchPartialElements) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  auto fake = FakeStorageChain::SearchAll(4u);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  std::vector<uint32_t> table_idx{1, 4, 2};
  auto res =
      chain->IndexSearch(FilterOp::kGt, SqlValue::Long(0),
                         Indices{table_idx.data(), uint32_t(table_idx.size()),
                                 Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2));
}

TEST(NullOverlay, IndexSearchIsNullOpEmptyRes) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  auto fake = FakeStorageChain::SearchNone(4u);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  std::vector<uint32_t> table_idx{0, 3, 5, 4, 2};
  auto res =
      chain->IndexSearch(FilterOp::kIsNull, SqlValue(),
                         Indices{table_idx.data(), uint32_t(table_idx.size()),
                                 Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 3));
}

TEST(NullOverlay, IndexSearchIsNullOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  auto fake = FakeStorageChain::SearchSubset(4u, Range(2, 3));
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  std::vector<uint32_t> table_idx{0, 3, 2, 4, 5};
  auto res =
      chain->IndexSearch(FilterOp::kIsNull, SqlValue(),
                         Indices{table_idx.data(), uint32_t(table_idx.size()),
                                 Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 3, 4));
}

TEST(NullOverlay, IndexSearchIsNotNullOp) {
  BitVector bv{0, 1, 1, 0, 0, 1, 1, 0};
  auto fake = FakeStorageChain::SearchAll(4u);
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  std::vector<uint32_t> table_idx{0, 3, 4};
  auto res =
      chain->IndexSearch(FilterOp::kIsNotNull, SqlValue(),
                         Indices{table_idx.data(), uint32_t(table_idx.size()),
                                 Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());
}

TEST(NullOverlay, OrderedIndexSearch) {
  BitVector bv{0, 1, 1, 1, 0, 1};
  // Passing values in final storage (on normal operations)
  // 0, 1, 0, 1, 0, 0
  auto fake = FakeStorageChain::SearchSubset(4, BitVector{1, 0, 1, 0});
  NullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  // Passing values on final data
  // NULL, NULL, 0, 1, 1
  std::vector<uint32_t> table_idx{0, 4, 5, 1, 3};
  Indices indices{table_idx.data(), uint32_t(table_idx.size()),
                  Indices::State::kNonmonotonic};

  Range res = chain->OrderedIndexSearch(FilterOp::kIsNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 2u);

  res = chain->OrderedIndexSearch(FilterOp::kIsNotNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = chain->OrderedIndexSearch(FilterOp::kEq, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = chain->OrderedIndexSearch(FilterOp::kGt, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = chain->OrderedIndexSearch(FilterOp::kGe, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = chain->OrderedIndexSearch(FilterOp::kLt, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);

  res = chain->OrderedIndexSearch(FilterOp::kLe, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 5u);
}

TEST(NullOverlay, StableSort) {
  std::vector<uint32_t> numeric_data{3, 1, 0, 2, 4};
  NumericStorage<uint32_t> numeric(&numeric_data, ColumnType::kUint32, false);

  BitVector null{0, 1, 0, 1, 1, 1, 1};
  NullOverlay overlay(&null);
  auto chain = overlay.MakeChain(numeric.MakeChain());

  auto make_tokens = []() {
    return std::vector{
        column::DataLayerChain::SortToken{0, 0},
        column::DataLayerChain::SortToken{1, 1},
        column::DataLayerChain::SortToken{2, 2},
        column::DataLayerChain::SortToken{3, 3},
        column::DataLayerChain::SortToken{4, 4},
        column::DataLayerChain::SortToken{5, 5},
        column::DataLayerChain::SortToken{6, 6},
    };
  };
  {
    auto tokens = make_tokens();
    chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                      column::DataLayerChain::SortDirection::kAscending);
    ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
                ElementsAre(0, 2, 4, 3, 5, 1, 6));
  }
  {
    auto tokens = make_tokens();
    chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                      column::DataLayerChain::SortDirection::kDescending);
    ASSERT_THAT(utils::ExtractPayloadForTesting(tokens),
                ElementsAre(6, 1, 5, 3, 4, 0, 2));
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::column
