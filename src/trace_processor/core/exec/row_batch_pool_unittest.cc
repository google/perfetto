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

#include "src/trace_processor/core/exec/row_batch_pool.h"

#include <utility>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

TEST(RowBatchPoolTest, AReleasedBatchIsHandedOutAgain) {
  RowBatchPool pool;
  {
    PooledRowBatch batch = pool.Acquire();
    ASSERT_TRUE(batch);
    EXPECT_EQ(pool.allocations(), 1u);
  }
  EXPECT_EQ(pool.free_count(), 1u);

  PooledRowBatch again = pool.Acquire();
  EXPECT_EQ(pool.allocations(), 1u);
  EXPECT_EQ(pool.free_count(), 0u);
}

// The point of the pool: an operator which holds several batches at once
// stops allocating once it has as many as it ever needs at one time.
TEST(RowBatchPoolTest, HoldingSeveralAtOnceAllocatesOnlyAsManyAsAreHeld) {
  RowBatchPool pool;
  {
    std::vector<PooledRowBatch> held;
    for (int i = 0; i < 4; ++i) {
      held.push_back(pool.Acquire());
    }
    EXPECT_EQ(pool.allocations(), 4u);
  }
  for (int round = 0; round < 10; ++round) {
    std::vector<PooledRowBatch> held;
    for (int i = 0; i < 4; ++i) {
      held.push_back(pool.Acquire());
    }
  }
  EXPECT_EQ(pool.allocations(), 4u);
}

// A batch handed back carries nothing over from its last use, or an operator
// would read the previous query's columns.
TEST(RowBatchPoolTest, ABatchComesBackEmpty) {
  RowBatchPool pool;
  {
    PooledRowBatch batch = pool.Acquire();
    batch->AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
    batch->SetCardinality(7);
    ASSERT_EQ(batch->column_count(), 1u);
  }
  PooledRowBatch again = pool.Acquire();
  EXPECT_EQ(again->column_count(), 0u);
  EXPECT_EQ(again->size(), 0u);
}

TEST(RowBatchPoolTest, MovingABatchMovesWhoReturnsIt) {
  RowBatchPool pool;
  {
    PooledRowBatch batch = pool.Acquire();
    PooledRowBatch moved = std::move(batch);
    EXPECT_FALSE(batch);
    EXPECT_TRUE(moved);
    EXPECT_EQ(pool.free_count(), 0u);
  }
  // Returned once, by whichever of them still held it.
  EXPECT_EQ(pool.free_count(), 1u);
  EXPECT_EQ(pool.allocations(), 1u);
}

TEST(RowBatchPoolTest, ABatchCanBeGivenBackEarly) {
  RowBatchPool pool;
  PooledRowBatch batch = pool.Acquire();
  batch.Release();
  EXPECT_FALSE(batch);
  EXPECT_EQ(pool.free_count(), 1u);
  batch.Release();
  EXPECT_EQ(pool.free_count(), 1u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
