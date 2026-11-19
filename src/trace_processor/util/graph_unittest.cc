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
#include <random>
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

// Whether `to` is reached from `from` without passing through `removed`.
bool Reaches(const Digraph& graph,
             uint32_t from,
             uint32_t to,
             uint32_t removed) {
  if (from == removed) {
    return false;
  }
  std::vector<bool> seen(graph.node_count());
  std::vector<uint32_t> stack{from};
  seen[from] = true;
  while (!stack.empty()) {
    uint32_t v = stack.back();
    stack.pop_back();
    if (v == to) {
      return true;
    }
    for (uint32_t i = graph.offsets[v]; i < graph.offsets[v + 1]; ++i) {
      uint32_t t = graph.targets[i];
      if (t != removed && !seen[t]) {
        seen[t] = true;
        stack.push_back(t);
      }
    }
  }
  return false;
}

// Checks every immediate dominator against the definition, on small random
// graphs: d dominates w if w is not reached once d is removed.
TEST(DominatorTreeTest, MatchesTheDefinitionOnRandomGraphs) {
  std::minstd_rand rng(1);
  for (int round = 0; round < 500; ++round) {
    uint32_t count = 1 + rng() % 12;
    std::vector<uint32_t> sources;
    std::vector<uint32_t> targets;
    for (uint32_t e = rng() % (3 * count); e > 0; --e) {
      sources.push_back(static_cast<uint32_t>(rng() % count));
      targets.push_back(static_cast<uint32_t>(rng() % count));
    }
    Digraph graph = Digraph::FromEdges(count, sources, targets);
    GraphTree tree = DominatorTree(graph, 0);
    EXPECT_EQ(tree.order, DepthFirstSearch(graph, {0}).order);

    for (uint32_t w = 0; w < count; ++w) {
      if (w == 0 || !Reaches(graph, 0, w, kNone)) {
        EXPECT_EQ(tree.parent[w], kNone);
        continue;
      }
      // The immediate dominator is the strict dominator which every other
      // one dominates.
      std::vector<uint32_t> dominators;
      for (uint32_t d = 0; d < count; ++d) {
        if (d != w && !Reaches(graph, 0, w, d)) {
          dominators.push_back(d);
        }
      }
      uint32_t immediate = kNone;
      for (uint32_t d : dominators) {
        bool closest = true;
        for (uint32_t other : dominators) {
          if (other != d && Reaches(graph, 0, d, other)) {
            closest = false;
          }
        }
        if (closest) {
          immediate = d;
        }
      }
      EXPECT_EQ(tree.parent[w], immediate)
          << "round " << round << " node " << w;
    }
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::util
