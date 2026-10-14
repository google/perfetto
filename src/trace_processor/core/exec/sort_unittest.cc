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

#include "src/trace_processor/core/exec/sort.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

std::vector<uint32_t> Ids(const Source& source, base::Status* status) {
  std::unique_ptr<OperatorState> state = source.MakeState(test::TestContext());
  RowBatch batch;
  std::vector<uint32_t> ids;
  while (source.GetData(batch, *state)) {
    for (uint32_t id : test::ReadColumn<uint32_t>(batch, 0)) {
      ids.push_back(id);
    }
  }
  *status = source.status(*state);
  return ids;
}

std::vector<Pipeline::Step> SortBy(std::vector<SortSpec::Key> keys) {
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<Sort>(SortSpec{std::move(keys)}));
  return ops;
}

TEST(SortTest, RewindAfterInvalidKeysAndCompletedSort) {
  Sort sort(SortSpec{{{1, false}, {2, true}}});
  auto state = sort.MakeState(test::TestContext());
  std::vector<int64_t> first = {2, 1, 2, 1};
  std::vector<int32_t> narrower = {2, 1, 2, 1};
  std::vector<double> second = {0.5, -1.5, -0.5, 2.5};
  RowBatch in;
  RowBatch out;
  in.AddBorrowedColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
  in.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, first.data()));
  // The first key is valid, but the second cannot be used in a row layout.
  in.AddBorrowedColumn(ColumnView::Variants(nullptr));
  in.SetRowCount(4);
  ASSERT_EQ(sort.Execute(in, out, *state), OpResult::kError);
  EXPECT_THAT(sort.status(*state).message(), testing::HasSubstr("key 2"));

  for (bool change_type : {false, true}) {
    // Recover from the rejected batch, then reuse a completed sort with a
    // different key width. Both runs must establish their own row layout.
    state->Reset();
    ASSERT_TRUE(sort.status(*state).ok());
    if (change_type) {
      in.SetColumn(1,
                   ColumnView::Reference(StorageType{Int32{}}, narrower.data()),
                   ColumnBuffer());
    }
    in.SetColumn(2, ColumnView::Reference(StorageType{Double{}}, second.data()),
                 ColumnBuffer());
    ASSERT_EQ(sort.Execute(in, out, *state), OpResult::kNeedMoreInput);
    ASSERT_EQ(sort.Finish(out, *state), OpResult::kHaveMoreOutput);
    // Equal first keys are ordered by the descending second key.
    EXPECT_THAT(test::ReadColumn<uint32_t>(out, 0), ElementsAre(3, 1, 0, 2));
    EXPECT_EQ(sort.Finish(out, *state), OpResult::kNeedMoreInput);
    EXPECT_TRUE(sort.status(*state).ok());
  }
}

TEST(SortTest, IdsSortAsTheirValues) {
  test::ArraySource source({5, 4, 3});
  Pipeline sorted(source, SortBy({{0, true}}), {});
  base::Status status;
  EXPECT_THAT(Ids(sorted, &status), ElementsAre(2, 1, 0));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
