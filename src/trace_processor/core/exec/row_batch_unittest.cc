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
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

// A column composed inside one batch carries the block its indices were
// materialized into. A batch adopting that column must not keep writing into
// the block, or narrowing the adopter corrupts the batch it came from.
TEST(RowBatchTest, AdoptingAComposedColumnLeavesTheOriginalAlone) {
  std::vector<int64_t> values = {0, 1, 2, 3, 4, 5, 6, 7};
  std::vector<uint32_t> picks = {0, 2, 4};
  std::vector<uint32_t> narrower = {0, 2};

  // A range not starting at zero followed by an indexed slice forces the
  // indices into a block owned by `original`.
  RowBatch original;
  original.AddColumn(
      ColumnView::Reference(StorageType{Int64{}}, values.data()));
  original.Compose(RowSelection::Range(2), 5);
  original.SetCardinality(5);
  ASSERT_TRUE(original.Slice(RowSelection::Indices(Span<const uint32_t>(
                                 picks.data(), picks.data() + 3)),
                             3));
  ASSERT_THAT(test::ReadColumn<int64_t>(original, 0), ElementsAre(2, 4, 6));

  RowBatch adopter;
  adopter.AddColumn(original.column(0));
  adopter.SetCardinality(3);
  ASSERT_TRUE(adopter.Slice(RowSelection::Indices(Span<const uint32_t>(
                                narrower.data(), narrower.data() + 2)),
                            2));

  EXPECT_THAT(test::ReadColumn<int64_t>(adopter, 0), ElementsAre(2, 6));
  EXPECT_THAT(test::ReadColumn<int64_t>(original, 0), ElementsAre(2, 4, 6));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
