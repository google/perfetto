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

#include "src/trace_processor/perfetto_sql/tree/tree_plugin.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/perfetto_sql/intrinsic_helpers.h"
#include "src/trace_processor/perfetto_sql/tree/column_utils.h"
#include "src/trace_processor/perfetto_sql/tree/tree.h"
#include "src/trace_processor/perfetto_sql/tree/tree_algorithms.h"
#include "src/trace_processor/plugins/plugin_context.h"
#include "src/trace_processor/sqlite/bindings/sqlite_aggregate_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto::trace_processor::plugins::tree {
namespace {

// Shared helpers from plugins namespace
using plugins::ExpectPointer;
using plugins::GetPointer;
using plugins::MakeUniquePtrResult;
using plugins::UniquePtrResult;

base::StatusOr<TreeAggType> ParseTreeAggType(const std::string& str) {
  if (str == "MIN")
    return TreeAggType::kMin;
  if (str == "MAX")
    return TreeAggType::kMax;
  if (str == "SUM")
    return TreeAggType::kSum;
  if (str == "COUNT")
    return TreeAggType::kCount;
  if (str == "ANY")
    return TreeAggType::kAny;
  return base::ErrStatus("Invalid aggregation type '%s'", str.c_str());
}

base::StatusOr<TreeMergeMode> ParseTreeMergeMode(const std::string& str) {
  if (str == "CONSECUTIVE")
    return TreeMergeMode::kConsecutive;
  if (str == "GLOBAL")
    return TreeMergeMode::kGlobal;
  return base::ErrStatus("Invalid merge mode '%s'", str.c_str());
}

base::StatusOr<TreeCompareOp> ParseTreeCompareOp(const std::string& str) {
  if (str == "EQ")
    return TreeCompareOp::kEq;
  if (str == "GLOB")
    return TreeCompareOp::kGlob;
  return base::ErrStatus("Invalid compare op '%s'", str.c_str());
}

// Create an ArgSpec for a pointer type using the type's kPointerType.
template <typename T>
sqlite::utils::ArgSpec PointerArg() {
  return {T::kPointerType, T::kPointerType};
}

// =============================================================================
// Tree operation helpers - reduce code duplication in Execute* functions
// =============================================================================

// Find aggregation type for a column, defaulting to kAny.
TreeAggType FindAggType(const std::string& col_name,
                        const std::vector<TreeAggSpec>& agg_specs) {
  for (const auto& spec : agg_specs) {
    if (spec.column_name == col_name) {
      return spec.agg_type;
    }
  }
  return TreeAggType::kAny;
}

// Aggregate passthrough columns given merged_sources mapping.
std::vector<PassthroughColumn> AggregatePassthroughColumns(
    const std::vector<PassthroughColumn>& materialized,
    const CsrVector<uint32_t>& merged_sources,
    const std::vector<TreeAggSpec>& agg_specs) {
  std::vector<PassthroughColumn> result;
  result.reserve(materialized.size());

  for (const auto& col : materialized) {
    TreeAggType agg_type = FindAggType(col.name, agg_specs);
    if (col.IsInt64()) {
      result.emplace_back(
          col.name, AggregateColumn(col.AsInt64(), merged_sources, agg_type));
    } else if (col.IsDouble()) {
      result.emplace_back(
          col.name, AggregateColumn(col.AsDouble(), merged_sources, agg_type));
    } else if (col.IsString()) {
      // Strings use ANY (take first)
      std::vector<StringPool::Id> agg_result;
      agg_result.reserve(merged_sources.size());
      for (auto merged_source : merged_sources) {
        agg_result.push_back(col.AsString()[merged_source[0]]);
      }
      result.emplace_back(col.name, std::move(agg_result));
    }
  }
  return result;
}

// Null out original ID columns after merge/invert operations.
// These columns become meaningless after such operations.
void NullOutOriginalIdColumns(std::vector<PassthroughColumn>& columns,
                              uint32_t count) {
  for (auto& col : columns) {
    if (col.name == Tree::kOriginalIdCol ||
        col.name == Tree::kOriginalParentIdCol) {
      col.data = std::vector<int64_t>(count, kNullInt64);
    }
  }
}

// =============================================================================
// __intrinsic_tree_key(column_name STRING) -> TREE_KEY pointer
// =============================================================================
struct TreeKey : public sqlite::Function<TreeKey> {
  static constexpr char kName[] = "__intrinsic_tree_key";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckExactArgTypes(
                 kName, argc, argv, {{sqlite::Type::kText, "column_name"}}));
    return MakeUniquePtrResult<TreeKeySpec>(ctx, sqlite::value::Text(argv[0]));
  }
};

// =============================================================================
// __intrinsic_tree_order(column_name STRING) -> TREE_ORDER pointer
// =============================================================================
struct TreeOrder : public sqlite::Function<TreeOrder> {
  static constexpr char kName[] = "__intrinsic_tree_order";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckExactArgTypes(
                 kName, argc, argv, {{sqlite::Type::kText, "column_name"}}));
    return MakeUniquePtrResult<TreeOrderSpec>(ctx,
                                              sqlite::value::Text(argv[0]));
  }
};

// =============================================================================
// __intrinsic_tree_agg(column_name STRING, agg_type STRING) -> TREE_AGG pointer
// =============================================================================
struct TreeAgg : public sqlite::Function<TreeAgg> {
  static constexpr char kName[] = "__intrinsic_tree_agg";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 2;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(ctx, sqlite::utils::CheckExactArgTypes(
                                    kName, argc, argv,
                                    {{sqlite::Type::kText, "column_name"},
                                     {sqlite::Type::kText, "agg_type"}}));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto agg_type,
                            ParseTreeAggType(sqlite::value::Text(argv[1])));
    return MakeUniquePtrResult<TreeAggSpec>(ctx, sqlite::value::Text(argv[0]),
                                            agg_type);
  }
};

// =============================================================================
// __intrinsic_tree_merge_strategy(mode STRING) -> TREE_MERGE_STRATEGY pointer
// =============================================================================
struct TreeMergeStrategy : public sqlite::Function<TreeMergeStrategy> {
  static constexpr char kName[] = "__intrinsic_tree_merge_strategy";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckExactArgTypes(
                 kName, argc, argv, {{sqlite::Type::kText, "mode"}}));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto mode,
                            ParseTreeMergeMode(sqlite::value::Text(argv[0])));
    return MakeUniquePtrResult<TreeStrategySpec>(ctx, mode);
  }
};

// =============================================================================
// __intrinsic_tree_delete_spec(col STRING, op STRING, value ANY) ->
// TREE_DELETE_SPEC
// =============================================================================
struct TreeDeleteSpecFn : public sqlite::Function<TreeDeleteSpecFn> {
  using UserData = StringPool;
  static constexpr char kName[] = "__intrinsic_tree_delete_spec";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 3;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(ctx,
                           sqlite::utils::CheckArgCountAtLeast(kName, argc, 3));
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgTypes(kName, argv,
                                          {{sqlite::Type::kText, "column_name"},
                                           {sqlite::Type::kText, "op"}}));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto op,
                            ParseTreeCompareOp(sqlite::value::Text(argv[1])));
    std::string col = sqlite::value::Text(argv[0]);
    switch (sqlite::value::Type(argv[2])) {
      case sqlite::Type::kInteger:
        return MakeUniquePtrResult<TreeDeleteSpec>(
            ctx, std::move(col), op, sqlite::value::Int64(argv[2]));
      case sqlite::Type::kText:
        return MakeUniquePtrResult<TreeDeleteSpec>(
            ctx, std::move(col), op,
            GetUserData(ctx)->InternString(sqlite::value::Text(argv[2])));
      case sqlite::Type::kFloat:
      case sqlite::Type::kBlob:
      case sqlite::Type::kNull:
        return sqlite::utils::SetError(
            ctx,
            "__intrinsic_tree_delete_spec: value must be integer or string");
    }
  }
};

// =============================================================================
// __intrinsic_tree_propagate_spec(out_col, in_col, agg_type) ->
// TREE_PROPAGATE_SPEC
// =============================================================================
struct TreePropagateSpecFn : public sqlite::Function<TreePropagateSpecFn> {
  static constexpr char kName[] = "__intrinsic_tree_propagate_spec";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 3;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(ctx, sqlite::utils::CheckExactArgTypes(
                                    kName, argc, argv,
                                    {{sqlite::Type::kText, "out_column"},
                                     {sqlite::Type::kText, "in_column"},
                                     {sqlite::Type::kText, "agg_type"}}));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto agg_type,
                            ParseTreeAggType(sqlite::value::Text(argv[2])));
    return MakeUniquePtrResult<TreePropagateSpec>(
        ctx, sqlite::value::Text(argv[0]), sqlite::value::Text(argv[1]),
        agg_type);
  }
};

// =============================================================================
// __intrinsic_tree_delete_node(tree, spec) -> TREE
// =============================================================================
struct TreeDeleteNode : public sqlite::Function<TreeDeleteNode> {
  static constexpr char kName[] = "__intrinsic_tree_delete_node";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 2;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 2));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* tree,
                            ExpectPointer<Tree>(argv[0], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* spec,
                            ExpectPointer<TreeDeleteSpec>(argv[1], kName));
    return UniquePtrResult(ctx, tree->CopyAndAddOp(TreeDeleteNodeOp(*spec)));
  }
};

// =============================================================================
// __intrinsic_tree_propagate_up(tree, spec) -> TREE
// =============================================================================
struct TreePropagateUp : public sqlite::Function<TreePropagateUp> {
  static constexpr char kName[] = "__intrinsic_tree_propagate_up";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 2;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 2));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* tree,
                            ExpectPointer<Tree>(argv[0], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* spec,
                            ExpectPointer<TreePropagateSpec>(argv[1], kName));
    return UniquePtrResult(ctx, tree->CopyAndAddOp(TreePropagateUpOp(*spec)));
  }
};

// =============================================================================
// __intrinsic_tree_propagate_down(tree, spec) -> TREE
// =============================================================================
struct TreePropagateDown : public sqlite::Function<TreePropagateDown> {
  static constexpr char kName[] = "__intrinsic_tree_propagate_down";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = 2;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::CheckArgCount(kName, static_cast<size_t>(argc), 2));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* tree,
                            ExpectPointer<Tree>(argv[0], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* spec,
                            ExpectPointer<TreePropagateSpec>(argv[1], kName));
    return UniquePtrResult(ctx, tree->CopyAndAddOp(TreePropagateDownOp(*spec)));
  }
};

// =============================================================================
// tree_invert(tree, key, order, aggs...) -> TREE
// =============================================================================
struct TreeInvert : public sqlite::Function<TreeInvert> {
  static constexpr char kName[] = "tree_invert";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = -1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(ctx,
                           sqlite::utils::CheckArgCountAtLeast(kName, argc, 3));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* tree,
                            ExpectPointer<Tree>(argv[0], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* key,
                            ExpectPointer<TreeKeySpec>(argv[1], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* order,
                            ExpectPointer<TreeOrderSpec>(argv[2], kName));

    // Collect aggregations
    std::vector<TreeAggSpec> aggs;
    for (int i = 3; i < argc; ++i) {
      SQLITE_ASSIGN_OR_RETURN(ctx, auto* agg,
                              ExpectPointer<TreeAggSpec>(argv[i], kName));
      aggs.push_back(*agg);
    }
    return UniquePtrResult(
        ctx, tree->CopyAndAddOp(TreeInvertOp(
                 key->column_name, order->column_name, std::move(aggs))));
  }
};

// =============================================================================
// __intrinsic_tree_from_parent_agg - Aggregate to build tree from parent refs
// =============================================================================
struct TreeFromParentAggContext {
  StringPool* pool = nullptr;

  // Structural data (vectors for efficient tree operations)
  std::vector<int64_t> node_ids;
  std::vector<int64_t> parent_ids;  // kNullInt64 for roots
  base::FlatHashMap<int64_t, uint32_t> id_to_row;

  // Passthrough user columns stored directly as typed vectors
  std::vector<PassthroughColumn> passthrough_columns;
};

struct TreeFromParentAgg : public sqlite::AggregateFunction<TreeFromParentAgg> {
  using UserData = StringPool;
  static constexpr char kName[] = "__intrinsic_tree_from_parent_agg";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = -1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    // argv[0] = id value
    // argv[1] = parent_id value (nullable)
    // argv[2..] = (col_name, col_value) pairs for passthrough columns

    if (argc < 2 || (argc - 2) % 2 != 0) {
      return sqlite::utils::SetError(
          ctx,
          "__intrinsic_tree_from_parent_agg: expected (id, parent_id, "
          "[col_name, col_value]...)");
    }

    auto& agg_ctx = sqlite::AggregateContext<
        TreeFromParentAggContext>::GetOrCreateContextForStep(ctx);

    // First row: initialize passthrough columns if there are user columns
    if (agg_ctx.node_ids.empty() && argc > 2) {
      agg_ctx.pool = GetUserData(ctx);

      // Initialize passthrough columns with names (types will be set on first
      // value)
      for (int i = 2; i < argc; i += 2) {
        SQLITE_RETURN_IF_ERROR(
            ctx, sqlite::utils::ExpectArgType(argv[i], sqlite::Type::kText,
                                              kName, "column_name"));
        // Type will be set when we see the first non-null value
        agg_ctx.passthrough_columns.emplace_back(sqlite::value::Text(argv[i]));
      }
    }

    // Get id
    SQLITE_RETURN_IF_ERROR(
        ctx, sqlite::utils::ExpectArgType(argv[0], sqlite::Type::kInteger,
                                          kName, "id"));
    int64_t id = sqlite::value::Int64(argv[0]);

    // Check for duplicate id
    if (agg_ctx.id_to_row.Find(id)) {
      return sqlite::utils::SetError(
          ctx,
          base::ErrStatus(
              "__intrinsic_tree_from_parent_agg: duplicate node ID: %" PRId64,
              id));
    }
    auto row_idx = static_cast<uint32_t>(agg_ctx.node_ids.size());
    agg_ctx.id_to_row[id] = row_idx;
    agg_ctx.node_ids.push_back(id);

    // Get parent_id (nullable - use sentinel for null)
    if (sqlite::value::Type(argv[1]) == sqlite::Type::kNull) {
      agg_ctx.parent_ids.push_back(kNullInt64);
    } else if (sqlite::value::Type(argv[1]) == sqlite::Type::kInteger) {
      agg_ctx.parent_ids.push_back(sqlite::value::Int64(argv[1]));
    } else {
      return sqlite::utils::SetError(
          ctx,
          "__intrinsic_tree_from_parent_agg: parent_id must be integer or "
          "null");
    }

    // Push passthrough user columns
    uint32_t col_idx = 0;
    for (int i = 3; i < argc; i += 2, ++col_idx) {
      if (col_idx >= agg_ctx.passthrough_columns.size()) {
        return sqlite::utils::SetError(
            ctx,
            "__intrinsic_tree_from_parent_agg: column index out of bounds");
      }
      auto& col = agg_ctx.passthrough_columns[col_idx];
      SQLITE_RETURN_IF_ERROR(
          ctx, PushSqliteValueToColumn(col, argv[i], agg_ctx.pool));
    }
  }

  PERFETTO_TEMPLATED_USED static void Final(sqlite3_context* ctx) {
    auto agg_ctx = sqlite::AggregateContext<
        TreeFromParentAggContext>::GetContextOrNullForFinal(ctx);
    if (!agg_ctx || agg_ctx.get()->node_ids.empty()) {
      return sqlite::result::Null(ctx);
    }

    auto* agg = agg_ctx.get();
    const uint32_t n = static_cast<uint32_t>(agg->node_ids.size());

    // Build the Tree
    auto tree = std::make_unique<Tree>();
    tree->data = std::make_shared<TreeData>();

    // Compute parent_indices from parent_ids
    tree->data->parent_indices.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      int64_t parent_id = agg->parent_ids[i];
      if (parent_id == kNullInt64) {
        tree->data->parent_indices[i] = kNullUint32;
      } else {
        auto* row_ptr = agg->id_to_row.Find(parent_id);
        if (!row_ptr) {
          return sqlite::utils::SetError(
              ctx, "tree_from_parent: orphan node (parent_id not found)");
        }
        tree->data->parent_indices[i] = *row_ptr;
      }
    }

    // Store original IDs as passthrough columns (first two columns)
    tree->data->passthrough_columns.emplace_back(Tree::kOriginalIdCol,
                                                 std::move(agg->node_ids));
    tree->data->passthrough_columns.emplace_back(Tree::kOriginalParentIdCol,
                                                 std::move(agg->parent_ids));

    // Add user passthrough columns
    for (auto& col : agg->passthrough_columns) {
      tree->data->passthrough_columns.push_back(std::move(col));
    }

    // Initialize source_indices as iota (0, 1, 2, ...)
    tree->data->source_indices.resize(n);
    std::iota(tree->data->source_indices.begin(),
              tree->data->source_indices.end(), 0u);
    return UniquePtrResult(ctx, std::move(tree));
  }
};

// =============================================================================
// tree_merge_siblings - Adds merge operation to tree (lazy)
// =============================================================================
struct TreeMergeSiblings : public sqlite::Function<TreeMergeSiblings> {
  static constexpr char kName[] = "tree_merge_siblings";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = -1;

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(ctx,
                           sqlite::utils::CheckArgCountAtLeast(kName, argc, 4));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* tree,
                            ExpectPointer<Tree>(argv[0], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* strategy,
                            ExpectPointer<TreeStrategySpec>(argv[1], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* key,
                            ExpectPointer<TreeKeySpec>(argv[2], kName));
    SQLITE_ASSIGN_OR_RETURN(ctx, auto* order,
                            ExpectPointer<TreeOrderSpec>(argv[3], kName));

    // Collect aggregations
    std::vector<TreeAggSpec> aggs;
    for (int i = 4; i < argc; ++i) {
      SQLITE_ASSIGN_OR_RETURN(ctx, auto* agg,
                              ExpectPointer<TreeAggSpec>(argv[i], kName));
      aggs.push_back(*agg);
    }
    return UniquePtrResult(ctx, tree->CopyAndAddOp(TreeMergeSiblingsOp(
                                    strategy->mode, key->column_name,
                                    order->column_name, std::move(aggs))));
  }
};

// =============================================================================
// __intrinsic_tree_emit - Executes pending ops and returns TABLE
// =============================================================================
struct TreeEmit : public sqlite::Function<TreeEmit> {
  using UserData = StringPool;
  static constexpr char kName[] = "__intrinsic_tree_emit";
  PERFETTO_TEMPLATED_USED static constexpr int kArgCount = -1;

  // Execute a delete operation in place.
  // Compacts source_indices and parent_indices; passthrough_columns unchanged.
  static base::Status ExecuteDelete(TreeData& data,
                                    const TreeDeleteNodeOp& op,
                                    StringPool* pool) {
    ASSIGN_OR_RETURN(auto delete_result, DeleteNodes(data, op.spec, pool));

    const auto old_count = static_cast<uint32_t>(data.parent_indices.size());
    const auto new_row_count =
        static_cast<uint32_t>(delete_result.new_parent_indices.size());

    // Compact source_indices using old_to_new mapping
    std::vector<uint32_t> new_source_indices(new_row_count);
    for (uint32_t old_idx = 0; old_idx < old_count; ++old_idx) {
      uint32_t new_idx = delete_result.old_to_new[old_idx];
      if (new_idx != kNullUint32) {
        new_source_indices[new_idx] = data.source_indices[old_idx];
      }
    }

    data.source_indices = std::move(new_source_indices);
    data.parent_indices = std::move(delete_result.new_parent_indices);
    // passthrough_columns unchanged (accessed via source_indices)
    return base::OkStatus();
  }

  // Execute a merge operation in place.
  // Aggregates passthrough columns and resets source_indices to iota.
  static base::Status ExecuteMerge(TreeData& data,
                                   const TreeMergeSiblingsOp& op,
                                   StringPool* /*pool*/) {
    if (data.passthrough_columns.empty()) {
      return base::ErrStatus(
          "tree_merge_siblings requires passthrough columns");
    }

    // Materialize columns via source_indices
    auto materialized = GatherAllPassthroughColumns(data.passthrough_columns,
                                                    data.source_indices);

    // Find and validate key/order columns
    const auto* order_col = FindColumnByName(materialized, op.order_column);
    if (!order_col || !order_col->IsInt64()) {
      return base::ErrStatus("Order column '%s' not found or not integer",
                             op.order_column.c_str());
    }
    ASSIGN_OR_RETURN(const auto* key_col,
                     FindColumnOrError(materialized, op.key_column, "Merge"));

    auto order_values = GatherValues(order_col->AsInt64(), data.source_indices);

    MergeSiblingsResult merge_result;
    if (key_col->IsString()) {
      auto key_values = GatherValues(key_col->AsString(), data.source_indices);
      ASSIGN_OR_RETURN(merge_result, (MergeSiblings<StringPool::Id, int64_t>(
                                         data.parent_indices, key_values,
                                         order_values, op.mode)));
    } else if (key_col->IsInt64()) {
      auto key_values = GatherValues(key_col->AsInt64(), data.source_indices);
      ASSIGN_OR_RETURN(merge_result, (MergeSiblings<int64_t, int64_t>(
                                         data.parent_indices, key_values,
                                         order_values, op.mode)));
    } else {
      return base::ErrStatus("Key column must be string or integer type");
    }

    // Update data
    uint32_t new_count = merge_result.merged_sources.size();
    data.parent_indices = std::move(merge_result.new_parent_indices);
    data.passthrough_columns = AggregatePassthroughColumns(
        materialized, merge_result.merged_sources, op.aggregations);
    NullOutOriginalIdColumns(data.passthrough_columns, new_count);
    data.source_indices.resize(new_count);
    std::iota(data.source_indices.begin(), data.source_indices.end(), 0);
    return base::OkStatus();
  }

  // Execute a propagate-up operation.
  // Adds a new column with aggregated values from leaves to root.
  static base::Status ExecutePropagateUp(TreeData& data,
                                         const TreePropagateUpOp& op) {
    ASSIGN_OR_RETURN(const auto* src_col,
                     FindColumnOrError(data.passthrough_columns,
                                       op.spec.in_column, "PropagateUp"));
    if (src_col->IsString()) {
      return base::ErrStatus(
          "PropagateUp: string columns not supported for aggregation");
    }

    TreeData temp_data(data.parent_indices,
                       GatherPassthroughColumn(*src_col, data.source_indices));
    ASSIGN_OR_RETURN(auto result, PropagateUp(temp_data, op.spec));
    data.passthrough_columns.push_back(std::move(result.out_column));
    return base::OkStatus();
  }

  // Execute a propagate-down operation.
  // Adds a new column with propagated values from root to leaves.
  static base::Status ExecutePropagateDown(TreeData& data,
                                           const TreePropagateDownOp& op) {
    ASSIGN_OR_RETURN(const auto* src_col,
                     FindColumnOrError(data.passthrough_columns,
                                       op.spec.in_column, "PropagateDown"));
    if (src_col->IsString()) {
      return base::ErrStatus(
          "PropagateDown: string columns not supported for aggregation");
    }

    TreeData temp_data(data.parent_indices,
                       GatherPassthroughColumn(*src_col, data.source_indices));
    ASSIGN_OR_RETURN(auto result, PropagateDown(temp_data, op.spec));
    data.passthrough_columns.push_back(std::move(result.out_column));
    return base::OkStatus();
  }

  // Execute an invert operation.
  // Inverts the tree (leaves become roots) and merges by key.
  static base::Status ExecuteInvert(TreeData& data,
                                    const TreeInvertOp& op,
                                    StringPool* /*pool*/) {
    if (data.passthrough_columns.empty()) {
      return base::ErrStatus("tree_invert requires passthrough columns");
    }

    // Materialize columns via source_indices
    auto materialized = GatherAllPassthroughColumns(data.passthrough_columns,
                                                    data.source_indices);

    // Find and validate order column (must be int64)
    const auto* order_col = FindColumnByName(materialized, op.order_column);
    if (!order_col || !order_col->IsInt64()) {
      return base::ErrStatus("Order column '%s' not found or not integer",
                             op.order_column.c_str());
    }
    ASSIGN_OR_RETURN(const auto* key_col,
                     FindColumnOrError(materialized, op.key_column, "Invert"));

    auto order_values = GatherValues(order_col->AsInt64(), data.source_indices);

    InvertAndMergeResult invert_result;
    if (key_col->IsString()) {
      auto key_values = GatherValues(key_col->AsString(), data.source_indices);
      ASSIGN_OR_RETURN(invert_result,
                       (InvertAndMerge<StringPool::Id, int64_t>(
                           data.parent_indices, key_values, order_values)));
    } else if (key_col->IsInt64()) {
      auto key_values = GatherValues(key_col->AsInt64(), data.source_indices);
      ASSIGN_OR_RETURN(invert_result,
                       (InvertAndMerge<int64_t, int64_t>(
                           data.parent_indices, key_values, order_values)));
    } else {
      return base::ErrStatus("Key column must be string or integer type");
    }

    // Update data
    auto new_count = invert_result.merged_sources.size();
    data.parent_indices = std::move(invert_result.new_parent_indices);
    data.passthrough_columns = AggregatePassthroughColumns(
        materialized, invert_result.merged_sources, op.aggregations);
    NullOutOriginalIdColumns(data.passthrough_columns, new_count);
    data.source_indices.resize(new_count);
    std::iota(data.source_indices.begin(), data.source_indices.end(), 0);

    return base::OkStatus();
  }

  PERFETTO_TEMPLATED_USED static void Step(sqlite3_context* ctx,
                                           int argc,
                                           sqlite3_value** argv) {
    SQLITE_RETURN_IF_ERROR(ctx, sqlite::utils::CheckExactArgTypes(
                                    kName, argc, argv, {PointerArg<Tree>()}));
    auto* tree = GetPointer<Tree>(argv[0]);

    StringPool* pool = GetUserData(ctx);

    // Get mutable access to tree data (copy-on-write if shared)
    std::shared_ptr<TreeData> data_ptr = tree->data;
    if (data_ptr.use_count() > 1) {
      data_ptr = std::make_shared<TreeData>(*data_ptr);
    }

    // Execute pending operations in place
    for (const auto& op_variant : tree->pending_ops) {
      if (const auto* merge_op =
              std::get_if<TreeMergeSiblingsOp>(&op_variant)) {
        SQLITE_RETURN_IF_ERROR(ctx, ExecuteMerge(*data_ptr, *merge_op, pool));
      } else if (const auto* delete_op =
                     std::get_if<TreeDeleteNodeOp>(&op_variant)) {
        SQLITE_RETURN_IF_ERROR(ctx, ExecuteDelete(*data_ptr, *delete_op, pool));
      } else if (const auto* propagate_up_op =
                     std::get_if<TreePropagateUpOp>(&op_variant)) {
        SQLITE_RETURN_IF_ERROR(ctx,
                               ExecutePropagateUp(*data_ptr, *propagate_up_op));
      } else if (const auto* propagate_down_op =
                     std::get_if<TreePropagateDownOp>(&op_variant)) {
        SQLITE_RETURN_IF_ERROR(
            ctx, ExecutePropagateDown(*data_ptr, *propagate_down_op));
      } else if (const auto* invert_op =
                     std::get_if<TreeInvertOp>(&op_variant)) {
        SQLITE_RETURN_IF_ERROR(ctx, ExecuteInvert(*data_ptr, *invert_op, pool));
      }
    }

    const auto& data = *data_ptr;

    // Compute depths from parent_indices
    std::vector<uint32_t> depths = ComputeDepths(data.parent_indices);

    // Build output column names: structural columns first, then passthrough
    std::vector<std::string> column_names;
    column_names.emplace_back(Tree::kNodeIdCol);
    column_names.emplace_back(Tree::kParentIdCol);
    column_names.emplace_back(Tree::kDepthCol);

    // Add passthrough column names
    for (const auto& col : data.passthrough_columns) {
      column_names.push_back(col.name);
    }

    // Build output dataframe using column-by-column bulk push
    using ColType = dataframe::AdhocDataframeBuilder::ColumnType;
    std::vector<ColType> col_types;
    col_types.push_back(ColType::kInt64);  // __node_id
    col_types.push_back(ColType::kInt64);  // __parent_id
    col_types.push_back(ColType::kInt64);  // __depth
    auto pt_col_types = GetColumnTypes(data.passthrough_columns);
    col_types.insert(col_types.end(), pt_col_types.begin(), pt_col_types.end());

    dataframe::AdhocDataframeBuilder builder(column_names, pool, col_types);

    const auto n = static_cast<uint32_t>(data.parent_indices.size());
    uint32_t col = 0;

    // __node_id: row index (0, 1, 2, ...)
    builder.PushIotaUnchecked(col++, n);

    // __parent_id: parent's row index (null for roots)
    builder.PushSpanAsInt64WithSentinelUnchecked(
        col++, base::MakeSpan(data.parent_indices), kNullUint32);

    // __depth (never null)
    builder.PushSpanAsInt64Unchecked(col++, base::MakeSpan(depths));

    // Passthrough columns: gather via source_indices (avoids intermediate copy)
    PushAllGatheredColumns(builder, col, data.passthrough_columns,
                           base::MakeSpan(data.source_indices));

    SQLITE_ASSIGN_OR_RETURN(ctx, auto df, std::move(builder).Build());
    return sqlite::result::UniquePointer(
        ctx, std::make_unique<dataframe::Dataframe>(std::move(df)), "TABLE");
  }
};

}  // namespace

base::Status TreePlugin::Register(PluginContext& ctx) {
  // Helper functions
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeKey>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeOrder>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeAgg>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeMergeStrategy>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeDeleteSpecFn>(ctx.pool()));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreePropagateSpecFn>(nullptr));

  // Tree construction aggregate (needs pool as user data for string interning)
  RETURN_IF_ERROR(ctx.RegisterAggregateFunction<TreeFromParentAgg>(ctx.pool()));

  // Tree operation functions
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeMergeSiblings>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeDeleteNode>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreePropagateUp>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreePropagateDown>(nullptr));
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeInvert>(nullptr));

  // Tree emit function (needs pool for building output dataframe)
  RETURN_IF_ERROR(ctx.RegisterFunction<TreeEmit>(ctx.pool()));

  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::plugins::tree
