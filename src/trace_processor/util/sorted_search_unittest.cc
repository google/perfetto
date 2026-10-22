/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/util/sorted_search.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

std::vector<uint32_t> ExpectedLowerBounds(const std::vector<int64_t>& data,
                                          const std::vector<int64_t>& keys) {
  std::vector<uint32_t> expected;
  expected.reserve(keys.size());
  for (int64_t key : keys) {
    auto it = std::lower_bound(data.begin(), data.end(), key);
    expected.push_back(static_cast<uint32_t>(it - data.begin()));
  }
  return expected;
}

std::vector<int64_t> GenerateUniformKeys(int64_t start,
                                         int64_t step,
                                         size_t count) {
  std::vector<int64_t> keys;
  keys.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    keys.push_back(start + static_cast<int64_t>(i) * step);
  }
  return keys;
}

std::vector<int64_t> GenerateSortedRandomKeys(int64_t min_val,
                                              int64_t max_val,
                                              size_t count,
                                              uint32_t seed = 42) {
  std::vector<int64_t> keys;
  keys.reserve(count);
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int64_t> dist(min_val, max_val);
  for (size_t i = 0; i < count; ++i) {
    keys.push_back(dist(rng));
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

TEST(GallopingSearchTest, Empty) {
  std::vector<int64_t> data;
  GallopingSearch searcher(data.data(), static_cast<uint32_t>(data.size()));

  std::vector<int64_t> keys = {0, 100};
  std::vector<uint32_t> results(keys.size());
  searcher.BatchedLowerBound(keys.data(), static_cast<uint32_t>(keys.size()),
                             results.data());
  EXPECT_EQ(results[0], 0u);
  EXPECT_EQ(results[1], 0u);
}

TEST(GallopingSearchTest, SingleElement) {
  std::vector<int64_t> data = {50};
  GallopingSearch searcher(data.data(), static_cast<uint32_t>(data.size()));

  std::vector<int64_t> keys = {0, 50, 100};
  std::vector<uint32_t> results(keys.size());
  searcher.BatchedLowerBound(keys.data(), static_cast<uint32_t>(keys.size()),
                             results.data());

  auto expected = ExpectedLowerBounds(data, keys);
  EXPECT_EQ(results, expected);
}

TEST(GallopingSearchTest, DenseQueries) {
  std::vector<int64_t> data;
  for (int64_t i = 0; i < 1000; ++i) {
    data.push_back(i * 10);
  }
  GallopingSearch searcher(data.data(), static_cast<uint32_t>(data.size()));

  auto keys = GenerateUniformKeys(0, 5, 200);
  std::vector<uint32_t> results(keys.size());
  searcher.BatchedLowerBound(keys.data(), static_cast<uint32_t>(keys.size()),
                             results.data());

  auto expected = ExpectedLowerBounds(data, keys);
  EXPECT_EQ(results, expected);
}

TEST(GallopingSearchTest, SparseQueries) {
  std::vector<int64_t> data;
  for (int64_t i = 0; i < 10000; ++i) {
    data.push_back(i * 10);
  }
  GallopingSearch searcher(data.data(), static_cast<uint32_t>(data.size()));

  auto keys = GenerateUniformKeys(0, 1000, 100);
  std::vector<uint32_t> results(keys.size());
  searcher.BatchedLowerBound(keys.data(), static_cast<uint32_t>(keys.size()),
                             results.data());

  auto expected = ExpectedLowerBounds(data, keys);
  EXPECT_EQ(results, expected);
}

TEST(GallopingSearchTest, RandomSortedKeys) {
  std::vector<int64_t> data;
  for (int64_t i = 0; i < 5000; ++i) {
    data.push_back(i * 10);
  }
  GallopingSearch searcher(data.data(), static_cast<uint32_t>(data.size()));

  auto keys = GenerateSortedRandomKeys(data.front(), data.back(), 500);
  std::vector<uint32_t> results(keys.size());
  searcher.BatchedLowerBound(keys.data(), static_cast<uint32_t>(keys.size()),
                             results.data());

  auto expected = ExpectedLowerBounds(data, keys);
  EXPECT_EQ(results, expected);
}

TEST(GallopingSearchTest, KeysBeyondData) {
  std::vector<int64_t> data = {10, 20, 30, 40, 50};
  GallopingSearch searcher(data.data(), static_cast<uint32_t>(data.size()));

  std::vector<int64_t> keys = {0, 5, 25, 35, 60, 100};
  std::vector<uint32_t> results(keys.size());
  searcher.BatchedLowerBound(keys.data(), static_cast<uint32_t>(keys.size()),
                             results.data());

  auto expected = ExpectedLowerBounds(data, keys);
  EXPECT_EQ(results, expected);
}

TEST(GallopingSearchTest, DuplicateKeys) {
  std::vector<int64_t> data = {10, 20, 20, 20, 30, 40};
  GallopingSearch searcher(data.data(), static_cast<uint32_t>(data.size()));

  std::vector<int64_t> keys = {15, 20, 25};
  std::vector<uint32_t> results(keys.size());
  searcher.BatchedLowerBound(keys.data(), static_cast<uint32_t>(keys.size()),
                             results.data());

  auto expected = ExpectedLowerBounds(data, keys);
  EXPECT_EQ(results, expected);
}

// Every point of every size up to past a few doublings of the probe step, so
// the scan, each probe and the final search all see the point at each offset.
TEST(GallopingSearchTest, PartitionPointsMatchStd) {
  for (uint32_t size = 0; size < 300; ++size) {
    for (uint32_t point = 0; point <= size; ++point) {
      std::vector<uint32_t> data(size);
      for (uint32_t i = 0; i < size; ++i) {
        data[i] = i < point ? 0 : 1;
      }
      auto pred = [](uint32_t v) { return v == 0; };
      const uint32_t* b = data.data();
      const uint32_t* e = b + size;
      EXPECT_EQ(static_cast<uint32_t>(PartitionPoint(b, e, pred) - b), point)
          << size;
      EXPECT_EQ(static_cast<uint32_t>(GallopingPartitionPoint(b, e, pred) - b),
                point)
          << size;
    }
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
