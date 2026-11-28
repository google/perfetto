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

#include "src/trace_processor/core/exec/run_vector.h"

#include <cstdint>

#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

TEST(RunBitsTest, ResizeSetsOnlyTheBitsAdded) {
  Context context;
  OperatorState& owner = *context.Make<EmptyState>();
  RunBits bits;
  bits.resize(owner, 70, false);
  bits.set(3);
  bits.resize(owner, 200, true);
  for (uint64_t i = 0; i < 200; ++i) {
    EXPECT_EQ(bits.is_set(i), i == 3 || i >= 70) << i;
  }
  bits.resize(owner, 71, false);
  bits.push_back(owner, false);
  EXPECT_FALSE(bits.is_set(71));
  EXPECT_EQ(bits.CountSetBits(), 2u);
}

TEST(RunMapTest, MapsEachKey) {
  Context context;
  OperatorState& owner = *context.Make<EmptyState>();
  RunMap map;
  for (uint32_t i = 0; i < 5000; ++i) {
    EXPECT_EQ(map.FindOrInsert(owner, i, i + 1), i + 1);
  }
  EXPECT_EQ(map.size(), 5000u);
  EXPECT_EQ(map.FindOrInsert(owner, 7, 99), 8u);
  EXPECT_EQ(*map.Find(4999), 5000u);
  EXPECT_EQ(map.Find(5000), nullptr);
  map.clear();
  EXPECT_EQ(map.Find(7), nullptr);
}

// Like FlatHashMap::Clear(), ending a run empties the map but keeps all of its
// slots.
TEST(RunMapTest, EndingARunKeepsTheSlots) {
  struct Mapping : OperatorState {
    RunMap map;
  };
  Context context;
  auto* s = context.Make<Mapping>();
  context.Made(s);
  for (uint32_t i = 0; i < 100000; ++i) {
    s->map.FindOrInsert(*s, i, i);
  }
  const uint32_t* before = s->map.Find(0);
  context.EndRun(*s);
  EXPECT_EQ(s->map.size(), 0u);
  EXPECT_EQ(s->map.Find(0), nullptr);
  EXPECT_EQ(s->map.FindOrInsert(*s, 0, 5), 5u);
  EXPECT_EQ(s->map.Find(0), before);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
