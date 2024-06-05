/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/containers/implicit_segment_forest.h"

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

struct Value {
  uint32_t value;
};

struct Sum {
  Value operator()(const Value& a, const Value& b) {
    return Value{a.value + b.value};
  }
};

TEST(ImplicitSegmentTree, SimpleSum) {
  std::vector<uint32_t> res = {209, 330, 901, 3, 10, 0, 3903, 309, 490};

  ImplicitSegmentForest<Value, Sum> forest;
  for (uint32_t x : res) {
    forest.Push(Value{x});
  }

  for (uint32_t i = 0; i < res.size(); ++i) {
    for (uint32_t j = i + 1; j < res.size(); ++j) {
      ASSERT_EQ(forest.Query(i, j).value,
                std::accumulate(res.begin() + i, res.begin() + j, 0u));
    }
  }
}

TEST(ImplicitSegmentTree, Stress) {
  static constexpr size_t kCount = 9249;
  std::minstd_rand0 rng(42);

  std::vector<uint32_t> res;
  ImplicitSegmentForest<Value, Sum> forest;
  for (uint32_t i = 0; i < kCount; ++i) {
    res.push_back(static_cast<uint32_t>(rng()));
    forest.Push(Value{res.back()});
  }

  for (uint32_t i = 0; i < 10000; ++i) {
    uint32_t s = rng() % kCount;
    uint32_t e = s + 1 + (rng() % (kCount - s));
    ASSERT_EQ(forest.Query(s, e).value,
              std::accumulate(res.begin() + s, res.begin() + e, 0u));
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
