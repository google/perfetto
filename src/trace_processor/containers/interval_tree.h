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

#ifndef SRC_TRACE_PROCESSOR_CONTAINERS_INTERVAL_TREE_H_
#define SRC_TRACE_PROCESSOR_CONTAINERS_INTERVAL_TREE_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace perfetto::trace_processor {

// An implementation of an interval tree data structure, designed to efficiently
// perform overlap queries on a set of intervals. Used by `interval_intersect`,
// where one set of intervals (generally the bigger one) has interval tree
// created based on it, as another queries `FindOverlaps` function for each
// interval.
// As interval tree is build on sorted (by `start`) set of N intervals, the
// complexity of creating a tree goes down from O(N*logN) to O(N) and the
// created tree is optimally balanced. Each call to `FindOverlaps` is O(logN).
class IntervalTree {
 public:
  struct Interval {
    uint32_t start;
    uint32_t end;
    uint32_t id;
  };

  // Takes vector of sorted intervals.
  explicit IntervalTree(std::vector<Interval>& sorted_intervals) {
    tree_root_ = BuildFromSortedIntervals(
        sorted_intervals, 0, static_cast<int32_t>(sorted_intervals.size() - 1));
  }

  // Modifies |overlaps| to contain ids of all intervals in the interval tree
  // that overlap with |interval|.
  void FindOverlaps(Interval interval, std::vector<uint32_t>& overlaps) const {
    if (tree_root_) {
      FindOverlaps(*tree_root_, interval, overlaps);
    }
  }

 private:
  struct Node {
    Interval interval;
    uint32_t max;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;

    explicit Node(Interval i) : interval(i), max(i.end) {}
  };

  static std::unique_ptr<Node> Insert(std::unique_ptr<Node> root, Interval i) {
    if (root == nullptr) {
      return std::make_unique<Node>(i);
    }

    if (i.start < root->interval.start) {
      root->left = Insert(std::move(root->left), i);
    } else {
      root->right = Insert(std::move(root->right), i);
    }

    if (root->max < i.end) {
      root->max = i.end;
    }

    return root;
  }

  static std::unique_ptr<Node> BuildFromSortedIntervals(
      const std::vector<Interval>& is,
      int32_t start,
      int32_t end) {
    // |start == end| happens if there is one element so we need to check for
    // |start > end| that happens in the next recursive call.
    if (start > end) {
      return nullptr;
    }

    int32_t mid = start + (end - start) / 2;
    auto node = std::make_unique<Node>(is[static_cast<uint32_t>(mid)]);

    node->left = BuildFromSortedIntervals(is, start, mid - 1);
    node->right = BuildFromSortedIntervals(is, mid + 1, end);

    uint32_t max_from_children = std::max(
        node->left ? node->left->max : std::numeric_limits<uint32_t>::min(),
        node->right ? node->right->max : std::numeric_limits<uint32_t>::min());

    node->max = std::max(node->interval.end, max_from_children);

    return node;
  }

  static void FindOverlaps(const Node& node,
                           const Interval& i,
                           std::vector<uint32_t>& overlaps) {
    // Intervals overlap if one starts before the other ends and ends after it
    // starts.
    if (node.interval.start < i.end && node.interval.end > i.start) {
      overlaps.push_back(node.interval.id);
    }

    // Try to find overlaps with left.
    if (i.start <= node.interval.start && node.left) {
      FindOverlaps(*node.left, i, overlaps);
    }

    // Try to find overlaps with right.
    if (i.start < node.max && node.right) {
      FindOverlaps(*node.right, i, overlaps);
    }
  }

  std::unique_ptr<Node> tree_root_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CONTAINERS_INTERVAL_TREE_H_
