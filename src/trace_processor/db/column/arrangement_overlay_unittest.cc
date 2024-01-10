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

#include "src/trace_processor/db/column/arrangement_overlay.h"

#include "src/trace_processor/db/column/fake_storage.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace column {
namespace {

using testing::ElementsAre;
using testing::IsEmpty;

TEST(ArrangementOverlay, SearchAll) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay storage(FakeStorage::SearchAll(5), &arrangement);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0u), Range(2, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(2u, 3u));
}

TEST(ArrangementOverlay, SearchNone) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay storage(FakeStorage::SearchNone(5), &arrangement);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0u), Range(2, 4));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), IsEmpty());
}

TEST(ArrangementOverlay, DISABLED_SearchLimited) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay storage(FakeStorage::SearchSubset(5, Range(4, 5)),
                             &arrangement);

  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0u), Range(2, 7));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(6u));
}

TEST(ArrangementOverlay, SearchBitVector) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay storage(
      FakeStorage::SearchSubset(5, BitVector({0, 1, 0, 1, 0})), &arrangement);

  // Table bv:
  // 1, 1, 0, 0, 1, 1, 0, 0, 1, 1
  auto res = storage.Search(FilterOp::kGe, SqlValue::Long(0u), Range(0, 10));
  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(0, 1, 4, 5, 8, 9));
}

TEST(ArrangementOverlay, IndexSearch) {
  std::vector<uint32_t> arrangement{1, 1, 2, 2, 3, 3, 4, 4, 1, 1};
  ArrangementOverlay storage(
      FakeStorage::SearchSubset(5, BitVector({0, 1, 0, 1, 0})), &arrangement);

  std::vector<uint32_t> table_idx{7u, 1u, 3u};
  RangeOrBitVector res =
      storage.IndexSearch(FilterOp::kGe, SqlValue::Long(0u), table_idx.data(),
                          static_cast<uint32_t>(table_idx.size()), false);

  ASSERT_THAT(utils::ToIndexVectorForTests(res), ElementsAre(1u));
}

}  // namespace
}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
