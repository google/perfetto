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

#include "src/trace_processor/core/exec/number_groups.h"

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;

// A new group starts wherever the keys change, with null as its own key.
// Groups continue across batches.
TEST(NumberGroupsTest, NumbersGroupsAcrossBatches) {
  NumberGroups numbers({0});
  auto state = numbers.MakeState(test::TestContext());

  std::vector<int64_t> first = {1, 1, 2, 0};
  BitVector first_valid = BitVector::CreateWithSize(4, true);
  first_valid.change(3, false);
  RowBatch batch;
  batch.AddBorrowedColumn(
      ColumnView::Reference(StorageType{Int64{}}, first.data(), &first_valid));
  batch.SetRowCount(4);
  ASSERT_TRUE(numbers.Process(batch, *state));
  EXPECT_THAT(test::ReadColumn<uint32_t>(batch, 1), ElementsAre(0, 0, 1, 2));

  std::vector<int64_t> second = {0, 3};
  BitVector second_valid = BitVector::CreateWithSize(2, true);
  second_valid.change(0, false);
  batch.Reset();
  batch.AddBorrowedColumn(ColumnView::Reference(StorageType{Int64{}},
                                                second.data(), &second_valid));
  batch.SetRowCount(2);
  ASSERT_TRUE(numbers.Process(batch, *state));
  EXPECT_THAT(test::ReadColumn<uint32_t>(batch, 1), ElementsAre(2, 3));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
