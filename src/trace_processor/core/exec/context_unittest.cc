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

#include "src/trace_processor/core/exec/context.h"

#include <cstdint>

#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

struct Counting : OperatorState {
  uint32_t calls = 0;
  RunVector<uint32_t> values;
};

Counting* MakeCounting(Context& context, uint32_t calls) {
  auto* state = context.Make<Counting>();
  state->calls = calls;
  context.Made(state);
  return state;
}

TEST(ContextTest, EndingARunPutsTheStateBackAsMade) {
  Context context;
  Counting* s = MakeCounting(context, 3);
  s->calls = 7;
  s->values.push_back(*s, 1);
  context.EndRun(*s);
  EXPECT_EQ(s->calls, 3u);
  EXPECT_TRUE(s->values.empty());
}

TEST(ContextTest, SmallRunMemoryIsKeptForTheNextRun) {
  Context context;
  Counting* s = MakeCounting(context, 0);
  s->values.push_back_multiple(*s, 0, 100);
  const uint32_t* block = s->values.data();
  context.EndRun(*s);
  s->values.push_back(*s, 1);
  EXPECT_EQ(s->values.data(), block);
}

// A RunVector keeps its memory in place across runs, as a FlexVector would.
TEST(ContextTest, SmallRunMemoryStaysWhereItIs) {
  Context context;
  Counting* s = MakeCounting(context, 0);
  s->values.push_back_multiple(*s, 0, 100);
  uint32_t capacity = s->values.capacity();
  context.EndRun(*s);
  EXPECT_TRUE(s->values.empty());
  EXPECT_EQ(s->values.capacity(), capacity);
}

TEST(ContextTest, LargeRunMemoryIsGivenBack) {
  Context context;
  Counting* s = MakeCounting(context, 0);
  s->values.push_back_multiple(*s, 0, kRunKeptBytes);
  context.EndRun(*s);
  s->values.push_back(*s, 1);
  EXPECT_LT(uint64_t{s->values.capacity()} * sizeof(uint32_t), kRunKeptBytes);
}

TEST(ContextTest, BatchesTakenForARunAreEmptiedAndTakenAgain) {
  Context context;
  Counting* s = MakeCounting(context, 0);
  RowBatch* batch = context.TakeBatch(*s);
  batch->AddBorrowedColumn(ColumnView::Sequence(nullptr, 0));
  batch->SetRowCount(1);
  context.EndRun(*s);
  EXPECT_EQ(batch->column_count(), 0u);
  EXPECT_EQ(context.TakeBatch(*s), batch);
}

TEST(ContextTest, TheFirstFailureIsKept) {
  Context context;
  context.Fail(base::ErrStatus("first"));
  context.Fail(base::ErrStatus("second"));
  EXPECT_EQ(context.error().message(), "first");
  context.ClearError();
  EXPECT_FALSE(context.failed());
}

TEST(ContextTest, SwappedRunVectorGrowthDoesNotLeaveStaleSavedHeader) {
  struct TwoVectors : OperatorState {
    RunVector<uint32_t> a;
    RunVector<uint32_t> b;
  };
  Context context;
  auto* s = context.Make<TwoVectors>();
  context.Made(s);

  // Run 1: both vectors allocate and are recorded in the saved state.
  s->a.push_back_multiple(*s, 1, 64);
  s->b.push_back_multiple(*s, 2, 64);
  context.EndRun(*s);

  // Run 2: swap them and grow b (which now holds a's old block).
  std::swap(s->a, s->b);
  s->b.push_back_multiple(*s, 3, 512);
  context.EndRun(*s);

  // Run 3: a must not point at the freed block from run 1.
  s->a.push_back(*s, 42);
  EXPECT_EQ(s->a.size(), 1u);
  EXPECT_EQ(s->a[0], 42u);
  s->b.push_back(*s, 99);
  EXPECT_EQ(s->b.size(), 1u);
  EXPECT_EQ(s->b[0], 99u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
