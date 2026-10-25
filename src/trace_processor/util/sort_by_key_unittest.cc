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

#include "src/trace_processor/util/sort.h"

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

TEST(SortByKeyTest, MatchesStdStableSort) {
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
    StableSortByKey(data.begin(), data.end(),
                    [](const auto& x) { return x.first; });
    ASSERT_EQ(data, expected);
  }
}

// One range or two runs, of few or many elements with narrow or wide keys, so
// both comparing and radix sorting, against std::stable_sort of the whole.
TEST(SortByKeyTest, StableSortElementsByKeyMatchesStdStableSort) {
  struct Element {
    int64_t key;
    uint32_t index;
    bool operator==(const Element& o) const {
      return key == o.key && index == o.index;
    }
  };
  std::mt19937_64 rnd(0);
  for (uint32_t size : {0u, 1u, 10u, 3000u, 100000u}) {
    for (bool wide : {false, true}) {
      std::vector<Element> data;
      for (uint32_t i = 0; i < size; ++i) {
        int64_t key = wide ? static_cast<int64_t>(rnd())
                           : static_cast<int64_t>(rnd() % 1000) - 500;
        data.push_back({key, i});
      }
      auto expected = data;
      std::stable_sort(
          expected.begin(), expected.end(),
          [](const Element& a, const Element& b) { return a.key < b.key; });
      for (uint32_t split : {size, size / 3}) {
        // The first `split` elements are the second run, as a wrapped ring
        // stores them.
        std::vector<Element> storage(data.begin() + split, data.end());
        storage.insert(storage.end(), data.begin(), data.begin() + split);
        Element* second = storage.data() + (size - split);
        StableSortElementsByKey(second, second + split, storage.data(), second,
                                [](const Element& e) { return e.key; });
        std::vector<Element> sorted(second, second + split);
        sorted.insert(sorted.end(), storage.data(), second);
        ASSERT_EQ(sorted, expected) << size << " " << wide << " " << split;
      }
    }
  }
}

// Wide and narrow keys, shuffled, sorted or nearly sorted, on either side of
// where radix sorting becomes cheaper.
TEST(SortByKeyTest, LargeInputsMatchStdStableSort) {
  std::mt19937_64 rnd(0);
  for (uint32_t size : {3000u, 5000u, 100000u}) {
    for (uint64_t range : {uint64_t{0}, uint64_t{1} << 20, uint64_t{1000}}) {
      std::vector<std::pair<int64_t, uint32_t>> data;
      for (uint32_t i = 0; i < size; ++i) {
        // Narrow ranges sit at timestamp-like values; the full range does not,
        // so it cannot overflow.
        int64_t key =
            range ? 350000000000000 + static_cast<int64_t>(rnd() % range)
                  : static_cast<int64_t>(rnd());
        data.emplace_back(key, i);
      }
      for (int order = 0; order < 3; ++order) {
        auto input = data;
        if (order > 0) {
          std::stable_sort(input.begin(), input.end());
        }
        if (order == 2) {
          for (uint32_t i = 0; i + 1 < size; i += 64) {
            std::swap(input[i].first, input[i + 1].first);
          }
        }
        auto expected = input;
        std::stable_sort(
            expected.begin(), expected.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        StableSortByKey(input.begin(), input.end(),
                        [](const auto& x) { return x.first; });
        ASSERT_EQ(input, expected) << size << " " << range << " " << order;
      }
    }
  }
}

TEST(SortByKeyTest, KeyExtremes) {
  std::vector<int64_t> data = {0, -1, std::numeric_limits<int64_t>::max(),
                               std::numeric_limits<int64_t>::min(), 1};
  SortByKey(data.begin(), data.end(), [](int64_t x) { return x; });
  ASSERT_EQ(data,
            (std::vector<int64_t>{std::numeric_limits<int64_t>::min(), -1, 0, 1,
                                  std::numeric_limits<int64_t>::max()}));
}

TEST(SortByKeyTest, Descending) {
  std::vector<std::pair<int64_t, int>> data = {
      {2, 0},
      {-5, 1},
      {2, 2},
      {std::numeric_limits<int64_t>::min(), 3},
      {7, 4}};
  StableSortByKeyDescending(data.begin(), data.end(),
                            [](const auto& x) { return x.first; });
  ASSERT_EQ(data, (std::vector<std::pair<int64_t, int>>{
                      {7, 4},
                      {2, 0},
                      {2, 2},
                      {-5, 1},
                      {std::numeric_limits<int64_t>::min(), 3}}));
}

TEST(SortByKeyTest, MoveOnly) {
  std::vector<std::unique_ptr<int>> data;
  for (int v : {3, 1, 2}) {
    data.push_back(std::make_unique<int>(v));
  }
  SortByKey(data.begin(), data.end(),
            [](const std::unique_ptr<int>& p) { return *p; });
  ASSERT_EQ(*data[0], 1);
  ASSERT_EQ(*data[1], 2);
  ASSERT_EQ(*data[2], 3);
}

}  // namespace
}  // namespace perfetto::trace_processor
