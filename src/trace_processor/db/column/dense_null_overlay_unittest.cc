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

#include "src/trace_processor/db/column/dense_null_overlay.h"

#include <cstdint>
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

TEST(DenseNullOverlay, NoFilteringSearch) {
  std::vector<uint32_t> data{0, 1, 0, 1, 0};
  auto numeric = std::make_unique<NumericStorage<uint32_t>>(
      &data, ColumnType::kUint32, false);

  BitVector bv{0, 1, 0, 1, 0};
  DenseNullOverlay storage(&bv);
  auto chain = storage.MakeChain(numeric->MakeChain());

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0), Range(0, 5));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1, 3));
}

TEST(DenseNullOverlay, RestrictInputSearch) {
  std::vector<uint32_t> data{0, 1, 0, 1, 0};
  auto numeric = std::make_unique<NumericStorage<uint32_t>>(
      &data, ColumnType::kUint32, false);

  BitVector bv{0, 1, 0, 1, 0};
  DenseNullOverlay storage(&bv);
  auto chain = storage.MakeChain(numeric->MakeChain());

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0), Range(1, 3));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));
}

TEST(DenseNullOverlay, RangeFilterSearch) {
  auto fake = FakeStorageChain::SearchSubset(5, Range(1, 3));

  BitVector bv{0, 1, 0, 1, 0};
  DenseNullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0), Range(0, 5));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));
}

TEST(DenseNullOverlay, BitvectorFilterSearch) {
  auto fake = FakeStorageChain::SearchSubset(5, BitVector({0, 1, 1, 0, 0}));

  BitVector bv{0, 1, 0, 1, 0};
  DenseNullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0), Range(0, 5));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1));
}

TEST(DenseNullOverlay, IsNullSearch) {
  auto fake = FakeStorageChain::SearchSubset(5, BitVector({1, 1, 0, 0, 1}));

  BitVector bv{1, 0, 0, 1, 1};
  DenseNullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kIsNull, SqlValue(), Range(0, 5));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 4));
}

TEST(DenseNullOverlay, IndexSearch) {
  std::vector<uint32_t> data{1, 0, 0, 1, 1, 1};
  auto numeric = std::make_unique<NumericStorage<uint32_t>>(
      &data, ColumnType::kUint32, false);

  BitVector bv{1, 0, 0, 1, 1, 1};
  DenseNullOverlay storage(&bv);
  auto chain = storage.MakeChain(numeric->MakeChain());

  std::vector<uint32_t> index({5, 2, 3, 4, 1});
  auto res = chain->IndexSearch(
      FilterOp::kGe, SqlValue::Long(0),
      Indices{index.data(), static_cast<uint32_t>(index.size()),
              Indices::State::kNonmonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2, 3));
}

TEST(DenseNullOverlay, IsNullIndexSearch) {
  auto fake = FakeStorageChain::SearchSubset(6, BitVector({0, 0, 0, 1, 1, 1}));

  BitVector bv{0, 1, 0, 1, 1, 1};
  DenseNullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  std::vector<uint32_t> index({5, 2, 3, 4, 1});
  auto res = chain->IndexSearch(
      FilterOp::kIsNull, SqlValue(),
      Indices{index.data(), static_cast<uint32_t>(index.size()),
              Indices::State::kMonotonic});
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 2, 3));
}

TEST(DenseNullOverlay, OrderedIndexSearch) {
  auto fake = FakeStorageChain::SearchSubset(6, BitVector({0, 1, 0, 1, 0, 1}));

  BitVector bv{0, 1, 0, 1, 0, 1};
  DenseNullOverlay storage(&bv);
  auto chain = storage.MakeChain(std::move(fake));

  std::vector<uint32_t> indices_vec({0, 2, 4, 1, 3, 5});
  Indices indices{indices_vec.data(), 6, Indices::State::kNonmonotonic};

  Range res = chain->OrderedIndexSearch(FilterOp::kIsNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 0u);
  ASSERT_EQ(res.end, 3u);

  res = chain->OrderedIndexSearch(FilterOp::kIsNotNull, SqlValue(), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 6u);

  res = chain->OrderedIndexSearch(FilterOp::kEq, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 6u);

  res = chain->OrderedIndexSearch(FilterOp::kGt, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 6u);

  res = chain->OrderedIndexSearch(FilterOp::kGe, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 6u);

  res = chain->OrderedIndexSearch(FilterOp::kLt, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 6u);

  res = chain->OrderedIndexSearch(FilterOp::kLe, SqlValue::Long(3), indices);
  ASSERT_EQ(res.start, 3u);
  ASSERT_EQ(res.end, 6u);
}

TEST(DenseNullOverlay, SingleSearch) {
  BitVector bv{0, 1, 0, 1, 1, 1};
  DenseNullOverlay storage(&bv);
  auto fake = FakeStorageChain::SearchSubset(5, std::vector<uint32_t>{1, 2});
  auto chain = storage.MakeChain(std::move(fake));

  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0u), 1),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kGe, SqlValue::Long(0u), 2),
            SingleSearchResult::kNoMatch);
}

TEST(DenseNullOverlay, SingleSearchIsNull) {
  BitVector bv{0, 1, 0, 1, 1, 1};
  DenseNullOverlay storage(&bv);
  auto fake = FakeStorageChain::SearchNone(5);
  auto chain = storage.MakeChain(std::move(fake));

  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNull, SqlValue(), 0),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNull, SqlValue(), 1),
            SingleSearchResult::kNoMatch);
}

TEST(DenseNullOverlay, SingleSearchIsNotNull) {
  BitVector bv{0, 1, 0, 1, 1, 1};
  DenseNullOverlay storage(&bv);
  auto fake = FakeStorageChain::SearchAll(5);
  auto chain = storage.MakeChain(std::move(fake));

  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNotNull, SqlValue(), 0),
            SingleSearchResult::kNoMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kIsNotNull, SqlValue(), 1),
            SingleSearchResult::kMatch);
}

TEST(DenseNullOverlay, StableSort) {
  std::vector<uint32_t> numeric_data{0, 3, 0, 1, 0, 2, 4};
  NumericStorage<uint32_t> numeric(&numeric_data, ColumnType::kUint32, false);

  BitVector null{0, 1, 0, 1, 1, 1, 1};
  DenseNullOverlay overlay(&null);
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
