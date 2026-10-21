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

#ifndef SRC_TRACE_PROCESSOR_UTIL_GRAPH_H_
#define SRC_TRACE_PROCESSOR_UTIL_GRAPH_H_

#include <cstdint>
#include <limits>
#include <vector>

namespace perfetto::trace_processor::util {

// A directed graph over the nodes [0, node_count()). The successors of node n
// are targets[offsets[n]] up to targets[offsets[n + 1]], in the order its
// edges were added.
struct Digraph {
  // Builds the graph from its edges, keeping each node's edges in order.
  static Digraph FromEdges(uint32_t node_count,
                           const std::vector<uint32_t>& sources,
                           const std::vector<uint32_t>& targets);

  uint32_t node_count() const {
    return static_cast<uint32_t>(offsets.size() - 1);
  }

  std::vector<uint32_t> offsets{0};
  std::vector<uint32_t> targets;
};

// A tree over the nodes of a graph which are reachable from some seeds.
struct GraphTree {
  static constexpr uint32_t kNoParent = std::numeric_limits<uint32_t>::max();

  // The nodes in the tree, parents before their children.
  std::vector<uint32_t> order;
  // By node, its parent in the tree, or kNoParent for a root and for a node
  // which is not in the tree.
  std::vector<uint32_t> parent;
};

// Searches breadth first from `seeds`, which are all reached first in order.
// Each node's parent is the first node searched which has an edge to it.
GraphTree BreadthFirstSearch(const Digraph&,
                             const std::vector<uint32_t>& seeds);

// Searches depth first from each seed in order which has not already been
// reached, following each node's edges in order. Each node's parent is the
// node it was reached from.
GraphTree DepthFirstSearch(const Digraph&, const std::vector<uint32_t>& seeds);

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_GRAPH_H_
