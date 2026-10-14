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

#include "src/trace_processor/util/cold_sort.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(ColdSortTest, MatchesStdStableSort) {
  std::minstd_rand0 rnd(0);
  for (uint32_t size : {0u, 1u, 2u, 3u, 16u, 17u, 64u, 65u, 1000u, 100000u}) {
    std::vector<std::pair<int32_t, std::string>> data;
    for (uint32_t i = 0; i < size; ++i) {
      data.emplace_back(static_cast<int32_t>(rnd() % 50) - 25,
                        std::to_string(i));
    }
    auto expected = data;
    std::stable_sort(
        expected.begin(), expected.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    ColdStableSortByKey(data.begin(), data.end(),
                        [](const auto& x) { return x.first; });
    ASSERT_EQ(data, expected);
  }
}

TEST(ColdSortTest, KeyExtremes) {
  std::vector<int64_t> data = {0, -1, std::numeric_limits<int64_t>::max(),
                               std::numeric_limits<int64_t>::min(), 1};
  ColdSortByKey(data.begin(), data.end(), [](int64_t x) { return x; });
  ASSERT_EQ(data,
            (std::vector<int64_t>{std::numeric_limits<int64_t>::min(), -1, 0, 1,
                                  std::numeric_limits<int64_t>::max()}));
}

TEST(ColdSortTest, Descending) {
  std::vector<std::pair<int64_t, int>> data = {
      {2, 0},
      {-5, 1},
      {2, 2},
      {std::numeric_limits<int64_t>::min(), 3},
      {7, 4}};
  ColdStableSortByKeyDescending(data.begin(), data.end(),
                                [](const auto& x) { return x.first; });
  ASSERT_EQ(data, (std::vector<std::pair<int64_t, int>>{
                      {7, 4},
                      {2, 0},
                      {2, 2},
                      {-5, 1},
                      {std::numeric_limits<int64_t>::min(), 3}}));
}

TEST(ColdSortTest, MoveOnly) {
  std::vector<std::unique_ptr<int>> data;
  for (int v : {3, 1, 2}) {
    data.push_back(std::make_unique<int>(v));
  }
  ColdSortByKey(data.begin(), data.end(),
                [](const std::unique_ptr<int>& p) { return *p; });
  ASSERT_EQ(*data[0], 1);
  ASSERT_EQ(*data[1], 2);
  ASSERT_EQ(*data[2], 3);
}

}  // namespace
}  // namespace perfetto::trace_processor
