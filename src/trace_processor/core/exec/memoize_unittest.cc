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

#include "src/trace_processor/core/exec/memoize.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using test::ArraySource;
using test::FailingSource;
using test::Sequence;

// Counts how many reads of `source` were started.
class CountingSource final : public Source {
 public:
  explicit CountingSource(const Source& source) : source_(source) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return source_.MakeState();
  }
  void Rewind(OperatorState& state) const override {
    ++reads;
    source_.Rewind(state);
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    return source_.GetData(out, state);
  }
  base::Status status(const OperatorState& state) const override {
    return source_.status(state);
  }

  mutable uint32_t reads = 0;

 private:
  const Source& source_;
};

// The values of column 1 from `cursor`, from its first row.
std::vector<int64_t> ReadAll(RowCursor& cursor) {
  std::vector<int64_t> values;
  for (bool row = cursor.Open(); row; row = cursor.Next()) {
    values.push_back(cursor.Value<int64_t>(1));
  }
  return values;
}

TEST(MemoizeTest, ReadsAfterTheFirstFullReadAreReplayed) {
  ArraySource array(Sequence(5000));
  CountingSource source(array);
  Memoize memoize(source);
  RowCursor cursor(memoize);
  EXPECT_EQ(ReadAll(cursor), Sequence(5000));
  EXPECT_EQ(ReadAll(cursor), Sequence(5000));
  EXPECT_EQ(ReadAll(cursor), Sequence(5000));
  EXPECT_TRUE(cursor.status().ok());
  EXPECT_EQ(source.reads, 1u);
}

TEST(MemoizeTest, APartReadKeepsNothing) {
  ArraySource array(Sequence(5000));
  CountingSource source(array);
  Memoize memoize(source);
  RowCursor cursor(memoize);
  ASSERT_TRUE(cursor.Open());
  EXPECT_EQ(ReadAll(cursor), Sequence(5000));
  EXPECT_EQ(ReadAll(cursor), Sequence(5000));
  EXPECT_EQ(source.reads, 2u);
}

TEST(MemoizeTest, AFailedReadKeepsNothing) {
  FailingSource failing;
  CountingSource source(failing);
  Memoize memoize(source);
  RowCursor cursor(memoize);
  ReadAll(cursor);
  EXPECT_FALSE(cursor.status().ok());
  ReadAll(cursor);
  EXPECT_FALSE(cursor.status().ok());
  EXPECT_EQ(source.reads, 2u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
