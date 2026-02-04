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

#include "src/trace_processor/containers/pivot_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace perfetto::trace_processor {

namespace {

// Convert a PivotValue to a sortable double for comparison.
double PivotValueToDouble(const PivotValue& val) {
  if (std::holds_alternative<std::monostate>(val)) {
    return std::numeric_limits<double>::lowest();
  }
  if (std::holds_alternative<int64_t>(val)) {
    return static_cast<double>(std::get<int64_t>(val));
  }
  if (std::holds_alternative<double>(val)) {
    return std::get<double>(val);
  }
  // For strings, return 0 (can't meaningfully convert to double)
  return 0.0;
}

// Gets the display name for a node (the hierarchy value at its level).
std::string GetNodeName(const PivotNode* node) {
  if (node->level < 0 ||
      static_cast<size_t>(node->level) >= node->hierarchy_values.size()) {
    return "";
  }
  return node->hierarchy_values[static_cast<size_t>(node->level)];
}

}  // namespace

PivotTable::PivotTable(std::vector<std::string> hierarchy_cols,
                       size_t num_aggregates)
    : hierarchy_cols_(std::move(hierarchy_cols)),
      num_aggregates_(num_aggregates) {
  // Initialize root node
  root_ = std::make_unique<PivotNode>();
  root_->id = 0;
  root_->level = -1;
  root_->hierarchy_values.resize(hierarchy_cols_.size());
  root_->aggs.resize(num_aggregates_, std::monostate{});
}

PivotTable::~PivotTable() = default;

PivotTable::PivotTable(PivotTable&&) noexcept = default;
PivotTable& PivotTable::operator=(PivotTable&&) noexcept = default;

PivotNode* PivotTable::FindOrCreateNode(
    const std::vector<std::string>& segments,
    int level) {
  if (segments.empty() || level < 0) {
    return root_.get();
  }

  PivotNode* current = root_.get();
  for (int i = 0; i <= level && i < static_cast<int>(segments.size()); i++) {
    const std::string& segment = segments[static_cast<size_t>(i)];
    PivotNode* found = nullptr;

    // Look for existing child with matching hierarchy value at this level
    for (auto& child : current->children) {
      if (static_cast<size_t>(i) < child->hierarchy_values.size() &&
          child->hierarchy_values[static_cast<size_t>(i)] == segment) {
        found = child.get();
        break;
      }
    }

    if (!found) {
      auto node = std::make_unique<PivotNode>();
      node->id = next_id_++;
      node->level = i;
      node->parent = current;

      // Store hierarchy values (values up to level i, rest empty for NULL)
      node->hierarchy_values.resize(hierarchy_cols_.size());
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

void PivotTable::AddRow(int level,
                        const std::vector<std::string>& hierarchy_path,
                        std::vector<PivotValue> aggregates) {
  PivotNode* node = FindOrCreateNode(hierarchy_path, level);
  if (node && node != root_.get()) {
    node->aggs = std::move(aggregates);
    total_nodes_++;
  }
}

void PivotTable::SetRootAggregates(std::vector<PivotValue> aggregates) {
  root_->aggs = std::move(aggregates);
}

void PivotTable::SortTree(PivotNode* node, const PivotSortSpec& spec) {
  if (!node) {
    return;
  }

  std::sort(node->children.begin(), node->children.end(),
            [&spec](const std::unique_ptr<PivotNode>& a,
                    const std::unique_ptr<PivotNode>& b) {
              if (spec.agg_index < 0) {
                // Sort by name (hierarchy value at node's level)
                std::string name_a = GetNodeName(a.get());
                std::string name_b = GetNodeName(b.get());
                return spec.descending ? (name_a > name_b) : (name_a < name_b);
              }
              size_t idx = static_cast<size_t>(spec.agg_index);
              if (idx >= a->aggs.size() || idx >= b->aggs.size()) {
                return false;
              }
              const PivotValue& val_a = a->aggs[idx];
              const PivotValue& val_b = b->aggs[idx];

              // Handle string comparison for MIN/MAX of text
              if (std::holds_alternative<std::string>(val_a) &&
                  std::holds_alternative<std::string>(val_b)) {
                const std::string& str_a = std::get<std::string>(val_a);
                const std::string& str_b = std::get<std::string>(val_b);
                return spec.descending ? (str_a > str_b) : (str_a < str_b);
              }

              // For numeric types, convert to double
              double d_a = PivotValueToDouble(val_a);
              double d_b = PivotValueToDouble(val_b);
              return spec.descending ? (d_a > d_b) : (d_a < d_b);
            });

  for (auto& child : node->children) {
    SortTree(child.get(), spec);
  }
}

void PivotTable::FlattenTree(PivotNode* node,
                             const std::set<int64_t>& ids,
                             bool denylist_mode,
                             int min_depth,
                             int max_depth,
                             std::vector<PivotNode*>* out) {
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

PivotFlatRow PivotTable::NodeToFlatRow(const PivotNode* node) const {
  PivotFlatRow row;
  row.id = node->id;
  row.parent_id = node->parent ? node->parent->id : -1;
  row.depth = node->level + 1;  // Root is level -1, so depth starts at 0
  row.child_count = static_cast<int>(node->children.size());
  row.hierarchy_values = node->hierarchy_values;
  row.aggregates = node->aggs;
  return row;
}

std::vector<PivotFlatRow> PivotTable::GetRows(
    const PivotFlattenOptions& options) {
  // Sort if needed
  std::string sort_key = std::to_string(options.sort.agg_index) + "_" +
                         (options.sort.descending ? "desc" : "asc");
  if (sort_key != cached_sort_spec_) {
    SortTree(root_.get(), options.sort);
    cached_sort_spec_ = sort_key;
  }

  // Flatten the tree
  std::vector<PivotNode*> flat;
  FlattenTree(root_.get(), options.ids, options.denylist_mode, options.min_depth,
              options.max_depth, &flat);

  // Apply pagination and convert to output format
  std::vector<PivotFlatRow> result;
  int start = options.offset;
  int end = std::min(static_cast<int>(flat.size()),
                     options.offset + options.limit);

  for (int i = start; i < end; i++) {
    result.push_back(NodeToFlatRow(flat[static_cast<size_t>(i)]));
  }

  return result;
}

int PivotTable::GetTotalRows(const PivotFlattenOptions& options) {
  // Sort if needed (to ensure consistent state)
  std::string sort_key = std::to_string(options.sort.agg_index) + "_" +
                         (options.sort.descending ? "desc" : "asc");
  if (sort_key != cached_sort_spec_) {
    SortTree(root_.get(), options.sort);
    cached_sort_spec_ = sort_key;
  }

  // Flatten without pagination to count
  std::vector<PivotNode*> flat;
  FlattenTree(root_.get(), options.ids, options.denylist_mode, options.min_depth,
              options.max_depth, &flat);
  return static_cast<int>(flat.size());
}

}  // namespace perfetto::trace_processor
