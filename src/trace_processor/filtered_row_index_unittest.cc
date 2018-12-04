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

TEST(FilteredRowIndexUnittest, Noop) {
  FilteredRowIndex index(1, 13);
  ASSERT_TRUE(index.all_set());
  ASSERT_EQ(index.start_row(), 1);
  ASSERT_EQ(index.end_row(), 13);
}

TEST(FilteredRowIndexUnittest, FilterAllRows) {
  FilteredRowIndex index(1, 5);
  bool in_bound_indices = true;
  index.FilterRows([&in_bound_indices](uint32_t row) {
    in_bound_indices = in_bound_indices && row >= 1 && row < 5;
    return row == 2 || row == 3;
  });
  ASSERT_TRUE(in_bound_indices);
  ASSERT_FALSE(index.all_set());

  auto f = index.TakeBitvector();
  ASSERT_EQ(f.size(), 4);
  ASSERT_FALSE(f[0]);
  ASSERT_TRUE(f[1]);
  ASSERT_TRUE(f[2]);
  ASSERT_FALSE(f[3]);
}

TEST(FilteredRowIndexUnittest, FilterBitvectorTwice) {
  FilteredRowIndex index(1, 5);
  index.FilterRows([](uint32_t row) { return row == 2 || row == 3; });
  bool in_bound_indices = true;
  index.FilterRows([&in_bound_indices](uint32_t row) {
    in_bound_indices = in_bound_indices && (row == 2 || row == 3);
    return row == 2;
  });
  ASSERT_TRUE(in_bound_indices);
  ASSERT_FALSE(index.all_set());

  auto f = index.TakeBitvector();
  ASSERT_EQ(f.size(), 4);
  ASSERT_FALSE(f[0]);
  ASSERT_TRUE(f[1]);
  ASSERT_FALSE(f[2]);
  ASSERT_FALSE(f[3]);
}

TEST(FilteredRowUnittest, SetAllRows) {
  FilteredRowIndex index(1, 5);
  index.IntersectRows({2, 3});

  ASSERT_FALSE(index.all_set());

  auto f = index.TakeBitvector();
  ASSERT_EQ(f.size(), 4);
  ASSERT_FALSE(f[0]);
  ASSERT_TRUE(f[1]);
  ASSERT_TRUE(f[2]);
  ASSERT_FALSE(f[3]);
}

TEST(FilteredRowUnittest, SetBitvectorRows) {
  FilteredRowIndex index(1, 5);
  index.FilterRows([](uint32_t row) { return row == 2 || row == 3; });
  index.IntersectRows({0, 2, 4, 5, 10});

  ASSERT_FALSE(index.all_set());

  auto f = index.TakeBitvector();
  ASSERT_EQ(f.size(), 4);
  ASSERT_FALSE(f[0]);
  ASSERT_TRUE(f[1]);
  ASSERT_FALSE(f[2]);
  ASSERT_FALSE(f[3]);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
