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
  for (uint32_t size : {0u, 1u, 2u, 17u, 1000u, 100000u}) {
    std::vector<std::pair<uint32_t, std::string>> data;
    for (uint32_t i = 0; i < size; ++i) {
      data.emplace_back(rnd() % 50, std::to_string(i));
    }
    auto by_first = [](const auto& a, const auto& b) {
      return a.first < b.first;
    };
    auto expected = data;
    std::stable_sort(expected.begin(), expected.end(), by_first);
    ColdStableSort(data.begin(), data.end(), by_first);
    ASSERT_EQ(data, expected);
  }
}

TEST(ColdSortTest, DefaultLess) {
  std::vector<std::string> data = {"c", "a", "b", "a"};
  ColdSort(data.begin(), data.end());
  ASSERT_EQ(data, (std::vector<std::string>{"a", "a", "b", "c"}));
}

TEST(ColdSortTest, MoveOnly) {
  std::vector<std::unique_ptr<int>> data;
  for (int v : {3, 1, 2}) {
    data.push_back(std::make_unique<int>(v));
  }
  ColdSort(data.begin(), data.end(),
           [](const auto& a, const auto& b) { return *a < *b; });
  ASSERT_EQ(*data[0], 1);
  ASSERT_EQ(*data[1], 2);
  ASSERT_EQ(*data[2], 3);
}

}  // namespace
}  // namespace perfetto::trace_processor
