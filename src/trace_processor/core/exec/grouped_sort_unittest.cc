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

#include "src/trace_processor/core/exec/grouped_sort.h"

#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

struct Output {
  std::vector<uint32_t> ids;
  std::vector<uint32_t> groups;
  base::Status status;
};

Output Drain(const Source& source) {
  std::unique_ptr<OperatorState> state = source.MakeState(test::TestContext());
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

// A GroupedSort into groups by `column`, sorted by `keys` within them.
std::vector<Pipeline::Step> GroupOn(uint32_t column,
                                    std::vector<SortKey> keys = {}) {
  std::vector<Pipeline::Step> ops;
  ops.push_back(std::make_unique<GroupedSort>(
      GroupedSortSpec{{column}, std::move(keys)}));
  return ops;
}

// Groups come in the order first seen, each with its rows in order or, given
// keys, sorted by them.
TEST(GroupedSortTest, GroupsInOrderOfFirstRow) {
  test::ArraySource source({7, 3, 7, 5, 3, 7});
  Pipeline grouped(source, GroupOn(1), {});
  Output out = Drain(grouped);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.ids, ElementsAre(0, 2, 5, 1, 4, 3));
  EXPECT_THAT(out.groups, ElementsAre(0, 0, 0, 1, 1, 2));

  Pipeline sorted(source, GroupOn(1, {{0, /*descending=*/true}}), {});
  out = Drain(sorted);
  ASSERT_TRUE(out.status.ok()) << out.status.message();
  EXPECT_THAT(out.ids, ElementsAre(5, 2, 0, 4, 1, 3));
  EXPECT_THAT(out.groups, ElementsAre(0, 0, 0, 1, 1, 2));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
