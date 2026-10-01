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

#include "src/trace_processor/core/common/string_ranks.h"

#include <cstdint>

#include "src/trace_processor/containers/string_pool.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

using testing::Pointee;

TEST(StringRanksTest, RanksByContents) {
  StringPool pool;
  StringPool::Id cherry = pool.InternString("cherry");
  StringPool::Id apple = pool.InternString("apple");
  StringPool::Id banana = pool.InternString("banana");

  StringRanks ranks;
  ranks.Add(cherry);
  ranks.Add(apple);
  ranks.Add(banana);
  ranks.Add(apple);
  ranks.Rank(pool);

  EXPECT_EQ(ranks.size(), 3u);
  EXPECT_THAT(ranks.Find(apple), Pointee(0u));
  EXPECT_THAT(ranks.Find(banana), Pointee(1u));
  EXPECT_THAT(ranks.Find(cherry), Pointee(2u));
  EXPECT_EQ(ranks.Find(pool.InternString("durian")), nullptr);
}

}  // namespace
}  // namespace perfetto::trace_processor::core
