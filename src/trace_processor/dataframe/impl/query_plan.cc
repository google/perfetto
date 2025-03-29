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
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"

namespace perfetto::trace_processor::dataframe::impl {

namespace {

// Calculates filter preference score for ordering filters.
// Lower scores are applied first for better efficiency.
uint32_t FilterPreference(const FilterSpec& fs, const ColumnSpec& col) {
  enum AbsolutePreference : uint8_t {
    kIndexAsValueEq,  // Most efficient: index-as-value equality check
    kLeastPreferred,  // Least preferred
  };
  const auto& op = fs.op;
  const auto& ct = col.column_type;
  const auto& n = col.nullability;

  // IndexAsValue columns with non-null equality comparison are most efficient
  if (n.Is<NonNull>() && ct.Is<Id>() && op.Is<Eq>()) {
    return kIndexAsValueEq;
  }
  return kLeastPreferred;
}

// Gets the appropriate bound modifier and range operation type
// for a given range operation.
std::pair<BoundModifier, EqualRangeLowerBoundUpperBound> GetSortedFilterArgs(
    const RangeOp& op) {
  switch (op.index()) {
    case EqualRangeLowerBoundUpperBound::GetTypeIndex<EqualRange>():
      return std::make_pair(BothBounds{}, EqualRange{});
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

}  // namespace

QueryPlanBuilder::QueryPlanBuilder(uint32_t row_count,
                                   const std::vector<Column>& columns)
    : max_row_count_(row_count),
      columns_(columns),
      column_states_(columns.size()) {
  // Initialize with a range covering all rows
  bytecode::reg::RwHandle<Range> range{register_count_++};
  {
    using B = bytecode::InitRange;
    auto& ir = AddOpcode<B>();
    ir.arg<B::size>() = max_row_count_;
    ir.arg<B::dest_register>() = range;
  }
  indices_reg_ = range;
}

void QueryPlanBuilder::Filter(std::vector<FilterSpec>& specs) {
  // Sort filters by efficiency (most selective/cheapest first)
  std::sort(specs.begin(), specs.end(),
            [this](const FilterSpec& a, const FilterSpec& b) {
              const auto& a_col = columns_[a.column_index];
              const auto& b_col = columns_[b.column_index];
              return std::make_tuple(FilterPreference(a, a_col.spec),
                                     a.column_index) <
                     std::make_tuple(FilterPreference(b, b_col.spec),
                                     b.column_index);
            });

  // Apply each filter in the optimized order
  for (FilterSpec& c : specs) {
    const Column& col = columns_[c.column_index];
    const auto& ct = col.spec.column_type;

    // Get the non-null operation (all our ops are non-null at this point)
    auto non_null_op = c.op.TryDowncast<NonNullOp>();
    PERFETTO_CHECK(non_null_op);

    // Create a register for the coerced filter value
    bytecode::reg::RwHandle<CastFilterValueResult> value_reg{register_count_++};
    {
      using B = bytecode::CastFilterValueBase;
      auto& bc = AddOpcode<B>(bytecode::Index<bytecode::CastFilterValue>(ct));
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

    PERFETTO_FATAL("Unreachable");
  }
}

void QueryPlanBuilder::Output(uint64_t cols_used) {
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
    switch (col.spec.nullability.index()) {
      case Nullability::GetTypeIndex<NonNull>():
        // For non-null columns, we can directly use the indices
        plan_.params.col_to_output_offset[i] = 0;
        break;
      default:
        PERFETTO_FATAL("Unreachable");
    }
  }

  // Ensure indices are in a slab for efficient access
  auto in_memory_indices = EnsureIndicesAreInSlab();
  bytecode::reg::ReadHandle<Span<uint32_t>> storage_indices_register;

  // Handle multi-column output if needed
  if (plan_.params.output_per_row > 1) {
    // Allocate storage for expanded indices
    bytecode::reg::RwHandle<Slab<uint32_t>> slab_register{register_count_++};
    bytecode::reg::RwHandle<Span<uint32_t>> span_register{register_count_++};
    {
      using B = bytecode::AllocateIndices;
      auto& bc = AddOpcode<B>();
      bc.arg<B::size>() = max_row_count_ * plan_.params.output_per_row;
      bc.arg<B::dest_slab_register>() = slab_register;
      bc.arg<B::dest_span_register>() = span_register;
    }

    // Expand indices with stride for multi-column access
    {
      using B = bytecode::StrideCopy;
      auto& bc = AddOpcode<B>();
      bc.arg<B::source_register>() = in_memory_indices;
      bc.arg<B::update_register>() = span_register;
      bc.arg<B::stride>() = plan_.params.output_per_row;
      storage_indices_register = span_register;
    }

    // Process nullability for each column if needed
    for (auto [col, offset] : null_cols) {
      const auto& c = columns_[col];
      switch (c.spec.nullability.index()) {
        case Nullability::GetTypeIndex<NonNull>():
        default:
          PERFETTO_FATAL("Unreachable");
      }
    }
  } else {
    // Single column output is simpler
    PERFETTO_CHECK(null_cols.empty());
    storage_indices_register = in_memory_indices;
  }

  // Set the output register
  plan_.params.output_register = storage_indices_register;
}

QueryPlan QueryPlanBuilder::Build() && {
  return std::move(plan_);
}

void QueryPlanBuilder::NonStringConstraint(
    const FilterSpec& c,
    const NonStringType& type,
    const NonStringOp& op,
    const bytecode::reg::ReadHandle<CastFilterValueResult>& result) {
  // Get the source register, applying any needed overlay translation
  auto source = MaybeAddOverlayTranslation(c);

  // Add the filter operation
  {
    using B = bytecode::NonStringFilterBase;
    B& bc = AddOpcode<B>(bytecode::Index<bytecode::NonStringFilter>(type, op));
    bc.arg<B::col>() = c.column_index;
    bc.arg<B::val_register>() = result;
    bc.arg<B::source_register>() = source;
    bc.arg<B::update_register>() = EnsureIndicesAreInSlab();
  }
}

bool QueryPlanBuilder::TrySortedConstraint(
    const FilterSpec& fs,
    const ColumnType& ct,
    const NonNullOp& op,
    const bytecode::reg::RwHandle<CastFilterValueResult>& result) {
  const auto& col = columns_[fs.column_index];
  const auto& n = col.spec.nullability;

  // Only applicable to non-null columns
  if (!n.Is<NonNull>()) {
    return false;
  }

  // Check if operation is a range operation
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

  // Get the appropriate bound modifier and range operation
  const auto& [bound, erlbub] = GetSortedFilterArgs(*range_op);

  // Add the sorted filter operation
  {
    using B = bytecode::SortedFilterBase;
    auto& bc =
        AddOpcode<B>(bytecode::Index<bytecode::SortedFilter>(ct, erlbub));
    bc.arg<B::col>() = fs.column_index;
    bc.arg<B::val_register>() = result;
    bc.arg<B::update_register>() = reg;
    bc.arg<B::write_result_to>() = bound;
  }
  return true;
}

bytecode::reg::RwHandle<Span<uint32_t>>
QueryPlanBuilder::MaybeAddOverlayTranslation(const FilterSpec& c) {
  bytecode::reg::RwHandle<Span<uint32_t>> main = EnsureIndicesAreInSlab();
  const auto& col = columns_[c.column_index];

  // Handle based on nullability type
  switch (col.spec.nullability.index()) {
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
    auto& bc = AddOpcode<B>();
    bc.arg<B::size>() = max_row_count_;
    bc.arg<B::dest_slab_register>() = slab_reg;
    bc.arg<B::dest_span_register>() = span_reg;
  }
  {
    using B = bytecode::Iota;
    auto& bc = AddOpcode<B>();
    bc.arg<B::source_register>() = range_reg;
    bc.arg<B::update_register>() = span_reg;
  }
  indices_reg_ = span_reg;
  return span_reg;
}

template <typename T>
T& QueryPlanBuilder::AddOpcode(uint32_t option) {
  plan_.bytecode.emplace_back();
  plan_.bytecode.back().option = option;
  return reinterpret_cast<T&>(plan_.bytecode.back());
}

void QueryPlanBuilder::SetGuaranteedToBeEmpty() {
  max_row_count_ = 0;

  // Set result size to zero (empty set)
  bytecode::reg::RwHandle<Slab<uint32_t>> slab_reg{register_count_++};
  bytecode::reg::RwHandle<Span<uint32_t>> span_reg{register_count_++};
  {
    using B = bytecode::AllocateIndices;
    auto& bc = AddOpcode<B>();
    bc.arg<B::size>() = 0;
    bc.arg<B::dest_slab_register>() = slab_reg;
    bc.arg<B::dest_span_register>() = span_reg;
  }
  indices_reg_ = span_reg;
}

}  // namespace perfetto::trace_processor::dataframe::impl
