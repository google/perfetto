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

#include "src/trace_processor/core/tree/tree_column_ops.h"

#include <cstdint>

#include "perfetto/ext/base/murmur_hash.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::tree_ops {
namespace {

TEST(TreeColumnOpsTest, HashDistinguishesNull) {
  Tree::Column column = Tree::Column::Create<int64_t>(2);
  column.unchecked_span<int64_t>()[0] = 0;
  column.unchecked_span<int64_t>()[1] = 0;
  column.null_bv = BitVector::CreateWithSize(2, true);
  column.null_bv.clear(0);

  base::MurmurHashCombiner hashes[2];
  UpdateRowHashes(column, MakeMutableSpan(hashes));
  EXPECT_NE(hashes[0].digest(), hashes[1].digest());
}

TEST(TreeColumnOpsTest, GathersNullableRows) {
  Tree::Column column = Tree::Column::Create<int64_t>(3);
  column.unchecked_span<int64_t>()[0] = 10;
  column.unchecked_span<int64_t>()[1] = 20;
  column.unchecked_span<int64_t>()[2] = 30;
  column.null_bv = BitVector::CreateWithSize(3, true);
  column.null_bv.clear(1);
  const uint32_t rows[] = {2, 1};

  Tree::Column output = Gather(column, MakeSpan(rows));
  EXPECT_EQ(output.unchecked_span<int64_t>()[0], 30);
  EXPECT_TRUE(output.null_bv.is_set(0));
  EXPECT_FALSE(output.null_bv.is_set(1));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::tree_ops
