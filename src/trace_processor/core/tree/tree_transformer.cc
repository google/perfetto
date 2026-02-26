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

#include "src/trace_processor/core/tree/tree_transformer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/tree_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/cursor.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/dataframe_transformer.h"
#include "src/trace_processor/core/dataframe/query_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/interpreter/bytecode_builder.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::tree {
namespace {

struct IdCallback : core::dataframe::CellCallback {
  void OnCell(int64_t id) {
    id_value = id;
    type_ok = true;
  }
  void OnCell(double) { type_ok = false; }
  void OnCell(NullTermStringView) { type_ok = false; }
  void OnCell(std::nullptr_t) {
    id_value = std::nullopt;
    type_ok = true;
  }
  void OnCell(uint32_t id) {
    id_value = id;
    type_ok = true;
  }
  void OnCell(int32_t id) {
    id_value = id;
    type_ok = true;
  }
  std::optional<int64_t> id_value;
  bool type_ok = false;
};

// Builds a mapping from ID values to row indices.
// Returns an error if IDs are non-integer or null.
base::StatusOr<base::FlatHashMap<int64_t, uint32_t>> BuildIdToRowMap(
    const dataframe::Dataframe& df) {
  base::FlatHashMap<int64_t, uint32_t> id_to_row;
  IdCallback id_cb;
  for (uint32_t row = 0; row < df.row_count(); ++row) {
    df.GetCell(row, 0, id_cb);
    if (PERFETTO_UNLIKELY(!id_cb.type_ok)) {
      return base::ErrStatus("ID column has non-integer values");
    }
    if (PERFETTO_UNLIKELY(!id_cb.id_value.has_value())) {
      return base::ErrStatus("ID column has null values");
    }
    id_to_row[*id_cb.id_value] = row;
  }
  return std::move(id_to_row);
}

// Builds normalized parent storage where parent IDs are converted to row
// indices. Root nodes (null parent_id) get UINT32_MAX.
base::StatusOr<Slab<uint32_t>> BuildNormalizedParentStorage(
    const dataframe::Dataframe& df,
    const base::FlatHashMap<int64_t, uint32_t>& id_to_row) {
  uint32_t row_count = df.row_count();
  auto normalized_parent = Slab<uint32_t>::Alloc(row_count);
  IdCallback id_cb;
  for (uint32_t row = 0; row < row_count; ++row) {
    df.GetCell(row, 1, id_cb);  // Parent ID is column 1
    if (PERFETTO_UNLIKELY(!id_cb.type_ok)) {
      return base::ErrStatus("Parent ID column has non-integer values");
    }
    if (id_cb.id_value.has_value()) {
      auto* parent_row = id_to_row.Find(*id_cb.id_value);
      if (!parent_row) {
        return base::ErrStatus("Parent ID not found in ID column");
      }
      normalized_parent[row] = *parent_row;
    } else {
      normalized_parent[row] = kNullParent;
    }
  }
  return std::move(normalized_parent);
}

// Creates an AdhocDataframeBuilder configured for tree columns.
dataframe::AdhocDataframeBuilder MakeTreeColumnBuilder(StringPool* pool) {
  return dataframe::AdhocDataframeBuilder(
      {"_tree_id", "_tree_parent_id"}, pool,
      dataframe::AdhocDataframeBuilder::Options{
          {}, dataframe::NullabilityType::kDenseNull});
}

// Builds tree columns (_tree_id, _tree_parent_id) from parent data.
// parent_data[i] contains the parent row index for row i, or kNullParent for
// roots.
base::StatusOr<dataframe::Dataframe> BuildTreeColumns(
    const uint32_t* parent_data,
    uint32_t count,
    StringPool* pool) {
  auto builder = MakeTreeColumnBuilder(pool);
  for (uint32_t i = 0; i < count; ++i) {
    builder.PushNonNull(0, i);
    if (parent_data[i] == kNullParent) {
      builder.PushNull(1);
    } else {
      builder.PushNonNull(1, parent_data[i]);
    }
  }
  return std::move(builder).Build();
}

// Filter value fetcher for tree operations that returns stored SqlValues.
struct TreeValueFetcher : core::ValueFetcher {
  static const Type kInt64 = static_cast<Type>(SqlValue::Type::kLong);
  static const Type kDouble = static_cast<Type>(SqlValue::Type::kDouble);
  static const Type kString = static_cast<Type>(SqlValue::Type::kString);
  static const Type kNull = static_cast<Type>(SqlValue::Type::kNull);

  explicit TreeValueFetcher(const SqlValue* v) : values(v) {}

  Type GetValueType(uint32_t i) const {
    return static_cast<Type>(values[i].type);
  }
  int64_t GetInt64Value(uint32_t i) const { return values[i].AsLong(); }
  double GetDoubleValue(uint32_t i) const { return values[i].AsDouble(); }
  const char* GetStringValue(uint32_t i) const { return values[i].AsString(); }
  static bool IteratorInit(uint32_t) { return false; }
  static bool IteratorNext(uint32_t) { return false; }

  const SqlValue* values = nullptr;
};

}  // namespace

// =============================================================================
// Constructor
// =============================================================================

TreeTransformer::TreeTransformer(dataframe::Dataframe df, StringPool* pool)
    : df_(std::move(df)), pool_(pool) {}

// =============================================================================
// Public methods
// =============================================================================

base::Status TreeTransformer::FilterTree(
    std::vector<dataframe::FilterSpec> specs,
    std::vector<SqlValue> values) {
  uint32_t n = df_.row_count();
  if (n == 0 || specs.empty()) {
    return base::OkStatus();
  }

  // Initialize tree structure on first FilterTree call.
  // This allocates all scratch buffers once for reuse across filters.
  if (!filter_scratch_.has_value()) {
    InitializeTreeStructure(n);
  }

  // Store filter values and set value_index in specs.
  for (size_t i = 0; i < specs.size(); ++i) {
    specs[i].value_index = static_cast<uint32_t>(filter_values_.size());
    filter_values_.push_back(values[i]);
  }

  // Rebuild P2C from current C2P if stale.
  EnsureParentToChildStructure();

  // Build filter bitvector from specs.
  ASSIGN_OR_RETURN(auto keep_bv, dt_->Filter(specs));

  // Emit the tree filter operation (updates C2P, invalidates P2C).
  EmitFilterTreeBytecode(keep_bv);
  p2c_stale_ = true;

  // Gather all columns at the surviving row positions.
  dt_->GatherAllColumns(original_rows_span_);

  // Re-emit propagation for any previously propagated columns,
  // since their source data was just re-gathered.
  if (!propagated_columns_.empty()) {
    EnsureParentToChildStructure();
    for (auto& pc : propagated_columns_) {
      auto result =
          EmitPropagateDownBytecode(pc.source_col_idx, pc.storage_type,
                                    pc.nullability, pc.combine_op_tag, n);
      pc.storage_reg = result.mutable_reg;
      pc.slab_reg = result.slab_reg;
      pc.null_bv_reg = result.null_bv_reg;
    }
  }

  return base::OkStatus();
}

base::Status TreeTransformer::PropagateDown(const std::string& col_name,
                                            const std::string& combine_op,
                                            const std::string& output_name) {
  uint32_t n = df_.row_count();
  if (n == 0) {
    return base::OkStatus();
  }

  // Initialize tree structure if this is the first tree operation.
  if (!filter_scratch_.has_value()) {
    InitializeTreeStructure(n);
  }

  // Rebuild P2C from current C2P if stale.
  EnsureParentToChildStructure();

  // Find the source column by name.
  auto col_idx = dt_->FindColumn(col_name);
  if (!col_idx) {
    return base::ErrStatus("PropagateDown: unknown column '%s'",
                           col_name.c_str());
  }

  dataframe::StorageType storage_type = dt_->GetStorageType(*col_idx);
  if (storage_type.Is<Id>()) {
    return base::ErrStatus("PropagateDown: cannot propagate Id columns");
  }

  // Parse the combine operation.
  uint32_t op_tag;
  if (combine_op == "sum") {
    if (storage_type.Is<String>()) {
      return base::ErrStatus("PropagateDown: 'sum' not supported for strings");
    }
    op_tag = interpreter::CombineOp(interpreter::SumOp{}).index();
  } else if (combine_op == "min") {
    op_tag = interpreter::CombineOp(interpreter::MinOp{}).index();
  } else if (combine_op == "max") {
    op_tag = interpreter::CombineOp(interpreter::MaxOp{}).index();
  } else if (combine_op == "first") {
    op_tag = interpreter::CombineOp(interpreter::FirstOp{}).index();
  } else if (combine_op == "last") {
    op_tag = interpreter::CombineOp(interpreter::LastOp{}).index();
  } else {
    return base::ErrStatus("PropagateDown: unknown combine op '%s'",
                           combine_op.c_str());
  }

  // Emit CopyStorageToSlab + PropagateDown bytecodes.
  Nullability nullability = dt_->GetNullability(*col_idx);
  auto result =
      EmitPropagateDownBytecode(*col_idx, storage_type, nullability, op_tag, n);

  // Add the output column. Use DenseNull if source is nullable (propagation
  // densifies sparse nulls), NonNull if source is non-null.
  Nullability out_null = nullability.Is<NonNull>() ? Nullability(NonNull{})
                                                   : Nullability(DenseNull{});
  dt_->AddColumn(output_name, storage_type, out_null, result.mutable_reg);

  // Track for re-propagation after future FilterTree calls.
  propagated_columns_.push_back(PropagatedColumn{
      output_name, *col_idx, op_tag, storage_type, nullability,
      result.mutable_reg, result.slab_reg, result.null_bv_reg});

  return base::OkStatus();
}

base::StatusOr<dataframe::Dataframe> TreeTransformer::ToDataframe() && {
  using StoragePtr = interpreter::StoragePtr;
  using SpanHandle = interpreter::ReadHandle<Span<uint32_t>>;

  ASSIGN_OR_RETURN(auto id_to_row, BuildIdToRowMap(df_));
  ASSIGN_OR_RETURN(auto normalized_parent,
                   BuildNormalizedParentStorage(df_, id_to_row));
  if (df_.row_count() == 0 || !filter_scratch_.has_value()) {
    ASSIGN_OR_RETURN(auto tree_cols, BuildTreeColumns(normalized_parent.begin(),
                                                      df_.row_count(), pool_));
    return dataframe::Dataframe::HorizontalConcat(std::move(tree_cols),
                                                  std::move(df_));
  }

  interpreter::Interpreter<TreeValueFetcher> interp;
  interp.Initialize(builder_->bytecode(), builder_->register_count(), pool_);
  interp.SetRegisterValue(
      interpreter::WriteHandle<StoragePtr>(parent_storage_reg_.index),
      StoragePtr{normalized_parent.begin(), nullptr,
                 dataframe::StorageType(Uint32{})});

  for (const auto& init : dt_->register_inits()) {
    auto val = dataframe::QueryPlanImpl::GetRegisterInitValue(init, df_);
    interp.SetRegisterValue(interpreter::HandleBase{init.dest_register},
                            std::move(val));
  }

  TreeValueFetcher fetcher(filter_values_.data());
  interp.Execute(fetcher);

  // Get final parent and original_rows spans.
  const auto* parent_span =
      interp.GetRegisterValue(SpanHandle(parent_span_.index));
  const auto* original_rows_span =
      interp.GetRegisterValue(SpanHandle(original_rows_span_.index));
  if (!parent_span || !original_rows_span) {
    return base::ErrStatus("Failed to get tree spans from interpreter");
  }

  // Build tree columns and combine with data.
  auto final_count = static_cast<uint32_t>(parent_span->size());
  ASSIGN_OR_RETURN(auto tree_cols,
                   BuildTreeColumns(parent_span->b, final_count, pool_));

  auto filtered_df =
      std::move(df_).SelectRows(original_rows_span->b, final_count);

  // If no propagated columns, just return tree + filtered df.
  if (propagated_columns_.empty()) {
    return dataframe::Dataframe::HorizontalConcat(std::move(tree_cols),
                                                  std::move(filtered_df));
  }

  // Build propagated columns dataframe.
  std::vector<std::string> prop_col_names;
  for (const auto& pc : propagated_columns_) {
    prop_col_names.push_back(pc.name);
  }
  dataframe::AdhocDataframeBuilder prop_builder(
      prop_col_names, pool_,
      dataframe::AdhocDataframeBuilder::Options{
          {}, dataframe::NullabilityType::kDenseNull});

  // Pre-fetch storage pointers and null bitvectors for each propagated column.
  struct ResolvedColumn {
    const void* data;
    const BitVector* null_bv;
  };
  std::vector<ResolvedColumn> resolved(propagated_columns_.size());
  for (uint32_t ci = 0; ci < propagated_columns_.size(); ++ci) {
    const auto& pc = propagated_columns_[ci];
    const StoragePtr* sp = interp.GetRegisterValue(
        interpreter::ReadHandle<StoragePtr>(pc.storage_reg.index));
    resolved[ci].data = (sp && sp->ptr) ? sp->ptr : nullptr;
    if (!pc.nullability.Is<NonNull>()) {
      resolved[ci].null_bv = interp.GetRegisterValue(
          interpreter::ReadHandle<BitVector>(pc.null_bv_reg.index));
    } else {
      resolved[ci].null_bv = nullptr;
    }
  }

  for (uint32_t row = 0; row < final_count; ++row) {
    for (uint32_t ci = 0; ci < propagated_columns_.size(); ++ci) {
      const auto& pc = propagated_columns_[ci];
      const auto& rc = resolved[ci];
      if (!rc.data) {
        prop_builder.PushNull(ci);
        continue;
      }
      // Check null bitvector if column is nullable.
      if (rc.null_bv && !rc.null_bv->is_set(row)) {
        prop_builder.PushNull(ci);
        continue;
      }
      if (pc.storage_type.Is<Uint32>()) {
        prop_builder.PushNonNull(ci,
                                 static_cast<const uint32_t*>(rc.data)[row]);
      } else if (pc.storage_type.Is<Int32>()) {
        prop_builder.PushNonNull(
            ci, int64_t{static_cast<const int32_t*>(rc.data)[row]});
      } else if (pc.storage_type.Is<Int64>()) {
        prop_builder.PushNonNull(ci, static_cast<const int64_t*>(rc.data)[row]);
      } else if (pc.storage_type.Is<Double>()) {
        prop_builder.PushNonNull(ci, static_cast<const double*>(rc.data)[row]);
      } else if (pc.storage_type.Is<String>()) {
        prop_builder.PushNonNull(
            ci, static_cast<const StringPool::Id*>(rc.data)[row]);
      }
    }
  }
  ASSIGN_OR_RETURN(auto prop_df, std::move(prop_builder).Build());

  // Concat: tree_cols + filtered_df + propagated_cols
  ASSIGN_OR_RETURN(auto base_df,
                   dataframe::Dataframe::HorizontalConcat(
                       std::move(tree_cols), std::move(filtered_df)));
  return dataframe::Dataframe::HorizontalConcat(std::move(base_df),
                                                std::move(prop_df));
}

// =============================================================================
// Private methods
// =============================================================================

void TreeTransformer::InitializeTreeStructure(uint32_t row_count) {
  using MakeC2P = interpreter::MakeChildToParentTreeStructure;

  // Create the DataframeTransformer from the current df.
  dt_.emplace(*builder_, df_);

  // Allocate persistent scratch for parent and original_rows spans.
  auto parent_scratch = builder_->AllocateScratch(row_count);
  auto orig_scratch = builder_->AllocateScratch(row_count);

  parent_span_ = parent_scratch.span;
  original_rows_span_ = orig_scratch.span;

  // Allocate register for parent storage pointer.
  parent_storage_reg_ = builder_->AllocateRegister<interpreter::StoragePtr>();

  // Emit bytecode to initialize child-to-parent structure from parent storage.
  auto& op = builder_->AddOpcode<MakeC2P>(interpreter::Index<MakeC2P>());
  op.arg<MakeC2P::parent_id_storage_register>() = parent_storage_reg_;
  op.arg<MakeC2P::row_count>() = row_count;
  op.arg<MakeC2P::parent_span_register>() = parent_span_;
  op.arg<MakeC2P::original_rows_span_register>() = original_rows_span_;

  // Allocate all scratch buffers once for reuse across FilterTree() calls.
  // This avoids emitting AllocateIndices bytecode for each filter operation.
  filter_scratch_ = FilterScratch{
      builder_->AllocateScratch(row_count * 2),
      builder_->AllocateScratch(row_count),
      builder_->AllocateScratch(row_count),
      builder_->AllocateScratch(row_count + 1),
      builder_->AllocateScratch(row_count),
      builder_->AllocateScratch(row_count),
  };
}

void TreeTransformer::EnsureParentToChildStructure() {
  if (!p2c_stale_) {
    return;
  }
  using MakeP2C = interpreter::MakeParentToChildTreeStructure;

  auto& op = builder_->AddOpcode<MakeP2C>(interpreter::Index<MakeP2C>());
  op.arg<MakeP2C::parent_span_register>() = parent_span_;
  op.arg<MakeP2C::scratch_register>() = filter_scratch_->p2c_scratch.span;
  op.arg<MakeP2C::offsets_register>() = filter_scratch_->p2c_offsets.span;
  op.arg<MakeP2C::children_register>() = filter_scratch_->p2c_children.span;
  op.arg<MakeP2C::roots_register>() = filter_scratch_->p2c_roots.span;
  p2c_stale_ = false;
}

TreeTransformer::PropagateResult TreeTransformer::EmitPropagateDownBytecode(
    uint32_t col_idx,
    dataframe::StorageType storage_type,
    Nullability nullability,
    uint32_t combine_op_tag,
    uint32_t max_rows) {
  auto source_reg = dt_->StorageRegisterFor(col_idx);
  auto null_bv_src = dt_->NullBitvectorRegisterFor(col_idx);

  auto copy_slab_reg = builder_->AllocateRegister<Slab<uint8_t>>();
  auto mutable_reg = interpreter::RwHandle<interpreter::StoragePtr>(
      builder_->AllocateRegister<interpreter::StoragePtr>());
  auto null_bv_reg = builder_->AllocateRegister<BitVector>();

  // Emit CopyStorageToSlab (handles all nullability types).
  {
    using CopySlab = interpreter::CopyStorageToSlab;
    auto& op = builder_->AddOpcode<CopySlab>(interpreter::Index<CopySlab>());
    op.arg<CopySlab::source_storage_register>() = source_reg;
    op.arg<CopySlab::row_count_span_register>() = original_rows_span_;
    op.arg<CopySlab::source_null_bv_register>() = null_bv_src;
    op.arg<CopySlab::source_nullability>() = nullability.index();
    op.arg<CopySlab::dest_slab_register>() = copy_slab_reg;
    op.arg<CopySlab::dest_storage_register>() = mutable_reg;
    op.arg<CopySlab::dest_null_bv_register>() = null_bv_reg;
  }

  // Allocate scratch for BFS queue.
  auto pd_scratch = builder_->AllocateScratch(max_rows);

  // Emit PropagateDown bytecode (null-aware when nullable).
  auto type_for_dispatch =
      *storage_type.TryDowncast<interpreter::NonIdStorageType>();
  using PD = interpreter::PropagateDownBase;
  auto& op = builder_->AddOpcode<PD>(
      interpreter::Index<interpreter::PropagateDown>(type_for_dispatch));
  op.arg<PD::offsets_register>() = filter_scratch_->p2c_offsets.span;
  op.arg<PD::children_register>() = filter_scratch_->p2c_children.span;
  op.arg<PD::roots_register>() = filter_scratch_->p2c_roots.span;
  op.arg<PD::update_storage_register>() = mutable_reg;
  op.arg<PD::scratch_register>() = pd_scratch.span;
  op.arg<PD::combine_op>() = combine_op_tag;
  op.arg<PD::null_bv_register>() = null_bv_reg;
  op.arg<PD::source_nullability>() = nullability.index();

  return {mutable_reg, copy_slab_reg, null_bv_reg};
}

void TreeTransformer::EmitFilterTreeBytecode(
    interpreter::RwHandle<BitVector> keep_bv) {
  using Filter = interpreter::FilterTree;

  auto& op = builder_->AddOpcode<Filter>(interpreter::Index<Filter>());
  op.arg<Filter::offsets_register>() = filter_scratch_->p2c_offsets.span;
  op.arg<Filter::children_register>() = filter_scratch_->p2c_children.span;
  op.arg<Filter::roots_register>() = filter_scratch_->p2c_roots.span;
  op.arg<Filter::keep_bitvector_register>() = keep_bv;
  op.arg<Filter::parent_span_register>() = parent_span_;
  op.arg<Filter::original_rows_span_register>() = original_rows_span_;
  op.arg<Filter::scratch1_register>() = filter_scratch_->scratch1.span;
  op.arg<Filter::scratch2_register>() = filter_scratch_->scratch2.span;
}

}  // namespace perfetto::trace_processor::core::tree
