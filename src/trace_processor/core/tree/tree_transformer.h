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

#ifndef SRC_TRACE_PROCESSOR_CORE_TREE_TREE_TRANSFORMER_H_
#define SRC_TRACE_PROCESSOR_CORE_TREE_TREE_TRANSFORMER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/dataframe_transformer.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/interpreter/bytecode_builder.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::tree {

// Transforms a tree-structured dataframe via a bunch of operations producing
// another dataframe.
//
// The tree structure is represented by the first two columns of the dataframe:
// - Column 0: node ID (the original ID, or _tree_id if already transformed)
// - Column 1: parent ID (parent node ID, or _tree_parent_id)
//
// Operations can be chained:
//   auto status = TreeTransformer(df, pool)
//       .FilterTree(filter_specs, values);
//   RETURN_IF_ERROR(status);
//   status = transformer.PropagateDown("col", "sum", "out_col");
//   RETURN_IF_ERROR(status);
//   auto result = std::move(transformer).ToDataframe();
//
// All operations emit bytecode immediately. ToDataframe() executes the
// accumulated bytecode and uses SelectRows to create the final dataframe.
//
// Uses a DataframeTransformer for low-level column/register plumbing
// (filtering, gathering, register allocation). TreeTransformer orchestrates
// the tree-specific bytecode (C2P/P2C structure, FilterTree, PropagateDown)
// and interpreter execution.
class TreeTransformer {
 public:
  explicit TreeTransformer(dataframe::Dataframe df, StringPool* pool);

  // Applies a filter to the tree, keeping only nodes matching the filter
  // and reparenting surviving children to their closest surviving ancestor.
  //
  // The filter specs are evaluated against the dataframe columns. Nodes
  // that pass the filter are kept; nodes that are filtered out have their
  // children reparented to the closest surviving ancestor.
  //
  // Multiple FilterTree calls can be chained; they are applied in order.
  // Bytecode is emitted immediately for each call.
  //
  // Returns Status (can fail if filter specs reference invalid columns).
  base::Status FilterTree(std::vector<dataframe::FilterSpec> specs,
                          std::vector<SqlValue> values);

  // Propagates values down the tree from roots to leaves using BFS.
  base::Status PropagateDown(const std::string& col_name,
                             const std::string& combine_op,
                             const std::string& output_name);

  // Returns the underlying dataframe (for accessing column metadata).
  const dataframe::Dataframe& df() const { return df_; }

  // Transforms the tree and returns the resulting dataframe.
  //
  // This always executes via a unified path:
  // - If no FilterTree calls: just builds tree structure columns
  // - If FilterTree was called: executes bytecode and filters rows
  base::StatusOr<dataframe::Dataframe> ToDataframe() &&;

 private:
  // Initializes tree structure on first FilterTree/PropagateDown call.
  // Allocates persistent parent/original_rows spans, all scratch buffers,
  // and emits MakeChildToParentTreeStructure bytecode.
  void InitializeTreeStructure(uint32_t row_count);

  // Emits MakeParentToChildTreeStructure bytecode if P2C is stale.
  // Uses pre-allocated scratch buffers stored as member variables.
  // Sets p2c_stale_ to false after emitting.
  void EnsureParentToChildStructure();

  // Emits FilterTree bytecode with all required registers.
  // Uses pre-allocated scratch buffers stored as member variables.
  void EmitFilterTreeBytecode(interpreter::RwHandle<BitVector> keep_bv);

  // Emits CopyStorageToSlab + PropagateDown bytecodes for a single column.
  // Returns the mutable storage register, slab register, and null bv register.
  struct PropagateResult {
    interpreter::RwHandle<interpreter::StoragePtr> mutable_reg;
    interpreter::RwHandle<Slab<uint8_t>> slab_reg;
    interpreter::RwHandle<BitVector> null_bv_reg;
  };
  PropagateResult EmitPropagateDownBytecode(uint32_t col_idx,
                                            dataframe::StorageType storage_type,
                                            Nullability nullability,
                                            uint32_t combine_op_tag,
                                            uint32_t max_rows);

  dataframe::Dataframe df_;
  StringPool* pool_;

  // Heap-allocated so its address is stable across moves of TreeTransformer.
  // DataframeTransformer holds a reference to this, so it must not relocate.
  std::unique_ptr<interpreter::BytecodeBuilder> builder_ =
      std::make_unique<interpreter::BytecodeBuilder>();

  // Low-level column/register plumbing. Lazily initialized on first
  // tree operation (when we know df has rows).
  std::optional<dataframe::DataframeTransformer> dt_;

  // Parent and original_rows span registers (set on first FilterTree call).
  interpreter::RwHandle<Span<uint32_t>> parent_span_;
  interpreter::RwHandle<Span<uint32_t>> original_rows_span_;

  // Register holding the normalized parent_id storage (set at execution time).
  interpreter::ReadHandle<interpreter::StoragePtr> parent_storage_reg_;

  // Alias for scratch register type.
  using Scratch = interpreter::BytecodeBuilder::ScratchRegisters;

  // Scratch buffers allocated once in InitializeTreeStructure() and reused
  // across all FilterTree() calls. This avoids emitting AllocateIndices
  // bytecode for each filter operation. Wrapped in optional to track
  // initialization state.
  struct FilterScratch {
    Scratch scratch1;
    Scratch scratch2;
    Scratch p2c_scratch;
    Scratch p2c_offsets;
    Scratch p2c_children;
    Scratch p2c_roots;
  };
  std::optional<FilterScratch> filter_scratch_;

  // Tracks whether the P2C (parent-to-child) structure is stale and needs
  // rebuilding. Operations that modify C2P (like FilterTree) set this to true.
  // EnsureParentToChildStructure() checks this and rebuilds P2C if needed.
  bool p2c_stale_ = true;

  // Propagated column tracking for re-propagation after FilterTree.
  struct PropagatedColumn {
    std::string name;
    uint32_t source_col_idx;
    uint32_t combine_op_tag;
    StorageType storage_type;
    Nullability nullability;
    interpreter::RwHandle<interpreter::StoragePtr> storage_reg;
    interpreter::RwHandle<Slab<uint8_t>> slab_reg;
    interpreter::RwHandle<BitVector> null_bv_reg;
  };
  std::vector<PropagatedColumn> propagated_columns_;

  // Accumulated filter values across all FilterTree() calls.
  std::vector<SqlValue> filter_values_;
};

}  // namespace perfetto::trace_processor::core::tree

namespace perfetto::trace_processor {

// Namespace alias for ergonomics.
namespace tree = core::tree;

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CORE_TREE_TREE_TRANSFORMER_H_
