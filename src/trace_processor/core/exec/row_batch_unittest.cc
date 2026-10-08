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

#include "src/trace_processor/core/exec/row_batch.h"

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// A batch's selection is its own: narrowing a copy of it, which shares its
// columns, leaves the batch it came from as it was.
TEST(RowBatchTest, NarrowingACopyLeavesTheOriginalAlone) {
  std::vector<int64_t> values = {0, 1, 2, 3, 4, 5, 6, 7};
  std::vector<uint32_t> picks = {0, 2, 4};
  std::vector<uint32_t> narrower = {0, 2};

  RowBatch original;
  original.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data(),
                                           nullptr, /*start=*/2));
  original.SetRowCount(5);
  original.mutable_selection().Keep(picks);
  ASSERT_THAT(test::ReadColumn<int64_t>(original, 0), ElementsAre(2, 4, 6));

  RowBatch copy;
  copy.CopyFrom(original);
  copy.mutable_selection().Keep(narrower);

  EXPECT_THAT(test::ReadColumn<int64_t>(copy, 0), ElementsAre(2, 6));
  EXPECT_THAT(test::ReadColumn<int64_t>(original, 0), ElementsAre(2, 4, 6));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
