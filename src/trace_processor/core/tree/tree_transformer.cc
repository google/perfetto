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

#include "perfetto/base/logging.h"
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

namespace i = interpreter;

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

std::unique_ptr<i::TreeState> CreateTreeState(const uint32_t* parent_data,
                                              uint32_t row_count) {
  auto ts = std::make_unique<i::TreeState>();
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

  ts->keep_bv = BitVector::CreateWithSize(row_count, false);

  return ts;
}

// Returns true if the column has sparse null storage (requires densification
// before the filter bytecodes can access storage with table indices).
bool IsSparseNull(const dataframe::Column& col) {
  return !col.null_storage.nullability().Is<NonNull>() &&
         !col.null_storage.nullability().Is<DenseNull>();
}

// Returns the size in bytes of one element for the given storage type.
uint32_t ElementSize(StorageType type) {
  switch (type.index()) {
    case StorageType::GetTypeIndex<Uint32>():
      return sizeof(uint32_t);
    case StorageType::GetTypeIndex<Int32>():
      return sizeof(int32_t);
    case StorageType::GetTypeIndex<Int64>():
      return sizeof(int64_t);
    case StorageType::GetTypeIndex<Double>():
      return sizeof(double);
    case StorageType::GetTypeIndex<String>():
      return sizeof(StringPool::Id);
    default:
      PERFETTO_FATAL("Unsupported storage type for densification");
  }
}

// Scatters sparse data into a dense buffer using the null bitvector.
// Templated to avoid per-row memcpy overhead for common element sizes.
template <typename T>
void ScatterSparse(const void* sparse_ptr,
                   const BitVector& bv,
                   uint8_t* dst,
                   uint32_t row_count) {
  const auto* src = static_cast<const T*>(sparse_ptr);
  auto* out = reinterpret_cast<T*>(dst);
  uint32_t sparse_idx = 0;
  for (uint32_t row = 0; row < row_count; ++row) {
    if (bv.is_set(row)) {
      out[row] = src[sparse_idx++];
    }
  }
}

// Converts a sparse null column's storage to dense format.
Slab<uint8_t> DensifySparseColumn(const dataframe::Column& col,
                                  uint32_t row_count) {
  uint32_t elem_size = ElementSize(col.storage.type());
  auto byte_count = static_cast<uint64_t>(row_count) * elem_size;
  auto dense = Slab<uint8_t>::Alloc(byte_count);
  memset(dense.begin(), 0, byte_count);

  const auto& bv = col.null_storage.GetNullBitVector();
  const void* sparse_ptr = nullptr;
  std::visit([&sparse_ptr](
                 const auto* p) { sparse_ptr = static_cast<const void*>(p); },
             col.storage.data());

  switch (elem_size) {
    case 4:
      ScatterSparse<uint32_t>(sparse_ptr, bv, dense.begin(), row_count);
      break;
    case 8:
      ScatterSparse<uint64_t>(sparse_ptr, bv, dense.begin(), row_count);
      break;
    default:
      PERFETTO_FATAL("Unexpected element size %u", elem_size);
  }
  return dense;
}

}  // namespace

TreeTransformer::TreeTransformer(dataframe::Dataframe df, StringPool* pool)
    : df_(std::move(df)),
      pool_(pool),
      builder_(std::make_unique<i::BytecodeBuilder>()) {
  uint32_t n = df_.row_count();
  if (n == 0) {
    return;
  }

  // Allocate registers used across all calls.
  auto range_reg = builder_->AllocateRegister<Range>();
  {
    using B = i::InitRange;
    auto& op = builder_->AddOpcode<B>(i::Index<B>());
    op.arg<B::size>() = n;
    op.arg<B::dest_register>() = range_reg;
  }
  auto slab_reg = builder_->AllocateRegister<Slab<uint32_t>>();
  auto span_reg = builder_->AllocateRegister<Span<uint32_t>>();
  span_reg_index_ = span_reg.index;
  {
    using B = i::AllocateIndices;
    auto& op = builder_->AddOpcode<B>(i::Index<B>());
    op.arg<B::size>() = n;
    op.arg<B::dest_slab_register>() = slab_reg;
    op.arg<B::dest_span_register>() = span_reg;
  }
  {
    using B = i::Iota;
    auto& op = builder_->AddOpcode<B>(i::Index<B>());
    op.arg<B::source_register>() = range_reg;
    op.arg<B::update_register>() = span_reg;
  }

  // TreeState register — shared across all FilterTree calls.
  auto ts_reg = builder_->AllocateRegister<std::unique_ptr<i::TreeState>>();
  tree_state_reg_index_ = ts_reg.index;
}

TreeTransformer::~TreeTransformer() = default;
TreeTransformer::TreeTransformer(TreeTransformer&&) noexcept = default;
TreeTransformer& TreeTransformer::operator=(TreeTransformer&&) noexcept =
    default;

base::Status TreeTransformer::FilterTree(
    std::vector<dataframe::FilterSpec> specs,
    std::vector<SqlValue> values) {
  uint32_t n = df_.row_count();
  if (n == 0 || specs.empty()) {
    return base::OkStatus();
  }

  has_filters_ = true;
  i::RwHandle<Span<uint32_t>> span_reg(span_reg_index_);

  for (size_t si = 0; si < specs.size(); ++si) {
    auto& spec = specs[si];
    const auto& col = *df_.columns_[spec.col];
    StorageType ct = col.storage.type();
    const BitVector* null_bv = col.null_storage.MaybeGetNullBitVector();

    // Handle null ops (IsNull/IsNotNull).
    if (auto null_op = spec.op.TryDowncast<i::NullOp>()) {
      spec.value_index = filter_value_count_++;
      filter_values_.push_back(values[si]);
      if (null_bv) {
        auto reg = builder_->AllocateRegister<i::NullBitvector>();
        using B = i::NullFilterBase;
        auto& bc = builder_->AddOpcode<B>(i::Index<i::NullFilter>(*null_op));
        bc.arg<B::null_bv_register>() = reg;
        bc.arg<B::update_register>() = span_reg;
        reg_inits_.push_back({RegInit::kNullBv, reg.index, spec.col});
      }
      continue;
    }

    // In is not supported for tree filters.
    PERFETTO_CHECK(!spec.op.Is<In>());

    auto non_null_op = spec.op.TryDowncast<i::NonNullOp>();
    PERFETTO_CHECK(non_null_op);

    // Cast filter value.
    auto value_reg = builder_->AllocateRegister<i::CastFilterValueResult>();
    {
      using B = i::CastFilterValueBase;
      auto& bc = builder_->AddOpcode<B>(i::Index<i::CastFilterValue>(ct));
      bc.arg<B::fval_handle>() = {filter_value_count_};
      bc.arg<B::write_register>() = value_reg;
      bc.arg<B::op>() = *non_null_op;
      spec.value_index = filter_value_count_++;
      filter_values_.push_back(values[si]);
    }

    // Prune null indices if column has nulls.
    if (null_bv) {
      auto reg = builder_->AllocateRegister<i::NullBitvector>();
      using B = i::NullFilter<IsNotNull>;
      auto& bc = builder_->AddOpcode<B>(i::Index<B>());
      bc.arg<B::null_bv_register>() = reg;
      bc.arg<B::update_register>() = span_reg;
      reg_inits_.push_back({RegInit::kNullBv, reg.index, spec.col});
    }

    // Allocate storage register and emit filter bytecode.
    auto storage_reg = builder_->AllocateRegister<i::StoragePtr>();
    reg_inits_.push_back({RegInit::kStorage, storage_reg.index, spec.col});

    if (ct.Is<String>()) {
      auto op = non_null_op->TryDowncast<i::StringOp>();
      PERFETTO_CHECK(op);
      using B = i::StringFilterBase;
      auto& bc = builder_->AddOpcode<B>(i::Index<i::StringFilter>(*op));
      bc.arg<B::storage_register>() = storage_reg;
      bc.arg<B::val_register>() = value_reg;
      bc.arg<B::source_register>() = span_reg;
      bc.arg<B::update_register>() = span_reg;
    } else {
      auto nst = ct.TryDowncast<i::NonStringType>();
      PERFETTO_CHECK(nst);
      auto op = non_null_op->TryDowncast<i::NonStringOp>();
      PERFETTO_CHECK(op);
      using B = i::NonStringFilterBase;
      auto& bc =
          builder_->AddOpcode<B>(i::Index<i::NonStringFilter>(*nst, *op));
      bc.arg<B::storage_register>() = storage_reg;
      bc.arg<B::val_register>() = value_reg;
      bc.arg<B::source_register>() = span_reg;
      bc.arg<B::update_register>() = span_reg;
    }
  }

  // Emit FilterTreeState: reparents, compacts tree + column data,
  // and resets the span to [0..new_row_count-1].
  i::RwHandle<std::unique_ptr<i::TreeState>> ts_reg(tree_state_reg_index_);
  {
    using F = i::FilterTreeState;
    auto& op = builder_->AddOpcode<F>(i::Index<F>());
    op.arg<F::tree_state_register>() = ts_reg;
    op.arg<F::indices_register>() = span_reg;
  }

  return base::OkStatus();
}

base::StatusOr<dataframe::Dataframe> TreeTransformer::ToDataframe() && {
  using TreeState = i::TreeState;

  ASSIGN_OR_RETURN(auto id_to_row, BuildIdToRowMap(df_));
  ASSIGN_OR_RETURN(auto normalized_parent,
                   BuildNormalizedParentStorage(df_, id_to_row));

  uint32_t n = df_.row_count();
  if (n == 0 || !has_filters_) {
    ASSIGN_OR_RETURN(auto tree_cols,
                     BuildTreeColumns(normalized_parent.begin(), n, pool_));
    return dataframe::Dataframe::HorizontalConcat(std::move(tree_cols),
                                                  std::move(df_));
  }

  // Create TreeState and copy column data into it.
  auto ts = CreateTreeState(normalized_parent.begin(), n);

  // Deduplicate columns: map col_index → TreeState index.
  base::FlatHashMap<uint32_t, uint32_t> col_to_storage;
  base::FlatHashMap<uint32_t, uint32_t> col_to_null_bv;

  for (const auto& ri : reg_inits_) {
    const auto& col = *df_.columns_[ri.col];
    switch (ri.kind) {
      case RegInit::kStorage: {
        auto* existing = col_to_storage.Find(ri.col);
        if (!existing) {
          uint32_t idx = static_cast<uint32_t>(ts->columns.size());
          uint32_t elem_size = ElementSize(col.storage.type());
          TreeState::ColumnStorage cs;
          cs.elem_size = elem_size;
          if (IsSparseNull(col)) {
            cs.data = DensifySparseColumn(col, n);
          } else {
            auto byte_count = static_cast<uint64_t>(n) * elem_size;
            cs.data = Slab<uint8_t>::Alloc(byte_count);
            const void* src = nullptr;
            std::visit(
                [&src](const auto* p) { src = static_cast<const void*>(p); },
                col.storage.data());
            memcpy(cs.data.begin(), src, byte_count);
          }
          ts->columns.push_back(std::move(cs));
          col_to_storage[ri.col] = idx;
        }
        break;
      }
      case RegInit::kNullBv: {
        if (!col_to_null_bv.Find(ri.col)) {
          uint32_t idx = static_cast<uint32_t>(ts->null_bitvectors.size());
          const BitVector* bv = col.null_storage.MaybeGetNullBitVector();
          ts->null_bitvectors.push_back(bv ? bv->Clone() : BitVector());
          col_to_null_bv[ri.col] = idx;
        }
        break;
      }
    }
  }

  // Initialize interpreter and set TreeState register.
  i::Interpreter<TreeValueFetcher> interp;
  interp.Initialize(builder_->bytecode(), builder_->register_count(), pool_);
  interp.SetRegisterValue(
      i::WriteHandle<std::unique_ptr<TreeState>>(tree_state_reg_index_),
      std::move(ts));

  // Set StoragePtr and null bitvector registers, pointing into TreeState.
  const auto* ts_ptr = interp.GetRegisterValue(
      i::ReadHandle<std::unique_ptr<TreeState>>(tree_state_reg_index_));
  PERFETTO_CHECK(ts_ptr && *ts_ptr);
  const auto& ts_ref = **ts_ptr;

  for (const auto& ri : reg_inits_) {
    const auto& col = *df_.columns_[ri.col];
    switch (ri.kind) {
      case RegInit::kStorage: {
        uint32_t idx = *col_to_storage.Find(ri.col);
        interp.SetRegisterValue(i::WriteHandle<i::StoragePtr>(ri.reg),
                                i::StoragePtr{ts_ref.columns[idx].data.begin(),
                                              col.storage.type()});
        break;
      }
      case RegInit::kNullBv: {
        uint32_t idx = *col_to_null_bv.Find(ri.col);
        i::NullBitvector nbv;
        nbv.bv = &ts_ref.null_bitvectors[idx];
        interp.SetRegisterValue(i::WriteHandle<i::NullBitvector>(ri.reg),
                                std::move(nbv));
        break;
      }
    }
  }

  TreeValueFetcher fetcher(filter_values_.data());
  interp.Execute(fetcher);

  const auto& final_ts = **ts_ptr;

  uint32_t final_count = final_ts.row_count;
  ASSIGN_OR_RETURN(auto tree_cols, BuildTreeColumns(final_ts.parent.begin(),
                                                    final_count, pool_));
  return dataframe::Dataframe::HorizontalConcat(
      std::move(tree_cols),
      std::move(df_).SelectRows(final_ts.original_rows.begin(), final_count));
}

}  // namespace perfetto::trace_processor::core::tree
