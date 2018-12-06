/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/filtered_row_index.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

using ::testing::ElementsAre;

TEST(FilteredRowIndexUnittest, Noop) {
  FilteredRowIndex index(1, 4);
  ASSERT_THAT(index.ToRowVector(), ElementsAre(1, 2, 3));
}

TEST(FilteredRowIndexUnittest, FilterRows) {
  FilteredRowIndex index(1, 5);
  bool in_bound_indices = true;
  index.FilterRows([&in_bound_indices](uint32_t row) {
    in_bound_indices = in_bound_indices && row >= 1 && row < 5;
    return row == 2 || row == 3;
  });
  ASSERT_THAT(index.ToRowVector(), ElementsAre(2, 3));
}

TEST(FilteredRowIndexUnittest, FilterRowsTwice) {
  FilteredRowIndex index(1, 5);
  index.FilterRows([](uint32_t row) { return row == 2 || row == 3; });
  bool in_bound_indices = true;
  index.FilterRows([&in_bound_indices](uint32_t row) {
    in_bound_indices = in_bound_indices && (row == 2 || row == 3);
    return row == 2;
  });
  ASSERT_TRUE(in_bound_indices);
  ASSERT_THAT(index.ToRowVector(), ElementsAre(2));
}

TEST(FilteredRowIndexUnittest, FilterThenIntersect) {
  FilteredRowIndex index(1, 5);
  index.FilterRows([](uint32_t row) { return row == 2 || row == 3; });
  index.IntersectRows({0, 2, 4, 5, 10});
  ASSERT_THAT(index.ToRowVector(), ElementsAre(2));
}

TEST(FilteredRowIndexUnittest, IntersectThenFilter) {
  FilteredRowIndex index(1, 5);
  index.IntersectRows({0, 2, 4, 5, 10});
  index.FilterRows([](uint32_t row) { return row == 2 || row == 3; });
  ASSERT_THAT(index.ToRowVector(), ElementsAre(2));
}

TEST(FilteredRowIndexUnittest, Intersect) {
  FilteredRowIndex index(1, 5);
  index.IntersectRows({0, 2, 4, 5, 10});
  ASSERT_THAT(index.ToRowVector(), ElementsAre(2, 4));
}

TEST(FilteredRowIndexUnittest, IntersectTwice) {
  FilteredRowIndex index(1, 5);
  index.IntersectRows({0, 2, 4, 5, 10});
  index.IntersectRows({4});
  ASSERT_THAT(index.ToRowVector(), ElementsAre(4));
}

TEST(FilteredRowIndexUnittest, ToIterator) {
  FilteredRowIndex index(1, 5);
  index.IntersectRows({0, 2, 4, 5, 10});
  auto iterator = index.ToRowIterator(false);

  ASSERT_THAT(iterator->Row(), 2);
  iterator->NextRow();
  ASSERT_THAT(iterator->Row(), 4);
  iterator->NextRow();
  ASSERT_TRUE(iterator->IsEnd());
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
