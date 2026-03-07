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
#include <cstring>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/tree_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/dataframe_register_cache.h"
#include "src/trace_processor/core/dataframe/query_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/interpreter/bytecode_builder.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/slab.h"

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

base::StatusOr<Slab<uint32_t>> BuildNormalizedParentStorage(
    const dataframe::Dataframe& df,
    const base::FlatHashMap<int64_t, uint32_t>& id_to_row) {
  uint32_t row_count = df.row_count();
  auto normalized_parent = Slab<uint32_t>::Alloc(row_count);
  IdCallback id_cb;
  for (uint32_t row = 0; row < row_count; ++row) {
    df.GetCell(row, 1, id_cb);
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

dataframe::AdhocDataframeBuilder MakeTreeColumnBuilder(StringPool* pool) {
  return dataframe::AdhocDataframeBuilder(
      {"_tree_id", "_tree_parent_id"}, pool,
      dataframe::AdhocDataframeBuilder::Options{
          {}, dataframe::NullabilityType::kDenseNull});
}

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

// Creates a TreeState with pre-allocated buffers and parent/original_rows
// initialized from the normalized parent storage.
std::unique_ptr<interpreter::TreeState> CreateTreeState(
    const uint32_t* parent_data,
    uint32_t row_count) {
  auto ts = std::make_unique<interpreter::TreeState>();
  ts->row_count = row_count;

  ts->parent = Slab<uint32_t>::Alloc(row_count);
  memcpy(ts->parent.begin(), parent_data, row_count * sizeof(uint32_t));

  ts->original_rows = Slab<uint32_t>::Alloc(row_count);
  std::iota(ts->original_rows.begin(), ts->original_rows.begin() + row_count,
            0u);

  ts->p2c_offsets = Slab<uint32_t>::Alloc(row_count + 1);
  ts->p2c_children = Slab<uint32_t>::Alloc(row_count);
  ts->p2c_roots = Slab<uint32_t>::Alloc(row_count);
  ts->p2c_valid = false;

  ts->scratch1 = Slab<uint32_t>::Alloc(static_cast<uint64_t>(row_count) * 2);
  ts->scratch2 = Slab<uint32_t>::Alloc(row_count);

  return ts;
}

}  // namespace

TreeTransformer::TreeTransformer(dataframe::Dataframe df, StringPool* pool)
    : df_(std::move(df)), pool_(pool) {}

base::Status TreeTransformer::FilterTree(
    std::vector<dataframe::FilterSpec> specs,
    std::vector<SqlValue> values) {
  if (df_.row_count() == 0 || specs.empty()) {
    return base::OkStatus();
  }

  // Accumulate filter specs and values with source_index tracking the
  // original position in filter_values_.
  for (size_t i = 0; i < specs.size(); ++i) {
    specs[i].source_index = static_cast<uint32_t>(filter_values_.size());
    filter_values_.push_back(values[i]);
    filter_specs_.push_back(specs[i]);
  }

  return base::OkStatus();
}

base::StatusOr<dataframe::Dataframe> TreeTransformer::ToDataframe() && {
  using TreeState = interpreter::TreeState;

  ASSIGN_OR_RETURN(auto id_to_row, BuildIdToRowMap(df_));
  ASSIGN_OR_RETURN(auto normalized_parent,
                   BuildNormalizedParentStorage(df_, id_to_row));

  uint32_t n = df_.row_count();
  if (n == 0 || filter_specs_.empty()) {
    ASSIGN_OR_RETURN(auto tree_cols,
                     BuildTreeColumns(normalized_parent.begin(), n, pool_));
    return dataframe::Dataframe::HorizontalConcat(std::move(tree_cols),
                                                  std::move(df_));
  }

  // Create bytecode builder and register cache.
  interpreter::BytecodeBuilder builder;
  dataframe::DataframeRegisterCache cache(builder);

  // Use FilterOnly to emit standard filter bytecodes.
  ASSIGN_OR_RETURN(auto filter_result, dataframe::QueryPlanBuilder::FilterOnly(
                                           builder, cache, n, df_.columns_,
                                           df_.indexes_, filter_specs_));

  // Allocate TreeState register and emit FilterTreeState bytecode.
  auto tree_state_reg = builder.AllocateRegister<std::unique_ptr<TreeState>>();
  {
    using F = interpreter::FilterTreeState;
    auto& op = builder.AddOpcode<F>(interpreter::Index<F>());
    op.arg<F::tree_state_register>() = tree_state_reg;
    op.arg<F::filtered_indices>() = filter_result.indices_reg;
  }

  // Reorder filter values to match the order assigned by Filter().
  std::vector<SqlValue> reordered(filter_result.filter_value_count);
  for (const auto& spec : filter_specs_) {
    if (spec.value_index.has_value()) {
      reordered[*spec.value_index] = filter_values_[spec.source_index];
    }
  }

  // Create TreeState.
  auto ts = CreateTreeState(normalized_parent.begin(), n);

  // Initialize interpreter.
  interpreter::Interpreter<TreeValueFetcher> interp;
  interp.Initialize(builder.bytecode(), builder.register_count(), pool_);

  // Set TreeState register.
  interp.SetRegisterValue(interpreter::WriteHandle<std::unique_ptr<TreeState>>(
                              tree_state_reg.index),
                          std::move(ts));

  // Initialize source column registers.
  for (const auto& init : filter_result.register_inits) {
    auto val = dataframe::QueryPlanImpl::GetRegisterInitValue(init, df_);
    interp.SetRegisterValue(interpreter::HandleBase{init.dest_register},
                            std::move(val));
  }

  // Execute all bytecodes.
  TreeValueFetcher fetcher(reordered.data());
  interp.Execute(fetcher);

  // Read results from TreeState.
  const auto* ts_result = interp.GetRegisterValue(
      interpreter::ReadHandle<std::unique_ptr<TreeState>>(
          tree_state_reg.index));
  if (!ts_result || !*ts_result) {
    return base::ErrStatus("Failed to get TreeState from interpreter");
  }
  const auto& final_ts = **ts_result;

  uint32_t final_count = final_ts.row_count;
  ASSIGN_OR_RETURN(auto tree_cols, BuildTreeColumns(final_ts.parent.begin(),
                                                    final_count, pool_));
  return dataframe::Dataframe::HorizontalConcat(
      std::move(tree_cols),
      std::move(df_).SelectRows(final_ts.original_rows.begin(), final_count));
}

}  // namespace perfetto::trace_processor::core::tree
