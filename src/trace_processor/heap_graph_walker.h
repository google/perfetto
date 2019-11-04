/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_HEAP_GRAPH_WALKER_H_
#define SRC_TRACE_PROCESSOR_HEAP_GRAPH_WALKER_H_

#include <inttypes.h>
#include <map>
#include <set>
#include <vector>

// Implements two algorithms that walk a HeapGraph.
// a) Traverse all references from roots and mark the nodes as reachable.
// b) For each node, calculate two numbers:
//    1. retained: The number of bytes that are directly and indirectly
//       referenced by the node.
//    2. uniquely retained: The number of bytes that are only retained through
//       this object. If this object were destroyed, this many bytes would be
//       freed up.
//
// The algorithm for b) is a modified Tarjan's algorithm. We use Tarjan's
// algorithm to find connected components. This is such that we break cycles
// that can exist in the retention graphs. All nodes within the cycle get
// assigned the same component. Then, most of the graph algorithm operates on
// these components.
//
// For instance, the below graph, which for simplicity does not contain any
// loops.
// Apart from nodes retaining / uniquely retaining themselves:
// a retains nothing
// a uniquely retains nothing
//
// b retains a
// b uniquely retains nothing
//
// c retains a
// c uniquely retains nothing
//
// d retains a, b, c
// d uniquely retains a, b, c
//
//     a      |
//    ^^      |
//   /  \     |
//   b   c    |
//   ^   ^    |
//    \ /     |
//     d      |
//
// The basic idea of the algorithm is to assign every node a fractional
// retention of other nodes. In the same graph:
// a retains nothing
// a uniquely retains nothing
//
// b retains a
// b 1/2 uniquely retains a
//
// c retains a
// c 1/2 uniquely retains a
//
// d retains a, b, c
// d 1/2 + 1/2 = 1 uniquely retains a
// d 1 uniquely retains b and c
//
// A more complete example
//
//     a       |
//    ^^       |
//   /  \      |
//   b   c     |
//   ^   ^     |
//    \ / \    |
//     d  e    |
//     ^  ^    |
//     \  /    |
//      f      |
//
// b: 1/2 retains a
// c: 1/2 retains a
// d: 3/4 retains a (all of b's share, half of c's)
// e: 1/4 retains a (half of c's share)
// f: 4/4 = 1 retains a

namespace perfetto {
namespace trace_processor {

class Fraction {
 public:
  Fraction() : Fraction(0, 1) {}

  Fraction(const Fraction&) = default;
  Fraction(uint64_t numerator, uint64_t denominator);
  Fraction& operator+=(const Fraction& other);
  bool operator==(uint64_t other) const;
  Fraction operator*(const Fraction& other);

  uint64_t numerator() const { return numerator_; }
  uint64_t denominator() const { return denominator_; }

 private:
  // Reduce fraction. E.g., turn 2 / 4 into 1 / 2.
  void Reduce();

  uint64_t numerator_;
  uint64_t denominator_;
};

class HeapGraphWalker {
 public:
  class Delegate {
   public:
    virtual ~Delegate();
    virtual void MarkReachable(int64_t row) = 0;
    virtual void SetRetained(int64_t row,
                             int64_t retained,
                             int64_t unique_retained) = 0;
  };

  HeapGraphWalker(Delegate* delegate) : delegate_(delegate) {}

  void AddEdge(int64_t owner_row, int64_t owned_row);
  void AddNode(int64_t row, uint64_t size);

  // Mark a a node as root. This marks all the nodes reachable from it as
  // reachable.
  void MarkRoot(int64_t row);
  // Calculate the retained and unique retained size for each node. This
  // includes nodes not reachable from roots.
  void CalculateRetained();

 private:
  struct Node {
    // These are sets to conveniently get rid of double edges between nodes.
    // We do not care if an object owns another object via multiple references
    // or only one.
    std::set<Node*> children;
    std::set<Node*> parents;
    bool reachable = false;
    bool on_stack = false;
    uint64_t self_size = 0;
    uint64_t retained_size = 0;

    int64_t row = 0;
    uint64_t node_index = 0;
    uint64_t lowlink = 0;
    int64_t component = -1;
  };

  struct Component {
    uint64_t unique_retained_size = 0;
    size_t incoming_edges = 0;
    size_t orig_incoming_edges = 0;
    std::map<int64_t, Fraction> children_components;
    uint64_t lowlink = 0;
  };

  Node& GetNode(int64_t id) { return nodes_[static_cast<size_t>(id)]; }

  void FindSCC(Node*);
  void FoundSCC(Node*);
  int64_t RetainedSize(const Component&);

  // Make node and all transitive children as reachable.
  void ReachableNode(Node*);

  std::vector<Component> components_;
  std::vector<Node*> node_stack_;
  uint64_t next_node_index_ = 1;
  std::vector<Node> nodes_;

  Delegate* delegate_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_HEAP_GRAPH_WALKER_H_
