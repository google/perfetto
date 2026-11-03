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

#include "src/trace_processor/core/exec/buffer_pool.h"

#include <cstdint>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

TEST(BufferPoolTest, StorageIsSizedForTheWidthTaken) {
  BufferPool pool;
  EXPECT_EQ(pool.Take(4).chunk().storage.size(), kMaxBatchRows * 4u);
  EXPECT_EQ(pool.Take(8).chunk().storage.size(), kMaxBatchRows * 8u);
  EXPECT_EQ(pool.Take(16).chunk().storage.size(), kMaxBatchRows * 16u);
}

// A buffer given back is the next of its width given out, and never one of
// another width.
TEST(BufferPoolTest, ReusesTheLastBufferGivenBackOfTheSameWidth) {
  BufferPool pool;
  const ColumnChunk* narrow;
  const ColumnChunk* wide;
  {
    ColumnBuffer a = pool.Take(4);
    ColumnBuffer b = pool.Take(8);
    narrow = &a.chunk();
    wide = &b.chunk();
  }
  uint64_t allocated = pool.allocated_bytes();
  EXPECT_EQ(allocated, kMaxBatchRows * (4u + 8u));

  ColumnBuffer c = pool.Take(8);
  EXPECT_EQ(&c.chunk(), wide);
  ColumnBuffer d = pool.Take(8);
  EXPECT_NE(&d.chunk(), narrow);
  ColumnBuffer e = pool.Take(4);
  EXPECT_EQ(&e.chunk(), narrow);
  EXPECT_EQ(pool.allocated_bytes(), allocated + kMaxBatchRows * 8u);
}

// A buffer comes back only once no copy of its handle is left.
TEST(BufferPoolTest, ABufferHeldByACopyIsNotGivenOut) {
  BufferPool pool;
  ColumnBuffer copy;
  const ColumnChunk* held;
  {
    ColumnBuffer original = pool.Take(8);
    held = &original.chunk();
    copy = original;
  }
  EXPECT_NE(&pool.Take(8).chunk(), held);
  copy = ColumnBuffer();
  EXPECT_EQ(&pool.Take(8).chunk(), held);
}

TEST(BufferPoolTest, ValueWidthMatchesTheValuesStored) {
  EXPECT_EQ(ValueWidth(StorageType{Id{}}), 4u);
  EXPECT_EQ(ValueWidth(StorageType{Uint32{}}), 4u);
  EXPECT_EQ(ValueWidth(StorageType{Int32{}}), 4u);
  EXPECT_EQ(ValueWidth(StorageType{String{}}), 4u);
  EXPECT_EQ(ValueWidth(StorageType{Int64{}}), 8u);
  EXPECT_EQ(ValueWidth(StorageType{Double{}}), 8u);
  EXPECT_EQ(ValueWidth(ColumnView::Variants(nullptr)), sizeof(Variant));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
