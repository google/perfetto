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

// The state of the Lengauer-Tarjan algorithm. Nodes are numbered in the order
// the depth first search reaches them, and `semi` holds those numbers.
class Dominators {
 public:
  Dominators(const Digraph& graph, uint32_t root)
      : graph_(graph),
        semi_(graph.node_count(), kNone),
        tree_parent_(graph.node_count(), kNone),
        dominator_(graph.node_count(), kNone),
        bucket_head_(graph.node_count(), kNone),
        bucket_next_(graph.node_count(), kNone),
        ancestor_(graph.node_count(), kNone),
        label_(graph.node_count()) {
    for (uint32_t n = 0; n < graph.node_count(); ++n) {
      label_[n] = n;
    }
    RunDfs(root);
  }

  GraphTree Run() {
    ComputeSemiDominatorsAndPartialDominators();
    ComputeDominators();
    GraphTree tree;
    tree.parent = std::move(dominator_);
    tree.parent[order_[0]] = kNone;
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
      if (InTree(entry.node)) {
        continue;
      }
      semi_[entry.node] = static_cast<uint32_t>(order_.size());
      tree_parent_[entry.node] = entry.parent;
      order_.push_back(entry.node);
      for (uint32_t i = graph_.offsets[entry.node + 1];
           i-- > graph_.offsets[entry.node];) {
        stack.push_back(Entry{graph_.targets[i], semi_[entry.node]});
      }
    }
  }

  // Steps 2 and 3.
  void ComputeSemiDominatorsAndPartialDominators() {
    Digraph predecessors = Reverse();
    // The root is not processed.
    for (auto i = static_cast<uint32_t>(order_.size()) - 1; i > 0; --i) {
      uint32_t w = order_[i];
      for (uint32_t j = predecessors.offsets[w];
           j < predecessors.offsets[w + 1]; ++j) {
        uint32_t v = predecessors.targets[j];
        if (!InTree(v)) {
          continue;
        }
        semi_[w] = std::min(semi_[w], semi_[Eval(v)]);
      }
      uint32_t semi_dominator = order_[semi_[w]];
      bucket_next_[w] = bucket_head_[semi_dominator];
      bucket_head_[semi_dominator] = w;

      uint32_t parent = order_[tree_parent_[w]];
      ancestor_[w] = parent;
      for (uint32_t v = bucket_head_[parent]; v != kNone; v = bucket_next_[v]) {
        uint32_t u = Eval(v);
        dominator_[v] = semi_[u] < semi_[v] ? u : parent;
      }
      bucket_head_[parent] = kNone;
    }
  }

  // Step 4.
  void ComputeDominators() {
    // The root is not processed.
    for (uint32_t i = 1; i < order_.size(); ++i) {
      uint32_t w = order_[i];
      if (dominator_[w] != order_[semi_[w]]) {
        dominator_[w] = dominator_[dominator_[w]];
      }
    }
  }

  // The predecessors of each node in the tree.
  Digraph Reverse() const {
    std::vector<uint32_t> sources;
    std::vector<uint32_t> targets;
    for (uint32_t v : order_) {
      for (uint32_t i = graph_.offsets[v]; i < graph_.offsets[v + 1]; ++i) {
        sources.push_back(graph_.targets[i]);
        targets.push_back(v);
      }
    }
    return Digraph::FromEdges(graph_.node_count(), sources, targets);
  }

  bool InTree(uint32_t v) const { return semi_[v] != kNone; }

  // The node with the smallest semi-dominator on the path from `v` up to the
  // root of its tree in the forest built by linking.
  uint32_t Eval(uint32_t v) {
    if (ancestor_[v] == kNone) {
      return v;
    }
    Compress(v);
    return label_[v];
  }

  // Path compression, with an explicit stack because heap graphs are deep.
  void Compress(uint32_t v) {
    std::vector<uint32_t> path;
    for (uint32_t u = v; ancestor_[ancestor_[u]] != kNone; u = ancestor_[u]) {
      path.push_back(u);
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      uint32_t u = *it;
      uint32_t a = ancestor_[u];
      if (semi_[label_[a]] < semi_[label_[u]]) {
        label_[u] = label_[a];
      }
      ancestor_[u] = ancestor_[a];
    }
  }

  const Digraph& graph_;
  std::vector<uint32_t> order_;
  std::vector<uint32_t> semi_;
  std::vector<uint32_t> tree_parent_;
  std::vector<uint32_t> dominator_;
  // The nodes waiting for their dominator, as a list per semi-dominator.
  std::vector<uint32_t> bucket_head_;
  std::vector<uint32_t> bucket_next_;
  std::vector<uint32_t> ancestor_;
  std::vector<uint32_t> label_;
};

}  // namespace

GraphTree DominatorTree(const Digraph& graph, uint32_t root) {
  PERFETTO_DCHECK(root < graph.node_count());
  return Dominators(graph, root).Run();
}

}  // namespace perfetto::trace_processor::util
