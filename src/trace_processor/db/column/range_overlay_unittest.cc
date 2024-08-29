/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/db/column/range_overlay.h"

#include <cstdint>
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
using Range = Range;

using Indices = DataLayerChain::Indices;
using OrderedIndices = DataLayerChain::OrderedIndices;

TEST(RangeOverlay, SearchSingle) {
  Range range(3, 8);
  RangeOverlay storage(&range);
  auto fake = FakeStorageChain::SearchSubset(
      8, BitVector{false, false, false, true, false, false, false, false});
  auto chain = storage.MakeChain(std::move(fake));

  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Long(0u), 0),
            SingleSearchResult::kMatch);
  ASSERT_EQ(chain->SingleSearch(FilterOp::kEq, SqlValue::Long(0u), 1),
            SingleSearchResult::kNoMatch);
}

TEST(RangeOverlay, SearchAll) {
  Range range(3, 8);
  RangeOverlay storage(&range);
  auto fake = FakeStorageChain::SearchAll(10);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(1, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1u, 2u, 3u));
}

TEST(RangeOverlay, SearchNone) {
  Range range(3, 8);
  RangeOverlay storage(&range);
  auto fake = FakeStorageChain::SearchNone(10);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(1, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());
}

TEST(RangeOverlay, SearchLimited) {
  auto fake = FakeStorageChain::SearchSubset(10, std::vector<uint32_t>{4});
  Range range(3, 5);
  RangeOverlay storage(&range);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(0, 2));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1u));
}

TEST(RangeOverlay, SearchBitVector) {
  auto fake =
      FakeStorageChain::SearchSubset(8, BitVector({0, 1, 0, 1, 0, 1, 0, 0}));
  Range range(3, 6);
  RangeOverlay storage(&range);
  auto chain = storage.MakeChain(std::move(fake));

  auto res = chain->Search(FilterOp::kGe, SqlValue::Long(0u), Range(0, 3));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2));
}

TEST(RangeOverlay, IndexSearch) {
  auto fake =
      FakeStorageChain::SearchSubset(8, BitVector({0, 1, 0, 1, 0, 1, 0, 0}));

  // {true, false}
  Range range(3, 5);
  RangeOverlay storage(&range);
  auto chain = storage.MakeChain(std::move(fake));

  // {true, false, true}
  Indices indices = Indices::CreateWithIndexPayloadForTesting(
      {0, 1, 0}, Indices::State::kNonmonotonic);
  chain->IndexSearch(FilterOp::kGe, SqlValue::Long(0u), indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 2));
}

TEST(RangeOverlay, StableSort) {
  std::vector<uint32_t> numeric_data{100, 99, 2, 0, 1};
  NumericStorage<uint32_t> numeric(&numeric_data, ColumnType::kUint32, false);

  Range range(2, 4);
  RangeOverlay storage(&range);
  auto chain = storage.MakeChain(numeric.MakeChain());

  std::vector tokens{
      Token{0, 0},
      Token{1, 1},
      Token{2, 2},
  };
  chain->StableSort(tokens.data(), tokens.data() + tokens.size(),
                    SortDirection::kAscending);
  ASSERT_THAT(utils::ExtractPayloadForTesting(tokens), ElementsAre(1, 2, 0));
}

TEST(RangeOverlay, Distinct) {
  std::vector<uint32_t> numeric_data{100, 99, 2, 0, 1};
  NumericStorage<uint32_t> numeric(&numeric_data, ColumnType::kUint32, false);

  // 99, 2, 0, 1
  Range range(1, 4);
  RangeOverlay storage(&range);
  auto chain = storage.MakeChain(numeric.MakeChain());

  auto indices = Indices::CreateWithIndexPayloadForTesting(
      {0, 0, 0}, Indices::State::kNonmonotonic);
  chain->Distinct(indices);
  ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0));
}

}  // namespace
}  // namespace perfetto::trace_processor::column
