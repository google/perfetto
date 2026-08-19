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

#include "src/trace_processor/core/dataframe/query_plan.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/bytecode_lowering.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::dataframe {

namespace {

namespace i = interpreter;

// Calculates filter preference score for ordering filters.
// Lower scores are applied first for better efficiency.
uint32_t FilterPreference(const FilterSpec& fs, const Column& col) {
  enum AbsolutePreference : uint8_t {
    kIdEq,                     // Most efficient: id equality check
    kSetIdSortedEq,            // Set id sorted equality check
    kIdInequality,             // Id inequality check
    kNumericSortedEq,          // Numeric sorted equality check
    kNumericSortedInequality,  // Numeric inequality check
    kStringSortedEq,           // String sorted equality check
    kStringSortedInequality,   // String inequality check
    kLeastPreferred,           // Least preferred
  };
  const auto& op = fs.op;
  const auto& ct = col.storage.type();
  const auto& n = col.null_storage.nullability();
  if (n.Is<NonNull>() && ct.Is<Id>() && op.Is<Eq>()) {
    return kIdEq;
  }
  if (n.Is<NonNull>() && ct.Is<Uint32>() && col.sort_state.Is<SetIdSorted>() &&
      op.Is<Eq>()) {
    return kSetIdSortedEq;
  }
  if (n.Is<NonNull>() && ct.Is<Id>() && op.IsAnyOf<InequalityOp>()) {
    return kIdInequality;
  }
  if (n.Is<NonNull>() && col.sort_state.Is<Sorted>() &&
      ct.IsAnyOf<IntegerOrDoubleType>() && op.Is<Eq>()) {
    return kNumericSortedEq;
  }
  if (n.Is<NonNull>() && col.sort_state.Is<Sorted>() &&
      ct.IsAnyOf<IntegerOrDoubleType>() && op.IsAnyOf<InequalityOp>()) {
    return kNumericSortedInequality;
  }
  if (n.Is<NonNull>() && col.sort_state.Is<Sorted>() && ct.Is<String>() &&
      op.Is<Eq>()) {
    return kStringSortedEq;
  }
  if (n.Is<NonNull>() && col.sort_state.Is<Sorted>() && ct.Is<String>() &&
      op.IsAnyOf<InequalityOp>()) {
    return kStringSortedInequality;
  }
  return kLeastPreferred;
}

struct BestIndex {
  uint32_t best_index_idx;
  std::vector<uint32_t> best_index_specs;
};
std::optional<BestIndex> GetBestIndexForFilterSpecs(
    uint32_t max_row_count,
    const std::vector<FilterSpec>& all_specs,
    const std::vector<uint8_t>& spec_already_handled,
    const std::vector<Index>& indexes) {
  // If we have very few rows, there's no point in using an index.
  if (max_row_count <= 1) {
    return std::nullopt;
  }
  uint32_t best_index_idx = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> best_index_specs;
  for (uint32_t i = 0; i < indexes.size(); ++i) {
    const Index& index = indexes[i];
    std::vector<uint32_t> current_specs_for_this_index;
    for (uint32_t column : index.columns()) {
      bool found_spec_for_column = false;
      for (uint32_t spec_idx = 0; spec_idx < all_specs.size(); ++spec_idx) {
        if (spec_already_handled[spec_idx]) {
          continue;
        }
        const FilterSpec& current_spec = all_specs[spec_idx];
        if (current_spec.col == column &&
            (current_spec.op.Is<Eq>() || current_spec.op.Is<In>())) {
          current_specs_for_this_index.push_back(spec_idx);
          found_spec_for_column = true;
          break;
        }
      }
      if (!found_spec_for_column) {
        break;
      }
      // An In filter produces non-contiguous output, breaking the sort
      // invariant needed by subsequent columns' binary searches. So In
      // must be terminal: stop matching further index columns.
      if (all_specs[current_specs_for_this_index.back()].op.Is<In>()) {
        break;
      }
    }
    if (current_specs_for_this_index.size() > best_index_specs.size()) {
      best_index_idx = i;
      best_index_specs = std::move(current_specs_for_this_index);
    }
  }
  if (best_index_idx == std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  return BestIndex{best_index_idx, std::move(best_index_specs)};
}

}  // namespace

QueryPlanBuilder::QueryPlanBuilder(
    BytecodeLowering& lowering,
    const std::vector<std::shared_ptr<Column>>& columns,
    const std::vector<Index>& indexes)
    : columns_(columns), indexes_(indexes), lowering_(lowering) {}

base::StatusOr<QueryPlanImpl> QueryPlanBuilder::Build(
    uint32_t row_count,
    const std::vector<std::shared_ptr<Column>>& columns,
    const std::vector<Index>& indexes,
    std::vector<FilterSpec>& specs,
    const std::vector<DistinctSpec>& distinct,
    const std::vector<SortSpec>& sort_specs,
    const LimitSpec& limit_spec,
    uint64_t cols_used) {
  BytecodeLowering lowering(row_count, columns, indexes);
  QueryPlanBuilder builder(lowering, columns, indexes);
  RETURN_IF_ERROR(builder.Filter(specs));
  builder.Distinct(distinct);
  if (builder.CanUseMinMaxOptimization(sort_specs, limit_spec)) {
    builder.MinMax(sort_specs[0]);
    builder.Output({}, cols_used);
  } else {
    builder.Sort(sort_specs);
    builder.Output(limit_spec, cols_used);
  }
  return std::move(lowering).Build();
}

base::Status QueryPlanBuilder::Filter(std::vector<FilterSpec>& specs) {
  // Sort filters by efficiency (most selective/cheapest first)
  std::stable_sort(specs.begin(), specs.end(),
                   [this](const FilterSpec& a, const FilterSpec& b) {
                     const auto& a_col = GetColumn(a.col);
                     const auto& b_col = GetColumn(b.col);
                     return FilterPreference(a, a_col) <
                            FilterPreference(b, b_col);
                   });

  std::vector<uint8_t> specs_handled(specs.size(), false);

  // Phase 1: Handle sorted constraints first
  for (uint32_t i = 0; i < specs.size(); ++i) {
    if (specs_handled[i]) {
      continue;
    }
    FilterSpec& c = specs[i];
    auto non_null_op = c.op.TryDowncast<NonNullOp>();
    if (!non_null_op) {
      continue;
    }
    const Column& col = GetColumn(c.col);
    if (!TrySortedConstraint(c, col.storage.type(), *non_null_op)) {
      continue;
    }
    specs_handled[i] = true;
  }

  // Phase 2: Handle constraints which can use an index.
  std::optional<BestIndex> best_index = GetBestIndexForFilterSpecs(
      lowering_.max_row_count(), specs, specs_handled, indexes_);
  if (best_index) {
    IndexConstraints(specs, specs_handled, best_index->best_index_idx,
                     best_index->best_index_specs);
  }

  // Phase 3: Handle all remaining constraints.
  for (uint32_t i = 0; i < specs.size(); ++i) {
    if (specs_handled[i]) {
      continue;
    }
    FilterSpec& c = specs[i];
    const Column& col = GetColumn(c.col);
    StorageType ct = col.storage.type();

    if (c.op.Is<In>()) {
      lowering_.EmitScanFilterIn(c, lowering_.EmitCastFilterValueList(c, ct));
      continue;
    }

    // Get the non-null operation (all our ops are non-null at this point)
    auto non_null_op = c.op.TryDowncast<NonNullOp>();
    if (!non_null_op) {
      NullConstraint(*c.op.TryDowncast<NullOp>(), c);
      continue;
    }

    // Handle non-string data types
    if (const auto& n = ct.TryDowncast<NonStringType>(); n) {
      if (auto op = c.op.TryDowncast<NonStringOp>(); op) {
        NonStringConstraint(c, *n, *op,
                            lowering_.EmitCastFilterValue(c, ct, *non_null_op));
      } else {
        lowering_.EmitGuaranteedEmpty();
      }
      continue;
    }

    PERFETTO_CHECK(ct.Is<String>());
    auto op = non_null_op->TryDowncast<StringOp>();
    PERFETTO_CHECK(op);
    RETURN_IF_ERROR(StringConstraint(
        c, *op, lowering_.EmitCastFilterValue(c, ct, *non_null_op)));
  }
  return base::OkStatus();
}

bool QueryPlanBuilder::TrySortedConstraint(FilterSpec& fs,
                                           const StorageType& ct,
                                           const NonNullOp& op) {
  const auto& col = GetColumn(fs.col);
  const auto& nullability = col.null_storage.nullability();
  if (!nullability.Is<NonNull>() || col.sort_state.Is<Unsorted>()) {
    return false;
  }
  auto range_op = op.TryDowncast<RangeOp>();
  if (!range_op) {
    return false;
  }

  // We should have ordered the constraints such that we only reach this
  // point with range indices.
  PERFETTO_CHECK(lowering_.IsIndicesRange());

  auto value_reg = lowering_.EmitCastFilterValue(fs, ct, op);

  // Handle set id equality with a specialized opcode.
  if (ct.Is<Uint32>() && col.sort_state.Is<SetIdSorted>() && op.Is<Eq>()) {
    lowering_.EmitSetIdSortedEq(fs, value_reg);
    return true;
  }

  if (col.specialized_storage.Is<SpecializedStorage::SmallValueEq>() &&
      op.Is<Eq>()) {
    lowering_.EmitSmallValueEq(fs, value_reg);
    return true;
  }

  lowering_.EmitSortedFilter(fs, ct, *range_op, value_reg);
  return true;
}

void QueryPlanBuilder::IndexConstraints(
    std::vector<FilterSpec>& specs,
    std::vector<uint8_t>& specs_handled,
    uint32_t index_idx,
    const std::vector<uint32_t>& filter_specs) {
  lowering_.EmitIndexedFilters(specs, index_idx, filter_specs);
  for (uint32_t spec_idx : filter_specs) {
    specs_handled[spec_idx] = true;
  }
}

void QueryPlanBuilder::NonStringConstraint(
    const FilterSpec& c,
    const NonStringType& type,
    const NonStringOp& op,
    const i::ReadHandle<i::CastFilterValueResult>& result) {
  const auto& col = GetColumn(c.col);
  if (lowering_.IsIndicesRange() && op.Is<Eq>() &&
      col.null_storage.nullability().Is<NonNull>()) {
    // Non null equality on an id column should have been handled earlier.
    PERFETTO_CHECK(!type.Is<Id>());
    auto non_id_type = type.TryDowncast<NonIdStorageType>();
    PERFETTO_CHECK(non_id_type);
    lowering_.EmitLinearFilterEq(c, *non_id_type, result);
    return;
  }
  lowering_.EmitNonStringFilter(c, type, op, result);
}

base::Status QueryPlanBuilder::StringConstraint(
    const FilterSpec& c,
    const StringOp& op,
    const i::ReadHandle<i::CastFilterValueResult>& result) {
  const auto& col = GetColumn(c.col);
  if (op.Is<Eq>() && lowering_.IsIndicesRange() &&
      col.null_storage.nullability().Is<NonNull>()) {
    lowering_.EmitLinearFilterEq(c, NonIdStorageType{String{}}, result);
    return base::OkStatus();
  }
  lowering_.EmitStringFilter(c, op, result);
  return base::OkStatus();
}

void QueryPlanBuilder::NullConstraint(const NullOp& op, FilterSpec& c) {
  // Even if we don't need this to filter null/non-null, we add it so that
  // the caller (i.e. SQLite) knows that we are able to handle the constraint.
  lowering_.ReserveFilterValueSlot(c);

  const auto& col = GetColumn(c.col);
  uint32_t nullability_type_index = col.null_storage.nullability().index();
  switch (nullability_type_index) {
    case Nullability::GetTypeIndex<SparseNull>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountAlways>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountUntilFinalization>():
    case Nullability::GetTypeIndex<DenseNull>():
      lowering_.EmitNullFilter(c, op);
      break;
    case Nullability::GetTypeIndex<NonNull>():
      if (op.Is<IsNull>()) {
        lowering_.EmitGuaranteedEmpty();
        return;
      }
      // Nothing to do as the column is non-null.
      return;
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

void QueryPlanBuilder::Distinct(
    const std::vector<DistinctSpec>& distinct_specs) {
  if (distinct_specs.empty()) {
    return;
  }
  lowering_.EmitDistinct(distinct_specs);
}

void QueryPlanBuilder::Sort(const std::vector<SortSpec>& sort_specs) {
  if (sort_specs.empty()) {
    return;
  }

  // Optimization: If there's a single sort constraint on a NonNull
  // column that is already sorted accordingly, skip the sort operation.
  if (sort_specs.size() == 1) {
    const auto& single_spec = sort_specs[0];
    const Column& col = GetColumn(single_spec.col);
    if (col.null_storage.nullability().Is<NonNull>() &&
        (col.sort_state.Is<Sorted>() || col.sort_state.Is<IdSorted>() ||
         col.sort_state.Is<SetIdSorted>())) {
      switch (single_spec.direction) {
        case SortDirection::kAscending:
          // The column is NonNull and already sorted as required.
          return;
        case SortDirection::kDescending:
          // The column is NonNull and sorted in the reverse order. Just
          // reverse the indices to get the correct order.
          lowering_.EmitReverse();
          return;
      }
    }
  }
  lowering_.EmitSort(sort_specs);
}

void QueryPlanBuilder::MinMax(const SortSpec& sort_spec) {
  lowering_.EmitMinMax(sort_spec);
}

void QueryPlanBuilder::Output(const LimitSpec& limit, uint64_t cols_used) {
  lowering_.EmitOutput(limit, cols_used);
}

bool QueryPlanBuilder::CanUseMinMaxOptimization(
    const std::vector<SortSpec>& sort_specs,
    const LimitSpec& limit_spec) {
  return sort_specs.size() == 1 &&
         GetColumn(sort_specs[0].col)
             .null_storage.nullability()
             .Is<NonNull>() &&
         limit_spec.limit == 1 && limit_spec.offset.value_or(0) == 0;
}

i::RegValue QueryPlanImpl::GetRegisterInitValue(const RegisterInit& init,
                                                const Column* const* columns,
                                                const Index* indexes) {
  switch (init.kind.index()) {
    case RegisterInit::Type::GetTypeIndex<Id>():
      // Id columns don't have actual storage - the row index IS the value.
      // Return a nullptr StoragePtr which the interpreter knows to handle.
      return i::StoragePtr{nullptr, Id{}};
    case RegisterInit::Type::GetTypeIndex<Uint32>():
      return i::StoragePtr{
          columns[init.source_index]->storage.unchecked_data<Uint32>(),
          Uint32{},
      };
    case RegisterInit::Type::GetTypeIndex<Int32>():
      return i::StoragePtr{
          columns[init.source_index]->storage.unchecked_data<Int32>(),
          Int32{},
      };
    case RegisterInit::Type::GetTypeIndex<Int64>():
      return i::StoragePtr{
          columns[init.source_index]->storage.unchecked_data<Int64>(),
          Int64{},
      };
    case RegisterInit::Type::GetTypeIndex<Double>():
      return i::StoragePtr{
          columns[init.source_index]->storage.unchecked_data<Double>(),
          Double{},
      };
    case RegisterInit::Type::GetTypeIndex<String>():
      return i::StoragePtr{
          columns[init.source_index]->storage.unchecked_data<String>(),
          String{},
      };
    case RegisterInit::Type::GetTypeIndex<RegisterInit::NullBitvector>(): {
      i::NullBitvector nbv;
      nbv.bv = columns[init.source_index]->null_storage.MaybeGetNullBitVector();
      return nbv;
    }
    case RegisterInit::Type::GetTypeIndex<RegisterInit::IndexVector>():
      return Span<uint32_t>(
          indexes[init.source_index].permutation_vector()->data(),
          indexes[init.source_index].permutation_vector()->data() +
              indexes[init.source_index].permutation_vector()->size());
    case RegisterInit::Type::GetTypeIndex<
        RegisterInit::SmallValueEqBitvector>(): {
      const auto& sve = columns[init.source_index]
                            ->specialized_storage
                            .unchecked_get<SpecializedStorage::SmallValueEq>();
      return &sve.bit_vector;
    }
    case RegisterInit::Type::GetTypeIndex<
        RegisterInit::SmallValueEqPopcount>(): {
      const auto& sve = columns[init.source_index]
                            ->specialized_storage
                            .unchecked_get<SpecializedStorage::SmallValueEq>();
      return Span<const uint32_t>(
          sve.prefix_popcount.data(),
          sve.prefix_popcount.data() + sve.prefix_popcount.size());
    }
    default:
      PERFETTO_FATAL("Unhandled RegisterInit kind: %u",
                     static_cast<uint32_t>(init.kind.index()));
  }
}

}  // namespace perfetto::trace_processor::core::dataframe
