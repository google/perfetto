/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_H_

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/containers/string_pool.h"

namespace perfetto::trace_processor::plugins::tree {

// Sentinel value indicating null for int64_t columns.
inline constexpr int64_t kNullInt64 = std::numeric_limits<int64_t>::max();

// Sentinel value indicating null for uint32_t columns (e.g., parent_index).
inline constexpr uint32_t kNullUint32 = std::numeric_limits<uint32_t>::max();

// Compressed Sparse Row (CSR) format for storing variable-length lists per
// node. More memory-efficient than vector<vector<T>> as it uses only 2
// allocations instead of N+1 allocations for N nodes.
//
// For N nodes with total M elements:
//   - offsets: N+1 elements, offsets[i] = start index of node i's data
//   - data: M elements, all lists concatenated
//
// Access pattern: elements for node i are data[offsets[i]..offsets[i+1])
template <typename T>
struct CsrVector {
  // STL-style typedefs for compatibility with gmock matchers.
  using value_type = base::Span<const T>;

  // Iterator for range-based for loops over nodes.
  class Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = base::Span<const T>;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = value_type;

    Iterator(const CsrVector* csr, uint32_t idx) : csr_(csr), idx_(idx) {}

    reference operator*() const { return (*csr_)[idx_]; }
    Iterator& operator++() {
      ++idx_;
      return *this;
    }
    Iterator operator++(int) {
      Iterator tmp = *this;
      ++idx_;
      return tmp;
    }
    bool operator==(const Iterator& o) const { return idx_ == o.idx_; }
    bool operator!=(const Iterator& o) const { return idx_ != o.idx_; }

   private:
    const CsrVector* csr_;
    uint32_t idx_;
  };

  using const_iterator = Iterator;
  using iterator = Iterator;

  std::vector<uint32_t> offsets;  // Size = num_nodes + 1
  std::vector<T> data;            // All elements concatenated

  CsrVector() = default;

  // Reserve space for expected number of nodes and total elements.
  void Reserve(uint32_t num_nodes, uint32_t total_elements) {
    offsets.reserve(num_nodes + 1);
    data.reserve(total_elements);
  }

  // Start building: call once before adding any nodes.
  void StartBuild() {
    offsets.clear();
    data.clear();
    offsets.push_back(0);
  }

  // Finish the current node and start the next one.
  // Call after adding all elements for the current node.
  void FinishNode() { offsets.push_back(static_cast<uint32_t>(data.size())); }

  // Add an element to the current node being built.
  void Push(T value) { data.push_back(value); }

  // Number of nodes (valid after build is complete).
  uint32_t size() const {
    if (offsets.empty()) {
      return 0;
    }
    return static_cast<uint32_t>(offsets.size()) - 1;
  }

  // Check if there are no nodes.
  bool empty() const { return size() == 0; }

  // Get elements for node i as a span (supports range-based for loops).
  base::Span<const T> operator[](uint32_t i) const {
    PERFETTO_DCHECK(i < size());
    return {data.data() + offsets[i], offsets[i + 1] - offsets[i]};
  }

  // Iterators for range-based for loops.
  Iterator begin() const { return Iterator(this, 0); }
  Iterator end() const { return Iterator(this, size()); }
};

// Merge strategy for tree_merge_siblings.
enum class TreeMergeMode : uint8_t {
  kConsecutive,  // Only merge adjacent siblings with same key
  kGlobal,       // Merge all siblings with same key
};

// Aggregation type for merged columns.
enum class TreeAggType : uint8_t {
  kMin,
  kMax,
  kSum,
  kCount,
  kAny,  // Take any value (first encountered)
};

// Specification for how to aggregate a column during merge.
struct TreeAggSpec {
  static constexpr const char* kPointerType = "TREE_AGG";
  std::string column_name;
  TreeAggType agg_type;

  TreeAggSpec(std::string col, TreeAggType agg)
      : column_name(std::move(col)), agg_type(agg) {}
};

// Operation: merge sibling nodes.
struct TreeMergeSiblingsOp {
  TreeMergeSiblingsOp(TreeMergeMode m,
                      std::vector<std::string> keys,
                      std::string order,
                      std::vector<TreeAggSpec> agg)
      : mode(m),
        key_columns(std::move(keys)),
        order_column(std::move(order)),
        aggregations(std::move(agg)) {}
  TreeMergeMode mode;
  std::vector<std::string> key_columns;
  std::string order_column;
  std::vector<TreeAggSpec> aggregations;
};

// Comparison operator for tree_delete_node.
enum class TreeCompareOp : uint8_t {
  kEq,    // Equal
  kGlob,  // Glob pattern match
};

// Specification for which nodes to delete.
// Value is stored as variant to support different comparison types.
struct TreeDeleteSpec {
  static constexpr const char* kPointerType = "TREE_DELETE_SPEC";
  std::string column_name;
  TreeCompareOp op;
  // Value to compare against. For kEq on int64, use int64_t.
  // For kEq/kGlob on string, use StringPool::Id.
  std::variant<int64_t, StringPool::Id> value;

  TreeDeleteSpec(std::string col, TreeCompareOp o, int64_t v)
      : column_name(std::move(col)), op(o), value(v) {}
  TreeDeleteSpec(std::string col, TreeCompareOp o, StringPool::Id v)
      : column_name(std::move(col)), op(o), value(v) {}
};

// Operation: delete nodes matching conditions, reparent children.
struct TreeDeleteNodeOp {
  explicit TreeDeleteNodeOp(TreeDeleteSpec s) : spec(std::move(s)) {}
  TreeDeleteSpec spec;
};

// Specification for propagating values up/down the tree.
struct TreePropagateSpec {
  static constexpr const char* kPointerType = "TREE_PROPAGATE_SPEC";
  std::string out_column;
  std::string in_column;
  TreeAggType agg_type;

  TreePropagateSpec(std::string out, std::string in, TreeAggType agg)
      : out_column(std::move(out)), in_column(std::move(in)), agg_type(agg) {}
};

// Operation: propagate values up from leaves to root.
struct TreePropagateUpOp {
  explicit TreePropagateUpOp(TreePropagateSpec s) : spec(std::move(s)) {}
  TreePropagateSpec spec;
};

// Operation: propagate values down from root to leaves.
struct TreePropagateDownOp {
  explicit TreePropagateDownOp(TreePropagateSpec s) : spec(std::move(s)) {}
  TreePropagateSpec spec;
};

// Operation: invert tree (leaves become roots) and merge siblings.
struct TreeInvertOp {
  TreeInvertOp(std::string key, std::string order, std::vector<TreeAggSpec> agg)
      : key_column(std::move(key)),
        order_column(std::move(order)),
        aggregations(std::move(agg)) {}
  std::string key_column;
  std::string order_column;
  std::vector<TreeAggSpec> aggregations;
};

// Operation: collapse parent-child chains where both have the same key.
// When a node has the same key as its parent, merge it into the parent
// (aggregate values) and reparent its children to the grandparent.
struct TreeCollapseOp {
  TreeCollapseOp(std::string key, std::vector<TreeAggSpec> agg)
      : key_column(std::move(key)), aggregations(std::move(agg)) {}
  std::string key_column;
  std::vector<TreeAggSpec> aggregations;
};

// All possible tree operations.
using TreeOp = std::variant<TreeMergeSiblingsOp,
                            TreeDeleteNodeOp,
                            TreePropagateUpOp,
                            TreePropagateDownOp,
                            TreeInvertOp,
                            TreeCollapseOp>;

// A passthrough column stores user data that's carried through tree operations.
// Uses variant to support different types without runtime type conversion
// issues. Strings are stored as interned StringPool::Id for efficiency.
struct PassthroughColumn {
  std::string name;
  // std::monostate represents uninitialized (type not yet determined)
  std::variant<std::monostate,
               std::vector<int64_t>,
               std::vector<double>,
               std::vector<StringPool::Id>>
      data;

  PassthroughColumn() = default;
  explicit PassthroughColumn(std::string n) : name(std::move(n)) {}
  PassthroughColumn(std::string n, std::vector<int64_t> d)
      : name(std::move(n)), data(std::move(d)) {}
  PassthroughColumn(std::string n, std::vector<double> d)
      : name(std::move(n)), data(std::move(d)) {}
  PassthroughColumn(std::string n, std::vector<StringPool::Id> d)
      : name(std::move(n)), data(std::move(d)) {}

  // Helper to check if this is an int64 column.
  bool IsInt64() const {
    return std::holds_alternative<std::vector<int64_t>>(data);
  }
  bool IsDouble() const {
    return std::holds_alternative<std::vector<double>>(data);
  }
  bool IsString() const {
    return std::holds_alternative<std::vector<StringPool::Id>>(data);
  }

  // Get typed access to the data.
  std::vector<int64_t>& AsInt64() {
    return std::get<std::vector<int64_t>>(data);
  }
  const std::vector<int64_t>& AsInt64() const {
    return std::get<std::vector<int64_t>>(data);
  }
  std::vector<double>& AsDouble() {
    return std::get<std::vector<double>>(data);
  }
  const std::vector<double>& AsDouble() const {
    return std::get<std::vector<double>>(data);
  }
  std::vector<StringPool::Id>& AsString() {
    return std::get<std::vector<StringPool::Id>>(data);
  }
  const std::vector<StringPool::Id>& AsString() const {
    return std::get<std::vector<StringPool::Id>>(data);
  }
};

// Inner data storage for Tree, wrapped in shared_ptr for cheap copying.
struct TreeData {
  // Structural data: parent's row index for each node (kNullUint32 for roots).
  std::vector<uint32_t> parent_indices;

  // Index into passthrough_columns for each tree node.
  // Allows lazy access: delete ops compact this without touching columns.
  // After aggregation ops, this is reset to iota and columns are materialized.
  std::vector<uint32_t> source_indices;

  // Passthrough user columns. Accessed via source_indices indirection.
  // Only modified by aggregation operations; filter ops leave this unchanged.
  // Includes original ID columns from from_parent (nulled after merge/invert).
  std::vector<PassthroughColumn> passthrough_columns;

  TreeData() = default;
  TreeData(std::vector<uint32_t> parents, PassthroughColumn col)
      : parent_indices(std::move(parents)) {
    passthrough_columns.push_back(std::move(col));
  }
};

// The TREE opaque type.
//
// Stores tree structure efficiently using vectors for structural data
// and a dataframe for passthrough user columns.
//
// Null values use sentinel: kNullInt64 for int64, kNullUint32 for uint32.
//
// Tree with unique ownership - consumed by operations.
struct Tree {
  static constexpr const char* kPointerType = "TREE";

  // Unique data storage - stolen by operations.
  std::unique_ptr<TreeData> data;

  // Pending operations to apply at emit time.
  std::vector<TreeOp> pending_ops;

  Tree() = default;
  Tree(std::unique_ptr<TreeData> d, std::vector<TreeOp> ops)
      : data(std::move(d)), pending_ops(std::move(ops)) {}

  // Check if this tree has been consumed.
  bool IsConsumed() const { return data == nullptr; }

  // Steal data and pending_ops, leaving this tree consumed.
  // Returns a new Tree owning the stolen data.
  std::unique_ptr<Tree> Steal() {
    return std::make_unique<Tree>(std::move(data), std::move(pending_ops));
  }

  // Steal and add an operation in one step.
  std::unique_ptr<Tree> StealAndAddOp(TreeOp op) {
    auto stolen = Steal();
    stolen->pending_ops.push_back(std::move(op));
    return stolen;
  }

  // Column names for structural columns in output.
  static constexpr const char* kNodeIdCol = "__node_id";
  static constexpr const char* kParentIdCol = "__parent_id";
  static constexpr const char* kDepthCol = "__depth";

  // Column names for original IDs (stored as passthrough, nulled after
  // merge/invert).
  static constexpr const char* kOriginalIdCol = "original_id";
  static constexpr const char* kOriginalParentIdCol = "original_parent_id";
};

// Helper types for tagged pointer values returned by helper macros.

struct TreeKeysSpec {
  static constexpr const char* kPointerType = "TREE_KEYS";
  explicit TreeKeysSpec(std::vector<std::string> cols)
      : column_names(std::move(cols)) {}
  std::vector<std::string> column_names;
};

struct TreeOrderSpec {
  static constexpr const char* kPointerType = "TREE_ORDER";
  explicit TreeOrderSpec(std::string col) : column_name(std::move(col)) {}
  std::string column_name;
};

struct TreeStrategySpec {
  static constexpr const char* kPointerType = "TREE_MERGE_STRATEGY";
  explicit TreeStrategySpec(TreeMergeMode m) : mode(m) {}
  TreeMergeMode mode;
};

}  // namespace perfetto::trace_processor::plugins::tree

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_TREE_TREE_H_
