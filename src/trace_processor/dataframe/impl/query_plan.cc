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

#include "src/trace_processor/dataframe/impl/query_plan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/util/regex.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::dataframe::impl {

namespace {

// Calculates filter preference score for ordering filters.
// Lower scores are applied first for better efficiency.
uint32_t FilterPreference(const FilterSpec& fs, const impl::Column& col) {
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

// Gets the appropriate bound modifier and range operation type
// for a given range operation.
std::pair<BoundModifier, EqualRangeLowerBoundUpperBound> GetSortedFilterArgs(
    const RangeOp& op) {
  switch (op.index()) {
    case RangeOp::GetTypeIndex<Eq>():
      return std::make_pair(BothBounds{}, EqualRange{});
    case RangeOp::GetTypeIndex<Lt>():
      return std::make_pair(EndBound{}, LowerBound{});
    case RangeOp::GetTypeIndex<Le>():
      return std::make_pair(EndBound{}, UpperBound{});
    case RangeOp::GetTypeIndex<Gt>():
      return std::make_pair(BeginBound{}, UpperBound{});
    case RangeOp::GetTypeIndex<Ge>():
      return std::make_pair(BeginBound{}, LowerBound{});
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

// Helper to get byte size of storage types for layout calculation.
// Returns 0 for Id type as it's handled specially.
inline uint8_t GetDataSize(StorageType type) {
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
    case StorageType::GetTypeIndex<Uint32>():
    case StorageType::GetTypeIndex<Int32>():
    case StorageType::GetTypeIndex<String>():
      return sizeof(uint32_t);
    case StorageType::GetTypeIndex<Int64>():
      return sizeof(int64_t);
    case StorageType::GetTypeIndex<Double>():
      return sizeof(double);
    default:
      PERFETTO_FATAL("Invalid storage type");
  }
}

}  // namespace

QueryPlanBuilder::QueryPlanBuilder(uint32_t row_count,
                                   const std::vector<Column>& columns)
    : columns_(columns) {
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    column_states_.emplace_back();
  }
  // Setup the maximum and estimated row counts.
  plan_.params.max_row_count = row_count;
  plan_.params.estimated_row_count = row_count;

  // Initialize with a range covering all rows
  bytecode::reg::RwHandle<Range> range{register_count_++};
  {
    using B = bytecode::InitRange;
    auto& ir = AddOpcode<B>(UnchangedRowCount{});
    ir.arg<B::size>() = row_count;
    ir.arg<B::dest_register>() = range;
  }
  indices_reg_ = range;
}

base::Status QueryPlanBuilder::Filter(std::vector<FilterSpec>& specs) {
  // Sort filters by efficiency (most selective/cheapest first)
  std::stable_sort(specs.begin(), specs.end(),
                   [this](const FilterSpec& a, const FilterSpec& b) {
                     const auto& a_col = columns_[a.col];
                     const auto& b_col = columns_[b.col];
                     return FilterPreference(a, a_col) <
                            FilterPreference(b, b_col);
                   });

  // Apply each filter in the optimized order
  for (FilterSpec& c : specs) {
    const Column& col = columns_[c.col];
    StorageType ct = col.storage.type();

    // Get the non-null operation (all our ops are non-null at this point)
    auto non_null_op = c.op.TryDowncast<NonNullOp>();
    if (!non_null_op) {
      NullConstraint(*c.op.TryDowncast<NullOp>(), c);
      continue;
    }

    // Create a register for the coerced filter value
    bytecode::reg::RwHandle<CastFilterValueResult> value_reg{register_count_++};
    {
      using B = bytecode::CastFilterValueBase;
      auto& bc = AddOpcode<B>(bytecode::Index<bytecode::CastFilterValue>(ct),
                              UnchangedRowCount{});
      bc.arg<B::fval_handle>() = {plan_.params.filter_value_count};
      bc.arg<B::write_register>() = value_reg;
      bc.arg<B::op>() = *non_null_op;
      c.value_index = plan_.params.filter_value_count++;
    }

    // Try specialized optimizations first
    if (TrySortedConstraint(c, ct, *non_null_op, value_reg)) {
      continue;
    }

    // Handle non-string data types
    if (const auto& n = ct.TryDowncast<NonStringType>(); n) {
      if (auto op = c.op.TryDowncast<NonStringOp>(); op) {
        NonStringConstraint(c, *n, *op, value_reg);
      } else {
        SetGuaranteedToBeEmpty();
      }
      continue;
    }

    PERFETTO_CHECK(ct.Is<String>());
    auto op = non_null_op->TryDowncast<StringOp>();
    PERFETTO_CHECK(op);
    RETURN_IF_ERROR(StringConstraint(c, *op, value_reg));
  }
  return base::OkStatus();
}

void QueryPlanBuilder::Distinct(
    const std::vector<DistinctSpec>& distinct_specs) {
  if (distinct_specs.empty()) {
    return;
  }
  bytecode::reg::RwHandle<Span<uint32_t>> indices = EnsureIndicesAreInSlab();
  uint16_t total_row_stride = 0;
  for (const auto& spec : distinct_specs) {
    const Column& col = columns_[spec.col];
    bool is_nullable = !col.null_storage.nullability().Is<NonNull>();
    total_row_stride +=
        (is_nullable ? 1u : 0u) + GetDataSize(col.storage.type());
  }

  uint32_t buffer_size = plan_.params.max_row_count * total_row_stride;
  bytecode::reg::RwHandle<Slab<uint8_t>> buffer_reg{register_count_++};
  {
    using B = bytecode::AllocateRowLayoutBuffer;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::buffer_size>() = buffer_size;
    bc.arg<B::dest_buffer_register>() = buffer_reg;
  }
  uint16_t current_offset = 0;
  for (const auto& spec : distinct_specs) {
    const Column& col = columns_[spec.col];
    const auto& nullability = col.null_storage.nullability();
    uint8_t data_size = GetDataSize(col.storage.type());
    switch (nullability.index()) {
      case Nullability::GetTypeIndex<NonNull>(): {
        using B = bytecode::CopyToRowLayoutNonNull;
        auto& bc = AddOpcode<B>(UnchangedRowCount{});
        bc.arg<B::col>() = spec.col;
        bc.arg<B::source_indices_register>() = indices;
        bc.arg<B::dest_buffer_register>() = buffer_reg;
        bc.arg<B::row_layout_offset>() = current_offset;
        bc.arg<B::row_layout_stride>() = total_row_stride;
        bc.arg<B::copy_size>() = data_size;
        break;
      }
      case Nullability::GetTypeIndex<DenseNull>(): {
        using B = bytecode::CopyToRowLayoutDenseNull;
        auto& bc = AddOpcode<B>(UnchangedRowCount{});
        bc.arg<B::col>() = spec.col;
        bc.arg<B::source_indices_register>() = indices;
        bc.arg<B::dest_buffer_register>() = buffer_reg;
        bc.arg<B::row_layout_offset>() = current_offset;
        bc.arg<B::row_layout_stride>() = total_row_stride;
        bc.arg<B::copy_size>() = data_size;
        break;
      }
      case Nullability::GetTypeIndex<SparseNull>(): {
        auto popcount_reg = PrefixPopcountRegisterFor(spec.col);
        using B = bytecode::CopyToRowLayoutSparseNull;
        auto& bc = AddOpcode<B>(UnchangedRowCount{});
        bc.arg<B::col>() = spec.col;
        bc.arg<B::source_indices_register>() = indices;
        bc.arg<B::dest_buffer_register>() = buffer_reg;
        bc.arg<B::row_layout_offset>() = current_offset;
        bc.arg<B::row_layout_stride>() = total_row_stride;
        bc.arg<B::copy_size>() = data_size;
        bc.arg<B::popcount_register>() = popcount_reg;
        break;
      }
      default:
        PERFETTO_FATAL("Unreachable");
    }
    current_offset += (nullability.Is<NonNull>() ? 0u : 1u) + data_size;
  }
  PERFETTO_CHECK(current_offset == total_row_stride);
  {
    using B = bytecode::Distinct;
    auto& bc = AddOpcode<B>(DoubleLog2RowCount{});
    bc.arg<B::buffer_register>() = buffer_reg;
    bc.arg<B::total_row_stride>() = total_row_stride;
    bc.arg<B::indices_register>() = indices;
  }
}

void QueryPlanBuilder::Sort(const std::vector<SortSpec>& sort_specs) {
  if (sort_specs.empty()) {
    return;
  }

  // As our data is columnar, it's always more efficient to sort one column
  // at a time rather than try and sort lexiographically all at once.
  // To preserve correctness, we need to stably sort the index vector once
  // for each order by in *reverse* order. Reverse order is important as it
  // preserves the lexiographical property.
  //
  // For example, suppose we have the following:
  // Table {
  //   Column x;
  //   Column y
  //   Column z;
  // }
  //
  // Then, to sort "y asc, x desc", we could do one of two things:
  //  1) sort the index vector all at once and on each index, we compare
  //     y then z. This is slow as the data is columnar and we need to
  //     repeatedly branch inside each column.
  //  2) we can stably sort first on x desc and then sort on y asc. This will
  //     first put all the x in the correct order such that when we sort on
  //     y asc, we will have the correct order of x where y is the same (since
  //     the sort is stable).
  //
  // TODO(lalitm): it is possible that we could sort the last constraint (i.e.
  // the first constraint in the below loop) in a non-stable way. However,
  // this is more subtle than it appears as we would then need special
  // handling where there are order bys on a column which is already sorted
  // (e.g. ts, id). Investigate whether the performance gains from this are
  // worthwhile. This also needs changes to the constraint modification logic
  // in DbSqliteTable which currently eliminates constraints on sorted
  // columns.
  bytecode::reg::RwHandle<Span<uint32_t>> indices = EnsureIndicesAreInSlab();
  for (auto it = sort_specs.rbegin(); it != sort_specs.rend(); ++it) {
    const SortSpec& sort_spec = *it;
    const Column& sort_col = columns_[sort_spec.col];
    StorageType sort_col_type = sort_col.storage.type();

    uint32_t nullability_type_index =
        sort_col.null_storage.nullability().index();
    bytecode::reg::RwHandle<Span<uint32_t>> sort_indices;
    switch (nullability_type_index) {
      case Nullability::GetTypeIndex<SparseNull>():
      case Nullability::GetTypeIndex<DenseNull>(): {
        sort_indices =
            bytecode::reg::RwHandle<Span<uint32_t>>{register_count_++};

        using B = bytecode::NullIndicesStablePartition;
        B& bc = AddOpcode<B>(UnchangedRowCount{});
        bc.arg<B::col>() = sort_spec.col;
        bc.arg<B::nulls_location>() = it->direction == SortDirection::kAscending
                                          ? NullsLocation{NullsAtStart{}}
                                          : NullsLocation{NullsAtEnd{}};
        bc.arg<B::partition_register>() = indices;
        bc.arg<B::dest_non_null_register>() = sort_indices;

        // If in sparse mode, we also need to translate all the indices.
        if (nullability_type_index == Nullability::GetTypeIndex<SparseNull>()) {
          auto popcount_reg = PrefixPopcountRegisterFor(sort_spec.col);
          {
            using BI = bytecode::TranslateSparseNullIndices;
            auto& bi = AddOpcode<BI>(UnchangedRowCount{});
            bi.arg<BI::col>() = sort_spec.col;
            bi.arg<BI::popcount_register>() = popcount_reg;
            bi.arg<BI::source_register>() = sort_indices;
            bi.arg<BI::update_register>() = sort_indices;
          }
        }
        break;
      }
      case Nullability::GetTypeIndex<NonNull>():
        sort_indices = indices;
        break;
      default:
        PERFETTO_FATAL("Unreachable");
    }
    using B = bytecode::StableSortIndicesBase;
    {
      auto& bc = AddOpcode<B>(
          bytecode::Index<bytecode::StableSortIndices>(sort_col_type),
          UnchangedRowCount{});
      bc.arg<B::col>() = sort_spec.col;
      bc.arg<B::direction>() = sort_spec.direction;
      bc.arg<B::update_register>() = sort_indices;
    }
  }
}

void QueryPlanBuilder::MinMax(const SortSpec& sort_spec) {
  uint32_t col_idx = sort_spec.col;
  const auto& col = columns_[col_idx];
  StorageType storage_type = col.storage.type();

  MinMaxOp mmop = sort_spec.direction == SortDirection::kAscending
                      ? MinMaxOp(MinOp{})
                      : MinMaxOp(MaxOp{});

  auto indices = EnsureIndicesAreInSlab();
  using B = bytecode::FindMinMaxIndexBase;
  auto& op = AddOpcode<B>(
      bytecode::Index<bytecode::FindMinMaxIndex>(storage_type, mmop),
      OneRowCount{});
  op.arg<B::update_register>() = indices;
  op.arg<B::col>() = col_idx;
}

void QueryPlanBuilder::Output(const LimitSpec& limit, uint64_t cols_used) {
  // Structure to track column and offset pairs
  struct ColAndOffset {
    uint32_t col;
    uint32_t offset;
  };

  base::SmallVector<ColAndOffset, 64> null_cols;
  plan_.params.output_per_row = 1;

  // Process each column that will be used in the output
  for (uint32_t i = 0; i < 64; ++i, cols_used >>= 1) {
    if ((cols_used & 1u) == 0) {
      continue;
    }
    const auto& col = columns_[i];
    switch (col.null_storage.nullability().index()) {
      case Nullability::GetTypeIndex<SparseNull>():
      case Nullability::GetTypeIndex<DenseNull>():
        null_cols.emplace_back(ColAndOffset{i, plan_.params.output_per_row});
        plan_.params.col_to_output_offset[i] = plan_.params.output_per_row++;
        break;
      case Nullability::GetTypeIndex<NonNull>():
        // For non-null columns, we can directly use the indices
        plan_.params.col_to_output_offset[i] = 0;
        break;
      default:
        PERFETTO_FATAL("Unreachable");
    }
  }

  auto in_memory_indices = EnsureIndicesAreInSlab();
  if (limit.limit || limit.offset) {
    auto o = limit.offset.value_or(0);
    auto l = limit.limit.value_or(std::numeric_limits<uint32_t>::max());
    using B = bytecode::LimitOffsetIndices;
    auto& bc = AddOpcode<B>(LimitOffsetRowCount{l, o});
    bc.arg<B::offset_value>() = o;
    bc.arg<B::limit_value>() = l;
    bc.arg<B::update_register>() = in_memory_indices;
  }

  bytecode::reg::RwHandle<Span<uint32_t>> storage_update_register;
  if (plan_.params.output_per_row > 1) {
    bytecode::reg::RwHandle<Slab<uint32_t>> slab_register{register_count_++};
    bytecode::reg::RwHandle<Span<uint32_t>> span_register{register_count_++};
    {
      using B = bytecode::AllocateIndices;
      auto& bc = AddOpcode<B>(UnchangedRowCount{});
      bc.arg<B::size>() =
          plan_.params.max_row_count * plan_.params.output_per_row;
      bc.arg<B::dest_slab_register>() = slab_register;
      bc.arg<B::dest_span_register>() = span_register;
    }
    {
      using B = bytecode::StrideCopy;
      auto& bc = AddOpcode<B>(UnchangedRowCount{});
      bc.arg<B::source_register>() = in_memory_indices;
      bc.arg<B::update_register>() = span_register;
      bc.arg<B::stride>() = plan_.params.output_per_row;
      storage_update_register = span_register;
    }
    for (auto [col, offset] : null_cols) {
      const auto& c = columns_[col];
      switch (c.null_storage.nullability().index()) {
        case Nullability::GetTypeIndex<SparseNull>(): {
          using B = bytecode::StrideTranslateAndCopySparseNullIndices;
          auto reg = PrefixPopcountRegisterFor(col);
          auto& bc = AddOpcode<B>(UnchangedRowCount{});
          bc.arg<B::update_register>() = storage_update_register;
          bc.arg<B::popcount_register>() = {reg};
          bc.arg<B::col>() = col;
          bc.arg<B::offset>() = offset;
          bc.arg<B::stride>() = plan_.params.output_per_row;
          break;
        }
        case Nullability::GetTypeIndex<DenseNull>(): {
          using B = bytecode::StrideCopyDenseNullIndices;
          auto& bc = AddOpcode<B>(UnchangedRowCount{});
          bc.arg<B::update_register>() = storage_update_register;
          bc.arg<B::col>() = col;
          bc.arg<B::offset>() = offset;
          bc.arg<B::stride>() = plan_.params.output_per_row;
          break;
        }
        case Nullability::GetTypeIndex<NonNull>():
        default:
          PERFETTO_FATAL("Unreachable");
      }
    }
  } else {
    PERFETTO_CHECK(null_cols.empty());
    storage_update_register = in_memory_indices;
  }
  plan_.params.output_register = storage_update_register;
}

QueryPlan QueryPlanBuilder::Build() && {
  return std::move(plan_);
}

void QueryPlanBuilder::NonStringConstraint(
    const FilterSpec& c,
    const NonStringType& type,
    const NonStringOp& op,
    const bytecode::reg::ReadHandle<CastFilterValueResult>& result) {
  auto source = MaybeAddOverlayTranslation(c);
  {
    using B = bytecode::NonStringFilterBase;
    B& bc = AddOpcode<B>(bytecode::Index<bytecode::NonStringFilter>(type, op),
                         op.Is<Eq>() ? RowCountModifier{DoubleLog2RowCount{}}
                                     : RowCountModifier{Div2RowCount{}});
    bc.arg<B::col>() = c.col;
    bc.arg<B::val_register>() = result;
    bc.arg<B::source_register>() = source;
    bc.arg<B::update_register>() = EnsureIndicesAreInSlab();
  }
}

base::Status QueryPlanBuilder::StringConstraint(
    const FilterSpec& c,
    const StringOp& op,
    const bytecode::reg::ReadHandle<CastFilterValueResult>& result) {
  if constexpr (!regex::IsRegexSupported()) {
    if (op.Is<Regex>()) {
      return base::ErrStatus("Regex is not supported");
    }
  }

  auto source = MaybeAddOverlayTranslation(c);
  {
    using B = bytecode::StringFilterBase;
    B& bc = AddOpcode<B>(bytecode::Index<bytecode::StringFilter>(op),
                         op.Is<Eq>() ? RowCountModifier{DoubleLog2RowCount{}}
                                     : RowCountModifier{Div2RowCount{}});
    bc.arg<B::col>() = c.col;
    bc.arg<B::val_register>() = result;
    bc.arg<B::source_register>() = source;
    bc.arg<B::update_register>() = EnsureIndicesAreInSlab();
  }
  return base::OkStatus();
}

void QueryPlanBuilder::NullConstraint(const NullOp& op, FilterSpec& c) {
  // Even if we don't need this to filter null/non-null, we add it so that
  // the caller (i.e. SQLite) knows that we are able to handle the constraint.
  c.value_index = plan_.params.filter_value_count++;

  const auto& col = columns_[c.col];
  uint32_t nullability_type_index = col.null_storage.nullability().index();
  switch (nullability_type_index) {
    case Nullability::GetTypeIndex<SparseNull>():
    case Nullability::GetTypeIndex<DenseNull>(): {
      auto indices = EnsureIndicesAreInSlab();
      {
        using B = bytecode::NullFilterBase;
        B& bc = AddOpcode<B>(bytecode::Index<bytecode::NullFilter>(op),
                             DoubleLog2RowCount{});
        bc.arg<B::col>() = c.col;
        bc.arg<B::update_register>() = indices;
      }
      break;
    }
    case Nullability::GetTypeIndex<NonNull>():
      if (op.Is<IsNull>()) {
        SetGuaranteedToBeEmpty();
        return;
      }
      // Nothing to do as the column is non-null.
      return;
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

bool QueryPlanBuilder::TrySortedConstraint(
    const FilterSpec& fs,
    const StorageType& ct,
    const NonNullOp& op,
    const bytecode::reg::RwHandle<CastFilterValueResult>& result) {
  const auto& col = columns_[fs.col];
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
  PERFETTO_CHECK(
      std::holds_alternative<bytecode::reg::RwHandle<Range>>(indices_reg_));
  const auto& reg =
      base::unchecked_get<bytecode::reg::RwHandle<Range>>(indices_reg_);

  // Handle set id equality with a specialized opcode.
  if (ct.Is<Uint32>() && col.sort_state.Is<SetIdSorted>() && op.Is<Eq>()) {
    using B = bytecode::Uint32SetIdSortedEq;
    auto& bc = AddOpcode<B>(RowCountModifier{DoubleLog2RowCount{}});
    bc.arg<B::val_register>() = result;
    bc.arg<B::update_register>() = reg;
    return true;
  }
  const auto& [bound, erlbub] = GetSortedFilterArgs(*range_op);

  RowCountModifier modifier;
  if (ct.Is<Id>()) {
    if (op.Is<Eq>()) {
      modifier = OneRowCount{};
    } else {
      modifier = DoubleLog2RowCount{};
    }
  } else if (op.Is<Eq>()) {
    modifier = DoubleLog2RowCount{};
  } else {
    modifier = Div2RowCount{};
  }
  {
    using B = bytecode::SortedFilterBase;
    auto& bc =
        AddOpcode<B>(bytecode::Index<bytecode::SortedFilter>(ct, erlbub),
                     modifier, bytecode::SortedFilterBase::EstimateCost(ct));
    bc.arg<B::col>() = fs.col;
    bc.arg<B::val_register>() = result;
    bc.arg<B::update_register>() = reg;
    bc.arg<B::write_result_to>() = bound;
  }
  return true;
}

bytecode::reg::RwHandle<Span<uint32_t>>
QueryPlanBuilder::MaybeAddOverlayTranslation(const FilterSpec& c) {
  bytecode::reg::RwHandle<Span<uint32_t>> main = EnsureIndicesAreInSlab();
  const auto& col = columns_[c.col];
  uint32_t nullability_type_index = col.null_storage.nullability().index();
  switch (nullability_type_index) {
    case Nullability::GetTypeIndex<SparseNull>(): {
      bytecode::reg::RwHandle<Slab<uint32_t>> scratch_slab{register_count_++};
      bytecode::reg::RwHandle<Span<uint32_t>> scratch_span{register_count_++};
      {
        using B = bytecode::NullFilter<IsNotNull>;
        bytecode::NullFilterBase& bc = AddOpcode<B>(DoubleLog2RowCount{});
        bc.arg<B::col>() = c.col;
        bc.arg<B::update_register>() = main;
      }
      {
        using B = bytecode::AllocateIndices;
        auto& bc = AddOpcode<B>(UnchangedRowCount{});
        bc.arg<B::size>() = plan_.params.max_row_count;
        bc.arg<B::dest_slab_register>() = scratch_slab;
        bc.arg<B::dest_span_register>() = scratch_span;
      }
      auto popcount_reg = PrefixPopcountRegisterFor(c.col);
      {
        using B = bytecode::TranslateSparseNullIndices;
        auto& bc = AddOpcode<B>(UnchangedRowCount{});
        bc.arg<B::col>() = c.col;
        bc.arg<B::popcount_register>() = popcount_reg;
        bc.arg<B::source_register>() = main;
        bc.arg<B::update_register>() = scratch_span;
      }
      return scratch_span;
    }
    case Nullability::GetTypeIndex<DenseNull>(): {
      using B = bytecode::NullFilter<IsNotNull>;
      bytecode::NullFilterBase& bc = AddOpcode<B>(DoubleLog2RowCount{});
      bc.arg<B::col>() = c.col;
      bc.arg<B::update_register>() = main;
      return main;
    }
    case Nullability::GetTypeIndex<NonNull>():
      return main;
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

PERFETTO_NO_INLINE bytecode::reg::RwHandle<Span<uint32_t>>
QueryPlanBuilder::EnsureIndicesAreInSlab() {
  using SpanReg = bytecode::reg::RwHandle<Span<uint32_t>>;
  using SlabReg = bytecode::reg::RwHandle<Slab<uint32_t>>;

  if (PERFETTO_LIKELY(std::holds_alternative<SpanReg>(indices_reg_))) {
    return base::unchecked_get<SpanReg>(indices_reg_);
  }

  using RegRange = bytecode::reg::RwHandle<Range>;
  PERFETTO_DCHECK(std::holds_alternative<RegRange>(indices_reg_));
  auto range_reg = base::unchecked_get<RegRange>(indices_reg_);

  SlabReg slab_reg{register_count_++};
  SpanReg span_reg{register_count_++};
  {
    using B = bytecode::AllocateIndices;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::size>() = plan_.params.max_row_count;
    bc.arg<B::dest_slab_register>() = slab_reg;
    bc.arg<B::dest_span_register>() = span_reg;
  }
  {
    using B = bytecode::Iota;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::source_register>() = range_reg;
    bc.arg<B::update_register>() = span_reg;
  }
  indices_reg_ = span_reg;
  return span_reg;
}

PERFETTO_NO_INLINE bytecode::Bytecode& QueryPlanBuilder::AddRawOpcode(
    uint32_t option,
    RowCountModifier rc,
    bytecode::Cost cost) {
  switch (cost.index()) {
    case base::variant_index<bytecode::Cost, bytecode::FixedCost>(): {
      const auto& c = base::unchecked_get<bytecode::FixedCost>(cost);
      plan_.params.estimated_cost += c.cost;
      break;
    }
    case base::variant_index<bytecode::Cost, bytecode::LogPerRowCost>(): {
      const auto& c = base::unchecked_get<bytecode::LogPerRowCost>(cost);
      plan_.params.estimated_cost += c.cost * log2(plan_.params.estimated_cost);
      break;
    }
    case base::variant_index<bytecode::Cost, bytecode::LinearPerRowCost>(): {
      const auto& c = base::unchecked_get<bytecode::LinearPerRowCost>(cost);
      plan_.params.estimated_cost += c.cost * plan_.params.estimated_cost;
      break;
    }
    case base::variant_index<bytecode::Cost, bytecode::LogLinearPerRowCost>(): {
      const auto& c = base::unchecked_get<bytecode::LogLinearPerRowCost>(cost);
      plan_.params.estimated_cost += c.cost * plan_.params.estimated_cost *
                                     log2(plan_.params.estimated_cost);
      break;
    }
    case base::variant_index<bytecode::Cost,
                             bytecode::PostOperationLinearPerRowCost>():
      break;
    default:
      PERFETTO_FATAL("Unknown cost type");
  }
  switch (rc.index()) {
    case base::variant_index<RowCountModifier, UnchangedRowCount>():
      break;
    case base::variant_index<RowCountModifier, Div2RowCount>():
      plan_.params.estimated_row_count =
          std::min(std::max(1u, plan_.params.estimated_row_count / 2),
                   plan_.params.estimated_row_count);
      break;
    case base::variant_index<RowCountModifier, DoubleLog2RowCount>(): {
      double new_count = plan_.params.estimated_row_count /
                         (2 * log2(plan_.params.estimated_row_count));
      plan_.params.estimated_row_count =
          std::min(std::max(1u, static_cast<uint32_t>(new_count)),
                   plan_.params.estimated_row_count);
      break;
    }
    case base::variant_index<RowCountModifier, OneRowCount>():
      plan_.params.estimated_row_count =
          std::min(1u, plan_.params.estimated_row_count);
      plan_.params.max_row_count = std::min(1u, plan_.params.max_row_count);
      break;
    case base::variant_index<RowCountModifier, ZeroRowCount>():
      plan_.params.estimated_row_count = 0;
      plan_.params.max_row_count = 0;
      break;
    case base::variant_index<RowCountModifier, LimitOffsetRowCount>(): {
      const auto& lc = base::unchecked_get<LimitOffsetRowCount>(rc);

      // Offset will cut out `offset` rows from the start of indices.
      uint32_t remove_from_start =
          std::min(plan_.params.max_row_count, lc.offset);
      plan_.params.max_row_count -= remove_from_start;

      // Limit will only preserve at most `limit` rows.
      plan_.params.max_row_count =
          std::min(lc.limit, plan_.params.max_row_count);

      // The max row count is also the best possible estimate we can make for
      // the row count.
      plan_.params.estimated_row_count = plan_.params.max_row_count;
      break;
    }
    default:
      PERFETTO_FATAL("Unknown row count modifier type");
  }
  // Handle all the cost types which need to be calculated *post* the row
  // estimate update.
  if (cost.index() ==
      base::variant_index<bytecode::Cost,
                          bytecode::PostOperationLinearPerRowCost>()) {
    const auto& c =
        base::unchecked_get<bytecode::PostOperationLinearPerRowCost>(cost);
    plan_.params.estimated_cost += c.cost * plan_.params.estimated_cost;
  }
  plan_.bytecode.emplace_back();
  plan_.bytecode.back().option = option;
  return plan_.bytecode.back();
}

void QueryPlanBuilder::SetGuaranteedToBeEmpty() {
  bytecode::reg::RwHandle<Slab<uint32_t>> slab_reg{register_count_++};
  bytecode::reg::RwHandle<Span<uint32_t>> span_reg{register_count_++};
  {
    using B = bytecode::AllocateIndices;
    auto& bc = AddOpcode<B>(ZeroRowCount{});
    bc.arg<B::size>() = 0;
    bc.arg<B::dest_slab_register>() = slab_reg;
    bc.arg<B::dest_span_register>() = span_reg;
  }
  indices_reg_ = span_reg;
}

bytecode::reg::ReadHandle<Slab<uint32_t>>
QueryPlanBuilder::PrefixPopcountRegisterFor(uint32_t col) {
  auto& reg = column_states_[col].prefix_popcount;
  if (!reg) {
    reg = bytecode::reg::RwHandle<Slab<uint32_t>>{register_count_++};
    {
      using B = bytecode::PrefixPopcount;
      auto& bc = AddOpcode<B>(UnchangedRowCount{});
      bc.arg<B::col>() = col;
      bc.arg<B::dest_register>() = *reg;
    }
  }
  return *reg;
}

bool QueryPlanBuilder::CanUseMinMaxOptimization(
    const std::vector<SortSpec>& sort_specs,
    const LimitSpec& limit_spec) {
  return sort_specs.size() == 1 &&
         columns_[sort_specs[0].col].null_storage.nullability().Is<NonNull>() &&
         limit_spec.limit == 1 && limit_spec.offset.value_or(0) == 0;
}

}  // namespace perfetto::trace_processor::dataframe::impl
