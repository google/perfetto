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

#include <cstddef>
#include <cstdint>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/circular_queue.h"

namespace perfetto::trace_processor::util {

Digraph Digraph::FromEdges(uint32_t node_count,
                           const std::vector<uint32_t>& sources,
                           const std::vector<uint32_t>& targets) {
  PERFETTO_DCHECK(sources.size() == targets.size());
  Digraph graph;
  graph.offsets.assign(node_count + 1, 0);
  for (uint32_t source : sources) {
    PERFETTO_DCHECK(source < node_count);
    ++graph.offsets[source + 1];
  }
  for (uint32_t n = 0; n < node_count; ++n) {
    graph.offsets[n + 1] += graph.offsets[n];
  }
  graph.targets.resize(targets.size());
  std::vector<uint32_t> next(graph.offsets.begin(), graph.offsets.end() - 1);
  for (size_t i = 0; i < sources.size(); ++i) {
    PERFETTO_DCHECK(targets[i] < node_count);
    graph.targets[next[sources[i]]++] = targets[i];
  }
  return graph;
}

GraphTree BreadthFirstSearch(const Digraph& graph,
                             const std::vector<uint32_t>& seeds) {
  GraphTree tree;
  tree.parent.assign(graph.node_count(), GraphTree::kNoParent);
  std::vector<bool> reached(graph.node_count());
  base::CircularQueue<uint32_t> queue;
  for (uint32_t seed : seeds) {
    if (!reached[seed]) {
      reached[seed] = true;
      queue.emplace_back(seed);
    }
  }
  while (!queue.empty()) {
    uint32_t node = queue.front();
    queue.pop_front();
    tree.order.push_back(node);
    for (uint32_t i = graph.offsets[node]; i < graph.offsets[node + 1]; ++i) {
      uint32_t next = graph.targets[i];
      if (!reached[next]) {
        reached[next] = true;
        tree.parent[next] = node;
        queue.emplace_back(next);
      }
    }
  }
  return tree;
}

GraphTree DepthFirstSearch(const Digraph& graph,
                           const std::vector<uint32_t>& seeds) {
  struct Entry {
    uint32_t node;
    uint32_t parent;
  };
  GraphTree tree;
  tree.parent.assign(graph.node_count(), GraphTree::kNoParent);
  std::vector<bool> reached(graph.node_count());
  std::vector<Entry> stack;
  for (auto it = seeds.rbegin(); it != seeds.rend(); ++it) {
    stack.push_back(Entry{*it, GraphTree::kNoParent});
  }
  while (!stack.empty()) {
    Entry entry = stack.back();
    stack.pop_back();
    if (reached[entry.node]) {
      continue;
    }
    reached[entry.node] = true;
    tree.parent[entry.node] = entry.parent;
    tree.order.push_back(entry.node);
    for (uint32_t i = graph.offsets[entry.node + 1];
         i-- > graph.offsets[entry.node];) {
      stack.push_back(Entry{graph.targets[i], entry.node});
    }
  }
  return tree;
}

}  // namespace perfetto::trace_processor::util
