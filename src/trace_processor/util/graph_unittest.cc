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

#include "src/trace_processor/util/graph.h"

#include <cstdint>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

using ::testing::ElementsAre;

constexpr uint32_t kNone = GraphTree::kNoParent;

// 0 -> 1, 0 -> 2, 1 -> 3, 2 -> 3, 3 -> 0, and 4 unreachable from 0.
Digraph Diamond() {
  return Digraph::FromEdges(5, {0, 0, 1, 2, 3, 4}, {1, 2, 3, 3, 0, 0});
}

TEST(GraphSearchTest, BreadthFirstReachesEverySeedFirst) {
  GraphTree tree = BreadthFirstSearch(Diamond(), {3, 4, 3});
  EXPECT_THAT(tree.order, ElementsAre(3, 4, 0, 1, 2));
  EXPECT_THAT(tree.parent, ElementsAre(3, 0, 0, kNone, kNone));
}

TEST(GraphSearchTest, DepthFirstSkipsSeedsAlreadyReached) {
  GraphTree tree = DepthFirstSearch(Diamond(), {4, 2, 0});
  EXPECT_THAT(tree.order, ElementsAre(4, 0, 1, 3, 2));
  EXPECT_THAT(tree.parent, ElementsAre(4, 0, 0, 1, kNone));
}

// The example in the Lengauer-Tarjan paper, with R = 0, A = 1, ..., L = 12.
TEST(DominatorTreeTest, PaperExample) {
  enum : uint32_t { R, A, B, C, D, E, F, G, H, I, J, K, L, kCount };
  std::vector<uint32_t> sources = {R, R, R, A, B, B, B, C, C, D, E,
                                   F, G, G, H, H, I, J, K, K, L};
  std::vector<uint32_t> targets = {A, B, C, D, A, D, E, F, G, L, H,
                                   I, I, J, E, K, K, I, I, R, H};
  GraphTree tree =
      DominatorTree(Digraph::FromEdges(kCount, sources, targets), R);
  EXPECT_THAT(tree.order, ElementsAre(R, A, D, L, H, E, K, I, B, C, F, G, J));
  EXPECT_THAT(tree.parent,
              ElementsAre(kNone, R, R, R, R, R, C, C, R, R, G, R, D));
}

}  // namespace
}  // namespace perfetto::trace_processor::util
