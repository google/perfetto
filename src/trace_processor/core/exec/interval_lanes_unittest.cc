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

#include "src/trace_processor/core/exec/interval_lanes.h"

#include <cstdint>
#include <limits>
#include <vector>

#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;
using testing::HasSubstr;
using Result = LaneBatch::Result;

// A batch of ts, dur, key and group number columns. A dur of `kNull` is null.
constexpr int64_t kNull = std::numeric_limits<int64_t>::min();
RowBatch Batch(const std::vector<int64_t>& ts,
               const std::vector<int64_t>& dur,
               const std::vector<int64_t>& keys,
               const std::vector<uint32_t>& groups) {
  BitVector valid = BitVector::CreateWithSize(dur.size(), true);
  for (uint32_t i = 0; i < dur.size(); ++i) {
    if (dur[i] == kNull) {
      valid.clear(i);
    }
  }
  RowBatch batch;
  Context& context = test::TestContext();
  test::AddCopy(context, ts, nullptr, &batch);
  test::AddCopy(context, dur, &valid, &batch);
  test::AddCopy(context, keys, nullptr, &batch);
  test::AddCopy(context, groups, nullptr, &batch);
  batch.SetRowCount(static_cast<uint32_t>(ts.size()));
  return batch;
}

IntervalLaneSpec Spec() {
  IntervalLaneSpec spec;
  spec.ts_column = 0;
  spec.dur_column = 1;
  spec.key_columns = {2};
  spec.group_column = 3;
  return spec;
}

TEST(LaneBatchTest, ReadsEachKindOfRow) {
  IntervalLaneSpec spec = Spec();
  RowBatch batch =
      Batch({10, 0, 5, 10, std::numeric_limits<int64_t>::max()},
            {5, 0, kNull, -1, 1}, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0});
  IntervalInputName name{"OP", "operand", 2};
  LaneBatch lanes(batch, spec);
  int64_t start;
  int64_t end;
  ASSERT_EQ(lanes.Read(0, &start, &end), Result::kOk);
  EXPECT_EQ(start, 10);
  EXPECT_EQ(end, 15);
  ASSERT_EQ(lanes.Read(1, &start, &end), Result::kOk);
  EXPECT_EQ(end, 0);
  EXPECT_EQ(lanes.Read(2, &start, &end), Result::kSkip);
  EXPECT_EQ(lanes.Read(3, &start, &end), Result::kNegative);
  EXPECT_THAT(lanes.Error(Result::kNegative, name).message(),
              HasSubstr("OP: operand 2 has a row whose ts or dur"));
  EXPECT_EQ(lanes.Read(4, &start, &end), Result::kOverflow);
  EXPECT_THAT(lanes.Error(Result::kOverflow, name).message(),
              HasSubstr("integer overflow"));

  // With `forever`, a dur of -1 never ends.
  spec.forever = true;
  LaneBatch forever(batch, spec);
  ASSERT_EQ(forever.Read(3, &start, &end), Result::kOk);
  EXPECT_EQ(end, std::numeric_limits<int64_t>::max());
}

std::vector<uint32_t> Lanes(LaneReader& reader,
                            const RowBatch& batch,
                            GroupTable* shared) {
  EXPECT_TRUE(reader.Start(batch, shared).ok());
  LaneBatch lanes = reader.batch();
  std::vector<uint32_t> out;
  for (uint32_t row = 0; row < batch.size(); ++row) {
    out.push_back(lanes.lane(row));
  }
  return out;
}

// Without a LaneReader, lanes are the input's group numbers. With one,
// inputs sharing a table agree on each key's number, also for a lane spanning
// two batches.
TEST(LaneReaderTest, NumbersLanes) {
  IntervalLaneSpec spec = Spec();
  RowBatch own = Batch({0, 0}, {1, 1}, {7, 3}, {0, 1});
  LaneBatch lanes(own, spec);
  EXPECT_EQ(lanes.lane(0), 0u);
  EXPECT_EQ(lanes.lane(1), 1u);

  GroupTable table;
  LaneReader a(spec, {"OP", "a"});
  LaneReader b(spec, {"OP", "b"});
  EXPECT_THAT(
      Lanes(a, Batch({0, 0, 0}, {1, 1, 1}, {7, 7, 3}, {0, 0, 1}), &table),
      ElementsAre(0, 0, 1));
  EXPECT_THAT(Lanes(a, Batch({0, 0}, {1, 1}, {3, 9}, {1, 2}), &table),
              ElementsAre(1, 2));
  EXPECT_THAT(
      Lanes(b, Batch({0, 0, 0}, {1, 1, 1}, {9, 4, 7}, {0, 1, 2}), &table),
      ElementsAre(2, 3, 0));
}

// A lane's keys stay readable after the next batch is held if carried.
TEST(LaneKeysTest, CarriesTheOpenLane) {
  Context& context = test::TestContext();
  std::vector<uint32_t> key_columns = {2};
  LaneKeys keys(key_columns);
  RowBatch first = Batch({0, 0}, {1, 1}, {7, 3}, {0, 1});
  ASSERT_TRUE(keys.Hold(first, context).ok());
  uint32_t open = keys.Of(1);
  keys.Carry(open);
  RowBatch second = Batch({0}, {1}, {9}, {2});
  ASSERT_TRUE(keys.Hold(second, context).ok());
  std::vector<uint32_t> handles = {LaneKeys::kCarried, keys.Of(0)};
  RowBatch out;
  keys.Gather(&out, handles, context);
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(3, 9));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
