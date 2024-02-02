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

#include "src/trace_processor/db/column/selector_overlay.h"

#include "src/trace_processor/db/column/fake_storage.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

TEST(SelectorOverlay, SearchAll) {
  BitVector selector{0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1};
  SelectorOverlay storage(FakeStorage::SearchAll(10), &selector);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0u), Range(1, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1u, 2u, 3u));
}

TEST(SelectorOverlay, SearchNone) {
  BitVector selector{0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1};
  SelectorOverlay storage(FakeStorage::SearchNone(10), &selector);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0u), Range(1, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());
}

TEST(SelectorOverlay, SearchLimited) {
  BitVector selector{0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1};
  SelectorOverlay storage(FakeStorage::SearchSubset(10, Range(4, 5)),
                          &selector);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0u), Range(1, 5));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2u));
}

TEST(SelectorOverlay, SearchBitVector) {
  BitVector selector{0, 1, 1, 0, 0, 1, 1, 0};
  SelectorOverlay storage(
      FakeStorage::SearchSubset(8, BitVector({0, 1, 0, 1, 0, 1, 0, 0})),
      &selector);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0u), Range(0, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 2));
}

TEST(SelectorOverlay, IndexSearch) {
  BitVector selector{0, 1, 1, 0, 0, 1, 1, 0};
  SelectorOverlay storage(
      FakeStorage::SearchSubset(8, BitVector({0, 1, 0, 1, 0, 1, 0, 0})),
      &selector);

  std::vector<uint32_t> table_idx{1u, 0u, 3u};
  RangeOrBitVector res =
      storage.IndexSearch(FilterOp::kGe, SqlValue::Long(0u), table_idx.data(),
                          static_cast<uint32_t>(table_idx.size()), false);
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1u));
}

}  // namespace
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
