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

#include "src/trace_processor/containers/rollup_tree.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace perfetto::trace_processor {

namespace {

// Type priority for SQLite ordering: NULL (0) < numeric (1) < text (2)
int RollupValueTypeOrder(const RollupValue& val) {
  if (std::holds_alternative<std::monostate>(val)) {
    return 0;  // NULL sorts first
  }
  if (std::holds_alternative<int64_t>(val) ||
      std::holds_alternative<double>(val)) {
    return 1;  // Numeric values
  }
  return 2;  // Text values
}

// Compare two RollupValues using SQLite semantics.
// Returns: -1 if a < b, 0 if a == b, 1 if a > b
int CompareRollupValues(const RollupValue& a, const RollupValue& b) {
  int type_a = RollupValueTypeOrder(a);
  int type_b = RollupValueTypeOrder(b);

  // Different type priorities: order by type
  if (type_a != type_b) {
    return (type_a < type_b) ? -1 : 1;
  }

  // Same type priority
  if (type_a == 0) {
    // Both NULL - equal
    return 0;
  }

  if (type_a == 1) {
    // Both numeric - compare as double
    double d_a = std::holds_alternative<int64_t>(a)
                     ? static_cast<double>(std::get<int64_t>(a))
                     : std::get<double>(a);
    double d_b = std::holds_alternative<int64_t>(b)
                     ? static_cast<double>(std::get<int64_t>(b))
                     : std::get<double>(b);
    if (d_a < d_b) return -1;
    if (d_a > d_b) return 1;
    return 0;
  }

  // Both text - lexicographic comparison
  const std::string& str_a = std::get<std::string>(a);
  const std::string& str_b = std::get<std::string>(b);
  if (str_a < str_b) return -1;
  if (str_a > str_b) return 1;
  return 0;
}

// Equality check for node matching.
bool RollupValuesEqual(const RollupValue& a, const RollupValue& b) {
  return CompareRollupValues(a, b) == 0;
}

// Gets the hierarchy value for a node at its level.
// Returns a pointer to the value, or nullptr if not available.
const RollupValue* GetNodeValue(const RollupNode* node) {
  if (node->level < 0 ||
      static_cast<size_t>(node->level) >= node->hierarchy_values.size()) {
    return nullptr;
  }
  return &node->hierarchy_values[static_cast<size_t>(node->level)];
}

}  // namespace

RollupTree::RollupTree(std::vector<std::string> hierarchy_cols,
                       size_t num_aggregates)
    : hierarchy_cols_(std::move(hierarchy_cols)),
      num_aggregates_(num_aggregates) {
  // Initialize root node
  root_ = std::make_unique<RollupNode>();
  root_->id = 0;
  root_->level = -1;
  root_->hierarchy_values.resize(hierarchy_cols_.size(), std::monostate{});
  root_->aggs.resize(num_aggregates_, std::monostate{});
}

RollupTree::~RollupTree() = default;

RollupTree::RollupTree(RollupTree&&) noexcept = default;
RollupTree& RollupTree::operator=(RollupTree&&) noexcept = default;

RollupNode* RollupTree::FindOrCreateNode(
    const std::vector<RollupValue>& segments,
    int level) {
  if (segments.empty() || level < 0) {
    return root_.get();
  }

  RollupNode* current = root_.get();
  for (int i = 0; i <= level && i < static_cast<int>(segments.size()); i++) {
    const RollupValue& segment = segments[static_cast<size_t>(i)];
    RollupNode* found = nullptr;

    // Look for existing child with matching hierarchy value at this level
    for (auto& child : current->children) {
      if (static_cast<size_t>(i) < child->hierarchy_values.size() &&
          RollupValuesEqual(child->hierarchy_values[static_cast<size_t>(i)],
                            segment)) {
        found = child.get();
        break;
      }
    }

    if (!found) {
      auto node = std::make_unique<RollupNode>();
      node->id = next_id_++;
      node->level = i;
      node->parent = current;

      // Store hierarchy values (values up to level i, rest are NULL)
      node->hierarchy_values.resize(hierarchy_cols_.size(), std::monostate{});
      for (int j = 0; j <= i && j < static_cast<int>(segments.size()); j++) {
        node->hierarchy_values[static_cast<size_t>(j)] =
            segments[static_cast<size_t>(j)];
      }

      found = node.get();
      current->children.push_back(std::move(node));
    }
    current = found;
  }
  return current;
}

void RollupTree::AddRow(int level,
                        const std::vector<RollupValue>& hierarchy_path,
                        std::vector<RollupValue> aggregates) {
  RollupNode* node = FindOrCreateNode(hierarchy_path, level);
  if (node && node != root_.get()) {
    node->aggs = std::move(aggregates);
    total_nodes_++;
  }
}

void RollupTree::SetRootAggregates(std::vector<RollupValue> aggregates) {
  root_->aggs = std::move(aggregates);
}

void RollupTree::SortTree(RollupNode* node, const RollupSortSpec& spec) {
  if (!node) {
    return;
  }

  std::sort(node->children.begin(), node->children.end(),
            [&spec](const std::unique_ptr<RollupNode>& a,
                    const std::unique_ptr<RollupNode>& b) {
              if (spec.agg_index < 0) {
                // Sort by hierarchy value at node's level
                const RollupValue* val_a = GetNodeValue(a.get());
                const RollupValue* val_b = GetNodeValue(b.get());
                // Handle nullptr (shouldn't happen, but be safe)
                if (!val_a && !val_b) return false;
                if (!val_a) return !spec.descending;  // NULL sorts first
                if (!val_b) return spec.descending;
                int cmp = CompareRollupValues(*val_a, *val_b);
                return spec.descending ? (cmp > 0) : (cmp < 0);
              }
              size_t idx = static_cast<size_t>(spec.agg_index);
              if (idx >= a->aggs.size() || idx >= b->aggs.size()) {
                return false;
              }
              const RollupValue& val_a = a->aggs[idx];
              const RollupValue& val_b = b->aggs[idx];
              int cmp = CompareRollupValues(val_a, val_b);
              return spec.descending ? (cmp > 0) : (cmp < 0);
            });

  for (auto& child : node->children) {
    SortTree(child.get(), spec);
  }
}

void RollupTree::FlattenTree(RollupNode* node,
                             const std::set<int64_t>& ids,
                             bool denylist_mode,
                             int min_depth,
                             int max_depth,
                             std::vector<RollupNode*>* out) {
  if (!node) {
    return;
  }

  // Calculate depth (root is level -1, so depth = level + 1)
  int depth = node->level + 1;

  // Don't recurse past max_depth - this is the efficiency win for max_depth
  if (depth > max_depth) {
    return;
  }

  // Add this node to output only if within depth range
  if (depth >= min_depth) {
    out->push_back(node);
  }

  // Determine if this node is expanded (shows children).
  // Root node (id=0) is always expanded so level-0 nodes are always visible.
  // In allowlist mode: nodes are expanded if their ID is in ids.
  // In denylist mode: nodes are expanded unless their ID is in ids.
  bool in_list = (ids.find(node->id) != ids.end());
  bool is_expanded =
      (node->id == 0) || (denylist_mode ? !in_list : in_list);
  node->expanded = is_expanded;

  // Recursively add children if this node is expanded
  if (is_expanded) {
    for (auto& child : node->children) {
      FlattenTree(child.get(), ids, denylist_mode, min_depth, max_depth, out);
    }
  }
}

RollupFlatRow RollupTree::NodeToFlatRow(const RollupNode* node) const {
  RollupFlatRow row;
  row.id = node->id;
  row.parent_id = node->parent ? node->parent->id : -1;
  row.depth = node->level + 1;  // Root is level -1, so depth starts at 0
  row.child_count = static_cast<int>(node->children.size());
  row.hierarchy_values = node->hierarchy_values;
  row.aggregates = node->aggs;
  return row;
}

std::vector<RollupFlatRow> RollupTree::GetRows(
    const RollupFlattenOptions& options) {
  // Sort if needed
  std::string sort_key = std::to_string(options.sort.agg_index) + "_" +
                         (options.sort.descending ? "desc" : "asc");
  if (sort_key != cached_sort_spec_) {
    SortTree(root_.get(), options.sort);
    cached_sort_spec_ = sort_key;
  }

  // Flatten the tree
  std::vector<RollupNode*> flat;
  FlattenTree(root_.get(), options.ids, options.denylist_mode, options.min_depth,
              options.max_depth, &flat);

  // Apply pagination and convert to output format
  std::vector<RollupFlatRow> result;
  int flat_size = static_cast<int>(flat.size());
  int start = options.offset;
  // Avoid integer overflow: if limit is large, just use flat_size
  int end = (options.limit > flat_size - start)
                ? flat_size
                : options.offset + options.limit;

  for (int i = start; i < end; i++) {
    result.push_back(NodeToFlatRow(flat[static_cast<size_t>(i)]));
  }

  return result;
}

int RollupTree::GetTotalRows(const RollupFlattenOptions& options) {
  // Sort if needed (to ensure consistent state)
  std::string sort_key = std::to_string(options.sort.agg_index) + "_" +
                         (options.sort.descending ? "desc" : "asc");
  if (sort_key != cached_sort_spec_) {
    SortTree(root_.get(), options.sort);
    cached_sort_spec_ = sort_key;
  }

  // Flatten without pagination to count
  std::vector<RollupNode*> flat;
  FlattenTree(root_.get(), options.ids, options.denylist_mode, options.min_depth,
              options.max_depth, &flat);
  return static_cast<int>(flat.size());
}

}  // namespace perfetto::trace_processor
