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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
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

namespace {

constexpr uint32_t kNone = GraphTree::kNoParent;

// The state of the Semi-NCA algorithm. Nodes are numbered in the order the
// depth first search reaches them, and every step after the search works on
// those numbers, so it reads its arrays mostly in order.
class Dominators {
 public:
  Dominators(const Digraph& graph, uint32_t root)
      : graph_(graph), number_(graph.node_count(), kNone) {
    RunDfs(root);
  }

  GraphTree Run() {
    ComputeSemiDominators();
    ComputeDominators();
    GraphTree tree;
    tree.parent.assign(graph_.node_count(), kNone);
    for (uint32_t w = 1; w < order_.size(); ++w) {
      tree.parent[order_[w]] = order_[dominator_[w]];
    }
    tree.order = std::move(order_);
    return tree;
  }

 private:
  // Step 1: numbers the nodes in depth first order.
  void RunDfs(uint32_t root) {
    struct Entry {
      uint32_t node;
      uint32_t parent;
    };
    std::vector<Entry> stack{{root, kNone}};
    while (!stack.empty()) {
      Entry entry = stack.back();
      stack.pop_back();
      if (number_[entry.node] != kNone) {
        continue;
      }
      auto number = static_cast<uint32_t>(order_.size());
      number_[entry.node] = number;
      order_.push_back(entry.node);
      tree_parent_.push_back(entry.parent);
      for (uint32_t i = graph_.offsets[entry.node + 1];
           i-- > graph_.offsets[entry.node];) {
        stack.push_back(Entry{graph_.targets[i], number});
      }
    }
  }

  // Step 2: the semi-dominator of each node, as Lengauer-Tarjan computes it.
  void ComputeSemiDominators() {
    auto count = static_cast<uint32_t>(order_.size());
    Predecessors();
    semi_.resize(count);
    for (uint32_t w = 0; w < count; ++w) {
      semi_[w] = w;
    }
    label_ = semi_;
    ancestor_.assign(count, kNone);
    // The root is not processed.
    for (uint32_t w = count - 1; w > 0; --w) {
      uint32_t semi = semi_[w];
      for (uint32_t i = predecessor_offsets_[w];
           i < predecessor_offsets_[w + 1]; ++i) {
        semi = std::min(semi, Eval(predecessors_[i]));
      }
      semi_[w] = semi;
      label_[w] = semi;
      ancestor_[w] = tree_parent_[w];
    }
  }

  // Step 3: the immediate dominator of each node is the nearest common
  // ancestor, in the dominator tree so far, of its tree parent and its
  // semi-dominator.
  void ComputeDominators() {
    dominator_.assign(order_.size(), 0);
    for (uint32_t w = 1; w < order_.size(); ++w) {
      uint32_t d = tree_parent_[w];
      while (d > semi_[w]) {
        d = dominator_[d];
      }
      dominator_[w] = d;
    }
  }

  // The predecessors of each reached node, by number.
  void Predecessors() {
    auto count = static_cast<uint32_t>(order_.size());
    // The successors of each reached node by number, so that the second pass
    // reads them in order instead of looking each one up again.
    std::vector<uint32_t> successors;
    std::vector<uint32_t> successor_ends(count);
    predecessor_offsets_.assign(count + 1, 0);
    for (uint32_t v = 0; v < count; ++v) {
      uint32_t node = order_[v];
      for (uint32_t i = graph_.offsets[node]; i < graph_.offsets[node + 1];
           ++i) {
        uint32_t target = number_[graph_.targets[i]];
        if (target != kNone) {
          successors.push_back(target);
          ++predecessor_offsets_[target + 1];
        }
      }
      successor_ends[v] = static_cast<uint32_t>(successors.size());
    }
    for (uint32_t w = 0; w < count; ++w) {
      predecessor_offsets_[w + 1] += predecessor_offsets_[w];
    }
    predecessors_.resize(predecessor_offsets_[count]);
    std::vector<uint32_t> next(predecessor_offsets_.begin(),
                               predecessor_offsets_.end() - 1);
    uint32_t i = 0;
    for (uint32_t v = 0; v < count; ++v) {
      for (; i < successor_ends[v]; ++i) {
        predecessors_[next[successors[i]]++] = v;
      }
    }
  }

  // The smallest semi-dominator on the path from `v` up to the root of its
  // tree in the forest built so far, excluding that root.
  uint32_t Eval(uint32_t v) {
    if (ancestor_[v] != kNone) {
      Compress(v);
    }
    return label_[v];
  }

  // Path compression, with an explicit stack because heap graphs are deep.
  void Compress(uint32_t v) {
    path_.clear();
    for (uint32_t u = v; ancestor_[ancestor_[u]] != kNone; u = ancestor_[u]) {
      path_.push_back(u);
    }
    for (auto it = path_.rbegin(); it != path_.rend(); ++it) {
      uint32_t u = *it;
      uint32_t a = ancestor_[u];
      label_[u] = std::min(label_[u], label_[a]);
      ancestor_[u] = ancestor_[a];
    }
  }

  const Digraph& graph_;
  // By node, its number, or kNone if it is not reached.
  std::vector<uint32_t> number_;
  // By number: the node, and its parent in the depth first search.
  std::vector<uint32_t> order_;
  std::vector<uint32_t> tree_parent_;
  // By number: the semi-dominator, the smallest semi-dominator on its path in
  // the forest, its forest parent, and its immediate dominator.
  std::vector<uint32_t> semi_;
  std::vector<uint32_t> label_;
  std::vector<uint32_t> ancestor_;
  std::vector<uint32_t> dominator_;
  std::vector<uint32_t> predecessor_offsets_;
  std::vector<uint32_t> predecessors_;
  // Reused by Compress.
  std::vector<uint32_t> path_;
};

}  // namespace

GraphTree DominatorTree(const Digraph& graph, uint32_t root) {
  PERFETTO_DCHECK(root < graph.node_count());
  return Dominators(graph, root).Run();
}

}  // namespace perfetto::trace_processor::util
