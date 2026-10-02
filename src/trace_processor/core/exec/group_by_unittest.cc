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

#include "src/trace_processor/core/exec/group_by.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

class StringSource final : public Source {
 public:
  StringSource(std::vector<StringPool::Id> keys, uint32_t chunk)
      : keys_(std::move(keys)), chunk_(chunk) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().emitted = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto rows = static_cast<uint32_t>(keys_.size());
    if (s.emitted == rows) {
      return false;
    }
    uint32_t count = std::min(chunk_, rows - s.emitted);
    out.Reset();
    out.AddColumn(ColumnView::Reference(StorageType{Id{}}, nullptr));
    out.AddColumn(ColumnView::Reference(StorageType{String{}}, keys_.data()));
    out.Compose(RowSelection::Range(s.emitted), count);
    out.SetCardinality(count);
    s.emitted += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t emitted = 0;
  };

  std::vector<StringPool::Id> keys_;
  uint32_t chunk_;
};

struct Output {
  std::vector<uint32_t> ids;
  std::vector<uint32_t> groups;
  base::Status status;
};

Output Drain(const Source& source) {
  std::unique_ptr<OperatorState> state = source.MakeState();
  RowBatch batch;
  Output out;
  while (source.GetData(batch, *state)) {
    for (uint32_t id : test::ReadColumn<uint32_t>(batch, 0)) {
      out.ids.push_back(id);
    }
    for (uint32_t group : test::ReadColumn<uint32_t>(batch, 2)) {
      out.groups.push_back(group);
    }
  }
  out.status = source.status(*state);
  return out;
}

std::vector<std::unique_ptr<Operator>> GroupOn(uint32_t column) {
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<GroupBy>(std::vector<uint32_t>{column}));
  return ops;
}

TEST(GroupByTest, GroupsInOrderOfFirstRowKeepingTheirRowsInOrder) {
  test::ArraySource source({7, 3, 7, 5, 3, 7});
  Pipeline grouped(source, GroupOn(1), {});
  Output out = Drain(grouped);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.ids, ElementsAre(0, 2, 5, 1, 4, 3));
  EXPECT_THAT(out.groups, ElementsAre(0, 0, 0, 1, 1, 2));
}

TEST(GroupByTest, KeepsOrderWithinGroupsAcrossManyBatches) {
  std::vector<int64_t> values(3 * kMaxBatchRows + 17);
  std::mt19937 rng(3);
  for (int64_t& value : values) {
    value = static_cast<int64_t>(rng() % 97);
  }
  test::ArraySource source(values);
  Pipeline grouped(source, GroupOn(1), {});
  Output out = Drain(grouped);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  ASSERT_EQ(out.ids.size(), values.size());
  for (uint32_t i = 1; i < out.ids.size(); ++i) {
    if (out.groups[i] == out.groups[i - 1]) {
      EXPECT_EQ(values[out.ids[i]], values[out.ids[i - 1]]);
      EXPECT_LT(out.ids[i - 1], out.ids[i]);
    } else {
      EXPECT_EQ(out.groups[i], out.groups[i - 1] + 1);
    }
  }
}

TEST(GroupByTest, GroupsStrings) {
  StringPool pool;
  StringPool::Id a = pool.InternString("a");
  StringPool::Id b = pool.InternString("b");
  StringSource source({b, StringPool::Id::Null(), a, b, StringPool::Id::Null()},
                      2);
  Pipeline grouped(source, GroupOn(1), {});
  Output out = Drain(grouped);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.ids, ElementsAre(0, 3, 1, 4, 2));
  EXPECT_THAT(out.groups, ElementsAre(0, 0, 1, 1, 2));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
