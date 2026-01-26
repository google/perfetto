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

#include "src/trace_processor/core/tree/tree_builder.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::tree {

namespace i = interpreter;

namespace {

// Column indices in the output dataframe.
constexpr uint32_t kTreeIdCol = 0;
constexpr uint32_t kTreeParentIdCol = 1;
constexpr uint32_t kFirstDataCol = 2;

std::optional<uint32_t> FindColumnIndex(const Tree& tree,
                                        const std::string& name) {
  if (!tree.columns) {
    return std::nullopt;
  }
  const auto& names = tree.columns->column_names();
  for (uint32_t idx = 0; idx < names.size(); ++idx) {
    if (names[idx] == name) {
      return idx;
    }
  }
  return std::nullopt;
}

// Creates an identity mapping [0, 1, 2, ..., size-1].
Slab<uint32_t> CreateIdentityMapping(uint32_t size) {
  Slab<uint32_t> result = Slab<uint32_t>::Alloc(size);
  std::iota(result.data(), result.data() + size, uint32_t{0});
  return result;
}

// Callback to copy cells from source dataframe to AdhocDataframeBuilder.
struct CellPusher : dataframe::CellCallback {
  dataframe::AdhocDataframeBuilder* builder;
  StringPool* pool;
  uint32_t col;

  void OnCell(int64_t v) const { builder->PushNonNull(col, v); }
  void OnCell(double v) const { builder->PushNonNull(col, v); }
  void OnCell(NullTermStringView v) const {
    builder->PushNonNull(col, pool->InternString(v));
  }
  void OnCell(std::nullptr_t) const { builder->PushNull(col); }
  void OnCell(uint32_t v) const { builder->PushNonNull(col, int64_t{v}); }
  void OnCell(int32_t v) const { builder->PushNonNull(col, int64_t{v}); }
};

}  // namespace

TreeTransformationBuilder::TreeTransformationBuilder(std::unique_ptr<Tree> tree)
    : base_(std::move(tree)) {}

bool TreeTransformationBuilder::Filter(const std::string& column_name,
                                       FilterOp op,
                                       const FilterValue& value) {
  auto col_idx = FindColumnIndex(*base_, column_name);
  if (!col_idx) {
    return false;
  }
  EnsureCsr();

  // Allocate register for the filter bitvector result.
  uint32_t filter_bv_reg = reg_state_.next_reg++;

  // TODO(lalitm): Generate bytecode to filter column *col_idx with op/value,
  // producing a bitvector in filter_bv_reg. For now this is a placeholder.
  (void)op;
  (void)value;

  // Add FilterTree bytecode to apply the filter.
  using FilterTreeOp = i::FilterTree;
  auto& bc = AddOpcode<FilterTreeOp>();
  bc.arg<FilterTreeOp::source_register>() =
      i::ReadHandle<i::TreeStructure::ParentToChild>{
          reg_state_.parent_to_child_reg};
  bc.arg<FilterTreeOp::filter_register>() =
      i::ReadHandle<const BitVector*>{filter_bv_reg};
  bc.arg<FilterTreeOp::update_register>() =
      i::RwHandle<i::TreeStructure::ChildToParent>{
          reg_state_.child_to_parent_reg};

  return true;
}

void TreeTransformationBuilder::EnsureCsr() {
  if (reg_state_.csr_valid) {
    return;
  }

  // Allocate registers for tree structures.
  reg_state_.child_to_parent_reg = reg_state_.next_reg++;
  reg_state_.parent_to_child_reg = reg_state_.next_reg++;

  // Add bytecode to build CSR (parent-to-child) from child-to-parent structure.
  using MakeCsrOp = i::MakeParentToChildTreeStructure;
  auto& bc = AddOpcode<MakeCsrOp>();
  bc.arg<MakeCsrOp::source_register>() =
      i::ReadHandle<i::TreeStructure::ChildToParent>{
          reg_state_.child_to_parent_reg};
  bc.arg<MakeCsrOp::update_register>() =
      i::RwHandle<i::TreeStructure::ParentToChild>{
          reg_state_.parent_to_child_reg};

  reg_state_.csr_valid = true;
}

std::unique_ptr<Tree> TreeTransformationBuilder::Build() && {
  // If no transformations were requested, return the base tree unchanged.
  if (bytecode_.empty()) {
    return std::move(base_);
  }

  auto tree_size = static_cast<uint32_t>(base_->size());
  Slab<uint32_t> original_rows = CreateIdentityMapping(tree_size);

  // Set up the initial ChildToParent structure from the tree's parents.
  i::TreeStructure::ChildToParent child_to_parent{
      Span<uint32_t>(base_->parents.data(), base_->parents.data() + tree_size),
      Span<uint32_t>(original_rows.data(), original_rows.data() + tree_size),
  };

  // Create interpreter, initialize, and execute bytecode.
  i::Interpreter<ErrorValueFetcher> interpreter;
  interpreter.Initialize(bytecode_, reg_state_.next_reg, nullptr);
  interpreter.SetRegisterValue({reg_state_.child_to_parent_reg},
                               child_to_parent);

  ErrorValueFetcher fetcher;
  interpreter.Execute(fetcher);

  // Read the resulting ChildToParent structure.
  const auto* result = interpreter.GetRegisterValue(
      i::ReadHandle<i::TreeStructure::ChildToParent>{
          reg_state_.child_to_parent_reg});
  if (!result) {
    // This shouldn't happen if bytecode was generated correctly.
    return std::move(base_);
  }

  // Build the new tree from the result.
  auto new_size = static_cast<uint32_t>(result->parents.size());
  auto new_tree = std::make_unique<Tree>();
  new_tree->parents = Slab<uint32_t>::Alloc(new_size);
  memcpy(new_tree->parents.data(), result->parents.b,
         new_size * sizeof(uint32_t));

  // TODO(lalitm): Handle columns - need to filter/reorder based on
  // result->original_rows. For now, just copy columns if sizes match.
  if (base_->columns && new_size == tree_size) {
    new_tree->columns = std::move(base_->columns);
  }
  return new_tree;
}

base::StatusOr<dataframe::Dataframe> TreeTransformationBuilder::BuildDataframe(
    StringPool* pool) && {
  return TreeToDataframe(std::move(*this).Build(), pool);
}

base::StatusOr<dataframe::Dataframe> TreeTransformationBuilder::TreeToDataframe(
    std::unique_ptr<Tree> tree,
    StringPool* pool) {
  using NullabilityType = dataframe::AdhocDataframeBuilder::NullabilityType;
  if (!tree->columns) {
    // Return empty dataframe if tree has no columns.
    // _tree_parent_id needs DenseNull for random access support.
    dataframe::AdhocDataframeBuilder builder(
        {"_tree_id", "_tree_parent_id"}, pool, {},
        {NullabilityType::kSparse, NullabilityType::kDense});
    return std::move(builder).Build();
  }

  // Build column names: _tree_id, _tree_parent_id, plus original columns
  // (excluding the _auto_id column which is the last one).
  const auto& src_names = tree->columns->column_names();
  uint32_t num_data_cols =
      src_names.empty() ? 0 : static_cast<uint32_t>(src_names.size() - 1);

  std::vector<std::string> col_names;
  col_names.reserve(kFirstDataCol + num_data_cols);
  col_names.emplace_back("_tree_id");
  col_names.emplace_back("_tree_parent_id");
  for (uint32_t col = 0; col < num_data_cols; ++col) {
    col_names.push_back(src_names[col]);
  }

  // TODO(lalitm): optimize this to directly reuse columns from tree->columns
  // instead of copying cell-by-cell. We should be able to share the underlying
  // Column shared_ptrs and just prepend the _tree_id and _tree_parent_id cols.
  //
  // Set up nullability types: _tree_parent_id needs DenseNull for random access
  // support. All other columns use default (Sparse).
  std::vector<NullabilityType> nullability_types(col_names.size(),
                                                 NullabilityType::kSparse);
  nullability_types[kTreeParentIdCol] = NullabilityType::kDense;
  dataframe::AdhocDataframeBuilder df_builder(col_names, pool, {},
                                              nullability_types);
  uint32_t row_count = tree->columns->row_count();
  for (uint32_t row = 0; row < row_count; ++row) {
    // Push _tree_id (row index).
    df_builder.PushNonNull(kTreeIdCol, int64_t{row});

    // Push _tree_parent_id (null for roots).
    if (tree->parents[row] == Tree::kNoParent) {
      df_builder.PushNull(kTreeParentIdCol);
    } else {
      df_builder.PushNonNull(kTreeParentIdCol, int64_t{tree->parents[row]});
    }

    // Copy data columns.
    for (uint32_t col = 0; col < num_data_cols; ++col) {
      CellPusher pusher{{}, &df_builder, pool, kFirstDataCol + col};
      tree->columns->GetCell(row, col, pusher);
    }
  }
  return std::move(df_builder).Build();
}

}  // namespace perfetto::trace_processor::core::tree
