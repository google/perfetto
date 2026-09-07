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

#include "src/trace_processor/core/exec/breaker.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using test::ArraySource;
using test::FailingSource;
using test::Sequence;
using ::testing::ElementsAre;

// Buffers every value and serves them back to front, which is only possible
// once the whole input is in. A negative value is refused.
class Reverse final : public Breaker {
 public:
  explicit Reverse(const Source& input) : Breaker(input) {}

  bool Consume(const RowBatch& in, Breaker::State& state) const override {
    State& s = state.Cast<State>();
    for (uint32_t row = 0; row < in.size(); ++row) {
      int64_t value = in.column(1).Value<int64_t>(row);
      if (value < 0) {
        s.status = base::ErrStatus("negative value");
        return false;
      }
      s.values.push_back(value);
    }
    return true;
  }
  bool Finish(Breaker::State& state) const override {
    State& s = state.Cast<State>();
    std::reverse(s.values.begin(), s.values.end());
    return true;
  }

 private:
  struct State : Breaker::State {
    ~State() override;
    std::vector<int64_t> values;
    uint32_t served = 0;
  };

  std::unique_ptr<Breaker::State> CreateState() const override {
    return std::make_unique<State>();
  }
  bool Serve(RowBatch& out, Breaker::State& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(s.values.size());
    if (s.served == rows) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, rows - s.served);
    out.Reset();
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, s.values.data()));
    out.Compose(RowSelection::Range(s.served), count);
    out.SetCardinality(count);
    s.served += count;
    return true;
  }
  void Reset(Breaker::State& state) const override {
    State& s = state.Cast<State>();
    s.values.clear();
    s.served = 0;
  }
};

Reverse::State::~State() = default;

std::vector<int64_t> DrainValues(const Source& source) {
  std::vector<int64_t> values;
  RowCursor cursor(source);
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
    values.push_back(cursor.Value<int64_t>(0));
  }
  return values;
}

TEST(BreakerTest, ServesOnlyOnceTheWholeInputIsIn) {
  ArraySource source(Sequence(kMaxBatchRows * 2 + 7));
  Reverse reverse(source);

  std::vector<int64_t> values = DrainValues(reverse);
  ASSERT_EQ(values.size(), kMaxBatchRows * 2u + 7u);
  EXPECT_EQ(values.front(), kMaxBatchRows * 2 + 6);
  EXPECT_EQ(values.back(), 0);
}

TEST(BreakerTest, RewindReadsTheInputAgain) {
  ArraySource source({1, 2, 3});
  Reverse reverse(source);
  RowCursor cursor(reverse);
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
  }
  std::vector<int64_t> again;
  for (cursor.Open(); !cursor.eof(); cursor.Next()) {
    again.push_back(cursor.Value<int64_t>(0));
  }
  EXPECT_THAT(again, ElementsAre(3, 2, 1));
  EXPECT_TRUE(cursor.status().ok());
}

TEST(BreakerTest, AFailingInputIsReported) {
  FailingSource source;
  Reverse reverse(source);
  RowCursor cursor(reverse);
  EXPECT_FALSE(cursor.Open());
  EXPECT_EQ(cursor.status().message(), "input broke");
}

TEST(BreakerTest, AFailingConsumeIsReported) {
  ArraySource source({1, -2, 3});
  Reverse reverse(source);
  RowCursor cursor(reverse);
  EXPECT_FALSE(cursor.Open());
  EXPECT_EQ(cursor.status().message(), "negative value");
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
