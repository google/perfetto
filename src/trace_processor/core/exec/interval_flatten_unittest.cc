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

#include "src/trace_processor/core/exec/interval_flatten.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

TEST(IntervalFlattenTest, StreamsAcrossInputAndOutputBoundaries) {
  // Each group has more distinct ends than fit in one output batch. All its
  // intervals start together, with points before and after the positive rows,
  // so equal timestamps cross input batches too.
  constexpr uint32_t kIntervals = kMaxBatchRows + 2;
  struct Input {
    int64_t ts;
    int64_t dur;
    int64_t weight;
    bool present;
    uint32_t group;
  };
  using Output = std::tuple<int64_t, int64_t, std::optional<int64_t>, int64_t,
                            std::optional<int64_t>, uint32_t>;
  std::vector<Input> input;
  std::vector<Output> expected;
  for (uint32_t group = 0; group < 2; ++group) {
    std::optional<int64_t> key =
        group ? std::nullopt : std::optional<int64_t>(10);
    input.push_back({0, 0, 2, true, group});
    for (uint32_t i = 0; i < kIntervals; ++i) {
      input.push_back({0, static_cast<int64_t>(i) + 1, 1, i % 2 == 0, group});
    }
    input.push_back({0, 0, 3, true, group});
    expected.emplace_back(0, 0, key, kIntervals + 2, kIntervals / 2 + 5, group);
    for (uint32_t ts = 0; ts < kIntervals; ++ts) {
      int64_t sum = kIntervals / 2 - (ts + 1) / 2;
      expected.emplace_back(ts, 1, key, kIntervals - ts,
                            sum ? std::optional<int64_t>(sum) : std::nullopt,
                            group);
    }
    if (group == 0) {
      // A later row drains the first heap from Execute; the final group is
      // drained by Finish. Both paths must be able to yield and resume.
      input.push_back({kIntervals + 10, 1, 4, true, group});
      expected.emplace_back(kIntervals + 10, 1, key, 1, 4, group);
    }
  }

  IntervalFlattenSpec spec;
  spec.ts_column = 0;
  spec.dur_column = 1;
  spec.key_columns = {2};
  spec.group_column = 4;
  spec.aggregates = {{IntervalFlattenSpec::Function::kCount, 0},
                     {IntervalFlattenSpec::Function::kSum, 3}};
  IntervalFlatten op(spec);
  auto state = op.MakeState(test::TestContext());
  // Refill the same vectors for each input batch, published in a buffer each,
  // as a source refilling its storage would. Retained group keys must survive
  // this, and Reset must discard a previous execution's state.
  for (uint32_t chunk : {1u, 17u, kMaxBatchRows}) {
    SCOPED_TRACE(chunk);
    state->Reset();
    std::vector<int64_t> ts(chunk), dur(chunk), key(chunk), weight(chunk);
    std::vector<uint32_t> groups(chunk);
    auto key_valid = BitVector::CreateWithSize(chunk);
    auto weight_valid = BitVector::CreateWithSize(chunk);
    RowBatch in, out;
    std::vector<Output> actual;
    auto collect = [&] {
      ASSERT_LE(out.size(), kMaxBatchRows);
      auto keys = test::ReadNullableColumn<int64_t>(out, 2);
      auto sums = test::ReadNullableColumn<int64_t>(out, 4);
      for (uint32_t row = 0; row < out.size(); ++row) {
        actual.emplace_back(
            out.Value<int64_t>(0, row), out.Value<int64_t>(1, row), keys[row],
            out.Value<int64_t>(3, row), sums[row], out.Value<uint32_t>(5, row));
      }
    };
    for (uint32_t at = 0; at < input.size(); at += chunk) {
      auto count =
          static_cast<uint32_t>(std::min<size_t>(chunk, input.size() - at));
      for (uint32_t row = 0; row < count; ++row) {
        const Input& value = input[at + row];
        ts[row] = value.ts;
        dur[row] = value.dur;
        key[row] = 10 + value.group;
        weight[row] = value.weight;
        groups[row] = 7 + value.group * 13;
        key_valid.change(row, value.group == 0);
        weight_valid.change(row, value.present);
      }
      in.Reset();
      Context& context = test::TestContext();
      test::AddCopy(context, ts, nullptr, &in);
      test::AddCopy(context, dur, nullptr, &in);
      test::AddCopy(context, key, &key_valid, &in);
      test::AddCopy(context, weight, &weight_valid, &in);
      test::AddCopy(context, groups, nullptr, &in);
      in.SetRowCount(count);
      OpResult result;
      uint32_t calls = 0;
      do {
        ASSERT_LT(calls++, expected.size());
        result = op.Execute(in, out, *state);
        ASSERT_NE(result, OpResult::kError) << op.status(*state).message();
        if (out.size())
          collect();
      } while (result == OpResult::kHaveMoreOutput);
    }
    ASSERT_FALSE(actual.empty());
    OpResult result;
    uint32_t calls = 0;
    do {
      ASSERT_LT(calls++, expected.size());
      result = op.Finish(out, *state);
      ASSERT_NE(result, OpResult::kError) << op.status(*state).message();
      if (out.size())
        collect();
    } while (result == OpResult::kHaveMoreOutput);
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(op.Finish(out, *state), OpResult::kNeedMoreInput);
    EXPECT_EQ(out.size(), 0u);
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
