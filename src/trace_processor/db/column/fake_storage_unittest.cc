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
#include "src/trace_processor/db/column/fake_storage.h"

#include <cstdint>
#include <limits>
#include <vector>

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {

inline bool operator==(const Range& a, const Range& b) {
  return std::tie(a.start, a.end) == std::tie(b.start, b.end);
}

inline bool operator==(const BitVector& a, const BitVector& b) {
  return a.size() == b.size() && a.CountSetBits() == b.CountSetBits();
}

namespace column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

using Indices = DataLayerChain::Indices;

TEST(FakeStorage, ValidateSearchConstraints) {
  {
    // All passes
    auto fake = FakeStorageChain::SearchAll(10);
    EXPECT_EQ(fake->ValidateSearchConstraints(FilterOp::kEq, SqlValue()),
              SearchValidationResult::kOk);
  }
  {
    // None passes
    auto fake = FakeStorageChain::SearchNone(10);
    EXPECT_EQ(fake->ValidateSearchConstraints(FilterOp::kEq, SqlValue()),
              SearchValidationResult::kOk);
  }
  {
    // Index vector
    auto fake =
        FakeStorageChain::SearchSubset(5, std::vector<uint32_t>{1, 2, 3, 4, 5});
    EXPECT_EQ(fake->ValidateSearchConstraints(FilterOp::kEq, SqlValue()),
              SearchValidationResult::kOk);
  }
  {
    // BitVector
    auto fake = FakeStorageChain::SearchSubset(5, BitVector{0, 1, 0, 1, 0});
    EXPECT_EQ(fake->ValidateSearchConstraints(FilterOp::kEq, SqlValue()),
              SearchValidationResult::kOk);
  }
  {
    // Range
    auto fake = FakeStorageChain::SearchSubset(5, Range(1, 4));
    EXPECT_EQ(fake->ValidateSearchConstraints(FilterOp::kEq, SqlValue()),
              SearchValidationResult::kOk);
  }
}

TEST(FakeStorage, SingleSearch) {
  {
    // All passes
    auto fake = FakeStorageChain::SearchAll(10);
    EXPECT_EQ(fake->SingleSearch(FilterOp::kEq, SqlValue(), 5u),
              SingleSearchResult::kMatch);
  }
  {
    // None passes
    auto fake = FakeStorageChain::SearchNone(10);
    EXPECT_EQ(fake->SingleSearch(FilterOp::kEq, SqlValue(), 5u),
              SingleSearchResult::kNoMatch);
  }
  {
    // Index vector
    auto fake =
        FakeStorageChain::SearchSubset(5, std::vector<uint32_t>{1, 2, 3, 4, 5});
    EXPECT_EQ(fake->SingleSearch(FilterOp::kEq, SqlValue(), 0u),
              SingleSearchResult::kNoMatch);
    EXPECT_EQ(fake->SingleSearch(FilterOp::kEq, SqlValue(), 1u),
              SingleSearchResult::kMatch);
  }
  {
    // BitVector
    auto fake = FakeStorageChain::SearchSubset(5, BitVector{0, 1, 0, 1, 0});
    EXPECT_EQ(fake->SingleSearch(FilterOp::kEq, SqlValue(), 0),
              SingleSearchResult::kNoMatch);
    EXPECT_EQ(fake->SingleSearch(FilterOp::kEq, SqlValue(), 1u),
              SingleSearchResult::kMatch);
  }
  {
    // Range
    auto fake = FakeStorageChain::SearchSubset(5, Range(1, 4));
    EXPECT_EQ(fake->SingleSearch(FilterOp::kEq, SqlValue(), 0),
              SingleSearchResult::kNoMatch);
    EXPECT_EQ(fake->SingleSearch(FilterOp::kEq, SqlValue(), 1u),
              SingleSearchResult::kMatch);
  }
}

TEST(FakeStorage, Search) {
  {
    // All passes
    auto fake = FakeStorageChain::SearchAll(5);
    auto ret = fake->Search(FilterOp::kEq, SqlValue(), Range(1, 3));
    ASSERT_THAT(utils::ToIndexVectorForTests(ret), ElementsAre(1, 2));
  }
  {
    // None passes
    auto fake = FakeStorageChain::SearchNone(5);
    auto ret = fake->Search(FilterOp::kEq, SqlValue(), Range(1, 3));
    ASSERT_THAT(utils::ToIndexVectorForTests(ret), ElementsAre());
  }
  {
    // Index vector
    auto fake =
        FakeStorageChain::SearchSubset(5, std::vector<uint32_t>{1, 2, 4, 5});
    auto ret = fake->Search(FilterOp::kEq, SqlValue(), Range(0, 3));
    ASSERT_THAT(utils::ToIndexVectorForTests(ret), ElementsAre(1, 2));
  }
  {
    // BitVector
    auto fake = FakeStorageChain::SearchSubset(5, BitVector{0, 1, 0, 1, 0});
    auto ret = fake->Search(FilterOp::kEq, SqlValue(), Range(1, 3));
    ASSERT_THAT(utils::ToIndexVectorForTests(ret), ElementsAre(1));
  }
  {
    // Range
    auto fake = FakeStorageChain::SearchSubset(5, Range(2, 4));
    auto ret = fake->Search(FilterOp::kEq, SqlValue(), Range(1, 3));
    ASSERT_THAT(utils::ToIndexVectorForTests(ret), ElementsAre(2));
  }
}

TEST(FakeStorage, IndexSearchValidated) {
  {
    // All passes
    Indices indices = Indices::CreateWithIndexPayloadForTesting(
        {1u, 0u, 3u}, Indices::State::kNonmonotonic);
    auto fake = FakeStorageChain::SearchAll(5);
    fake->IndexSearch(FilterOp::kGe, SqlValue::Long(0u), indices);
    ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 1, 2));
  }
  {
    // None passes
    Indices indices = Indices::CreateWithIndexPayloadForTesting(
        {1u, 0u, 3u}, Indices::State::kNonmonotonic);
    auto fake = FakeStorageChain::SearchNone(5);
    fake->IndexSearch(FilterOp::kGe, SqlValue::Long(0u), indices);
    EXPECT_TRUE(utils::ExtractPayloadForTesting(indices).empty());
  }
  {
    // BitVector
    Indices indices = Indices::CreateWithIndexPayloadForTesting(
        {1u, 0u, 3u}, Indices::State::kNonmonotonic);
    auto fake = FakeStorageChain::SearchSubset(5, BitVector{0, 1, 0, 1, 0});
    fake->IndexSearch(FilterOp::kGe, SqlValue::Long(0u), indices);
    ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 2));
  }
  {
    // Index vector
    Indices indices = Indices::CreateWithIndexPayloadForTesting(
        {1u, 0u, 3u}, Indices::State::kNonmonotonic);
    auto fake =
        FakeStorageChain::SearchSubset(5, std::vector<uint32_t>{1, 2, 3});
    fake->IndexSearch(FilterOp::kGe, SqlValue::Long(0u), indices);
    ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 2));
  }
  {
    // Range
    Indices indices = Indices::CreateWithIndexPayloadForTesting(
        {1u, 0u, 3u}, Indices::State::kNonmonotonic);
    auto fake = FakeStorageChain::SearchSubset(5, Range(1, 4));
    fake->IndexSearch(FilterOp::kGe, SqlValue::Long(0u), indices);
    ASSERT_THAT(utils::ExtractPayloadForTesting(indices), ElementsAre(0, 2));
  }
}

}  // namespace
}  // namespace column
}  // namespace perfetto::trace_processor
