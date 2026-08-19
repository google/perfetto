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

#include "src/trace_processor/core/dataframe/bytecode_lowering.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/variant.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/query_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/interpreter/bytecode_core.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/core/util/type_set.h"

namespace perfetto::trace_processor::core::dataframe {

namespace {

namespace i = interpreter;

// Assumed number of distinct values matched by an IN filter when the list size
// is not known at plan time. Scales the single-value equality estimate. Matches
// SQLite's own tuning constant for "x IN (SELECT ...)" (see whereLoopAddBtree).
constexpr double kAssumedInListSize = 25;

// Rows surviving a scalar equality filter on a HasDuplicates column with
// `estimated_distinct` distinct values (0 = unknown). With a known count, a
// uniform column keeps ~1/estimated_distinct of the rows; otherwise fall back
// to the data-blind heuristic.
double EqualityFilterRows(uint32_t row_count, uint32_t estimated_distinct) {
  if (estimated_distinct > 0) {
    return static_cast<double>(row_count) / estimated_distinct;
  }
  return row_count / (2 * log2(row_count));
}

// Register type identifiers for cache key encoding.
// Used with DataframeRegisterCache::GetOrAllocate(reg_type, ptr)
// to cache column/index-specific registers.
enum RegType : uint32_t {
  kStorageReg = 0,
  kNullBvReg = 1,
  kSmallValueEqBvReg = 2,
  kSmallValueEqPopcountReg = 3,
  kIndexReg = 4,
  kRegTypeCount = 5,
};

// TypeSet of all possible sparse nullability states.
using SparseNullTypes = TypeSet<SparseNull,
                                SparseNullWithPopcountAlways,
                                SparseNullWithPopcountUntilFinalization>;

// Gets the appropriate bound modifier and range operation type
// for a given range operation.
std::pair<i::BoundModifier, i::EqualRangeLowerBoundUpperBound>
GetSortedFilterArgs(const RangeOp& op) {
  switch (op.index()) {
    case RangeOp::GetTypeIndex<Eq>():
      return std::make_pair(i::BothBounds{}, i::EqualRange{});
    case RangeOp::GetTypeIndex<Lt>():
      return std::make_pair(i::EndBound{}, i::LowerBound{});
    case RangeOp::GetTypeIndex<Le>():
      return std::make_pair(i::EndBound{}, i::UpperBound{});
    case RangeOp::GetTypeIndex<Gt>():
      return std::make_pair(i::BeginBound{}, i::UpperBound{});
    case RangeOp::GetTypeIndex<Ge>():
      return std::make_pair(i::BeginBound{}, i::LowerBound{});
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

// Helper to get byte size of storage types for layout calculation.
uint8_t GetDataSize(StorageType type) {
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

i::SparseNullCollapsedNullability NullabilityToSparseNullCollapsedNullability(
    Nullability nullability) {
  switch (nullability.index()) {
    case Nullability::GetTypeIndex<NonNull>():
      return NonNull{};
    case Nullability::GetTypeIndex<DenseNull>():
      return DenseNull{};
    case Nullability::GetTypeIndex<SparseNull>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountAlways>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountUntilFinalization>():
      return SparseNull{};
    default:
      PERFETTO_FATAL("Invalid nullability type");
  }
}

}  // namespace

BytecodeLowering::BytecodeLowering(
    uint32_t row_count,
    const std::vector<std::shared_ptr<Column>>& columns,
    const std::vector<Index>& indexes)
    : columns_(columns),
      indexes_(indexes),
      cache_(builder_),
      indices_reg_(builder_.AllocateRegister<Range>()) {
  // Setup the maximum and estimated row counts.
  plan_.params.max_row_count = row_count;
  plan_.params.estimated_row_count = row_count;

  // Initialize with a range covering all rows.
  using B = i::InitRange;
  auto& ir = builder_.AddOpcode<B>(i::Index<B>());
  ir.arg<B::size>() = row_count;
  ir.arg<B::dest_register>() =
      base::unchecked_get<i::RwHandle<Range>>(indices_reg_);
}

QueryPlanImpl BytecodeLowering::Build() && {
  plan_.bytecode = std::move(builder_.bytecode());
  plan_.params.register_count = builder_.register_count();
  return std::move(plan_);
}

uint32_t BytecodeLowering::ReserveFilterValueSlot(FilterSpec& spec) {
  uint32_t slot = plan_.params.filter_value_count++;
  spec.value_index = slot;
  return slot;
}

i::ReadHandle<i::CastFilterValueResult> BytecodeLowering::EmitCastFilterValue(
    FilterSpec& spec,
    const StorageType& type,
    NonNullOp op) {
  i::RwHandle<i::CastFilterValueResult> value_reg =
      builder_.AllocateRegister<i::CastFilterValueResult>();
  {
    using B = i::CastFilterValueBase;
    auto& bc =
        AddOpcode<B>(i::Index<i::CastFilterValue>(type), UnchangedRowCount{});
    bc.arg<B::fval_handle>() = {plan_.params.filter_value_count};
    bc.arg<B::write_register>() = value_reg;
    bc.arg<B::op>() = op;
    spec.value_index = plan_.params.filter_value_count++;
  }
  return value_reg;
}

i::RwHandle<std::unique_ptr<i::CastFilterValueListResult>>
BytecodeLowering::EmitCastFilterValueList(FilterSpec& spec,
                                          const StorageType& type) {
  i::RwHandle<std::unique_ptr<i::CastFilterValueListResult>> value =
      builder_
          .AllocateRegister<std::unique_ptr<i::CastFilterValueListResult>>();
  {
    using B = i::CastFilterValueListBase;
    auto& bc = AddOpcode<B>(i::Index<i::CastFilterValueList>(type),
                            UnchangedRowCount{});
    bc.arg<B::fval_handle>() = {plan_.params.filter_value_count};
    bc.arg<B::write_register>() = value;
    bc.arg<B::op>() = Eq{};
    spec.value_index = plan_.params.filter_value_count++;
  }
  return value;
}

void BytecodeLowering::EmitSortedFilter(
    const FilterSpec& spec,
    const StorageType& type,
    const RangeOp& op,
    const i::ReadHandle<i::CastFilterValueResult>& value) {
  const auto& col = GetColumn(spec.col);

  // We should have ordered the constraints such that we only reach this
  // point with range indices.
  PERFETTO_CHECK(std::holds_alternative<i::RwHandle<Range>>(indices_reg_));
  const auto& reg = base::unchecked_get<i::RwHandle<Range>>(indices_reg_);

  const auto& [bound, erlbub] = GetSortedFilterArgs(op);
  RowCountModifier modifier;
  if (op.Is<Eq>()) {
    modifier =
        EqualityFilterRowCount{col.duplicate_state, col.estimated_distinct};
  } else {
    modifier = NonEqualityFilterRowCount{};
  }
  {
    using B = i::SortedFilterBase;
    auto& bc = AddOpcode<B>(i::Index<i::SortedFilter>(type, erlbub), modifier,
                            i::SortedFilterBase::EstimateCost(type));
    bc.arg<B::storage_register>() = StorageRegisterFor(spec.col, type);
    bc.arg<B::val_register>() = value;
    bc.arg<B::update_register>() = reg;
    bc.arg<B::write_result_to>() = bound;
  }
}

void BytecodeLowering::EmitSetIdSortedEq(
    const FilterSpec& spec,
    const i::ReadHandle<i::CastFilterValueResult>& value) {
  const auto& col = GetColumn(spec.col);
  PERFETTO_CHECK(std::holds_alternative<i::RwHandle<Range>>(indices_reg_));
  const auto& reg = base::unchecked_get<i::RwHandle<Range>>(indices_reg_);

  using B = i::Uint32SetIdSortedEq;
  auto& bc = AddOpcode<B>(RowCountModifier{
      EqualityFilterRowCount{col.duplicate_state, col.estimated_distinct}});
  bc.arg<B::storage_register>() =
      StorageRegisterFor(spec.col, col.storage.type());
  bc.arg<B::val_register>() = value;
  bc.arg<B::update_register>() = reg;
}

void BytecodeLowering::EmitSmallValueEq(
    const FilterSpec& spec,
    const i::ReadHandle<i::CastFilterValueResult>& value) {
  const auto& col = GetColumn(spec.col);
  PERFETTO_CHECK(std::holds_alternative<i::RwHandle<Range>>(indices_reg_));
  const auto& reg = base::unchecked_get<i::RwHandle<Range>>(indices_reg_);

  using B = i::SpecializedStorageSmallValueEq;
  auto& bc = AddOpcode<B>(RowCountModifier{
      EqualityFilterRowCount{col.duplicate_state, col.estimated_distinct}});
  bc.arg<B::small_value_bv_register>() = SmallValueEqBvRegisterFor(spec.col);
  bc.arg<B::small_value_popcount_register>() =
      SmallValueEqPopcountRegisterFor(spec.col);
  bc.arg<B::val_register>() = value;
  bc.arg<B::update_register>() = reg;
}

void BytecodeLowering::EmitLinearFilterEq(
    const FilterSpec& spec,
    const NonIdStorageType& type,
    const i::ReadHandle<i::CastFilterValueResult>& value) {
  const auto& col = GetColumn(spec.col);
  PERFETTO_DCHECK(std::holds_alternative<i::RwHandle<Range>>(indices_reg_));
  PERFETTO_DCHECK(col.null_storage.nullability().Is<NonNull>());
  PERFETTO_DCHECK(spec.op.Is<Eq>());

  using SpanReg = i::RwHandle<Span<uint32_t>>;
  using SlabReg = i::RwHandle<Slab<uint32_t>>;
  using RegRange = i::RwHandle<Range>;

  auto range_reg = base::unchecked_get<RegRange>(indices_reg_);
  SlabReg slab_reg = builder_.AllocateRegister<Slab<uint32_t>>();
  SpanReg span_reg = builder_.AllocateRegister<Span<uint32_t>>();
  {
    using B = i::AllocateIndices;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::size>() = plan_.params.max_row_count;
    bc.arg<B::dest_slab_register>() = slab_reg;
    bc.arg<B::dest_span_register>() = span_reg;
  }
  {
    using B = i::LinearFilterEqBase;
    B& bc = AddOpcode<B>(i::Index<i::LinearFilterEq>(type),
                         RowCountModifier{EqualityFilterRowCount{
                             col.duplicate_state, col.estimated_distinct}});
    bc.arg<B::storage_register>() =
        StorageRegisterFor(spec.col, type.Upcast<StorageType>());
    bc.arg<B::filter_value_reg>() = value;
    bc.arg<B::source_register>() = range_reg;
    bc.arg<B::update_register>() = span_reg;
  }
  indices_reg_ = span_reg;
}

void BytecodeLowering::EmitNonStringFilter(
    const FilterSpec& spec,
    const NonStringType& type,
    const NonStringOp& op,
    const i::ReadHandle<i::CastFilterValueResult>& value) {
  const auto& col = GetColumn(spec.col);
  auto update = EnsureIndicesAreInSlab();
  PruneNullIndices(spec.col, update, NullPruneScope::kResultRows);
  auto source = TranslateNonNullIndices(spec.col, update, false);
  {
    using B = i::NonStringFilterBase;
    B& bc = AddOpcode<B>(i::Index<i::NonStringFilter>(type, op),
                         op.Is<Eq>()
                             ? RowCountModifier{EqualityFilterRowCount{
                                   col.duplicate_state, col.estimated_distinct}}
                             : RowCountModifier{NonEqualityFilterRowCount{}});
    bc.arg<B::storage_register>() =
        StorageRegisterFor(spec.col, type.Upcast<StorageType>());
    bc.arg<B::val_register>() = value;
    bc.arg<B::source_register>() = source;
    bc.arg<B::update_register>() = update;
  }
  MaybeReleaseScratchSpanRegister();
}

void BytecodeLowering::EmitStringFilter(
    const FilterSpec& spec,
    const StringOp& op,
    const i::ReadHandle<i::CastFilterValueResult>& value) {
  const auto& col = GetColumn(spec.col);
  auto update = EnsureIndicesAreInSlab();
  PruneNullIndices(spec.col, update, NullPruneScope::kResultRows);
  auto source = TranslateNonNullIndices(spec.col, update, false);
  {
    using B = i::StringFilterBase;
    B& bc = AddOpcode<B>(i::Index<i::StringFilter>(op),
                         op.Is<Eq>()
                             ? RowCountModifier{EqualityFilterRowCount{
                                   col.duplicate_state, col.estimated_distinct}}
                             : RowCountModifier{NonEqualityFilterRowCount{}});
    bc.arg<B::storage_register>() = StorageRegisterFor(spec.col, String{});
    bc.arg<B::val_register>() = value;
    bc.arg<B::source_register>() = source;
    bc.arg<B::update_register>() = update;
  }
  MaybeReleaseScratchSpanRegister();
}

void BytecodeLowering::EmitNullFilter(const FilterSpec& spec,
                                      const NullOp& op) {
  auto indices = EnsureIndicesAreInSlab();
  using B = i::NullFilterBase;
  B& bc =
      AddOpcode<B>(i::Index<i::NullFilter>(op), NonEqualityFilterRowCount{});
  bc.arg<B::null_bv_register>() = NullBitvectorRegisterFor(spec.col);
  bc.arg<B::update_register>() = indices;
}

void BytecodeLowering::EmitScanFilterIn(
    const FilterSpec& spec,
    i::RwHandle<std::unique_ptr<i::CastFilterValueListResult>> values) {
  const auto& col = GetColumn(spec.col);
  auto update = EnsureIndicesAreInSlab();
  PruneNullIndices(spec.col, update, NullPruneScope::kResultRows);
  auto source = TranslateNonNullIndices(spec.col, update, false);
  {
    using B = i::FilterInBase;
    B& bc = AddOpcode<B>(
        i::Index<i::FilterIn>(col.storage.type(),
                              i::SparseNullCollapsedNullability{NonNull{}}),
        RowCountModifier{
            InFilterRowCount{col.duplicate_state, col.estimated_distinct}});
    bc.arg<B::storage_register>() =
        StorageRegisterFor(spec.col, col.storage.type());
    bc.arg<B::null_bv_register>() = {};
    bc.arg<B::value_list_register>() = values;
    bc.arg<B::index_register>() = {};
    bc.arg<B::source_range_register>() = {};
    bc.arg<B::source_register>() = source;
    bc.arg<B::dest_register>() = update;
  }
  MaybeReleaseScratchSpanRegister();
}

void BytecodeLowering::EmitIndexedFilters(
    std::vector<FilterSpec>& specs,
    uint32_t index_idx,
    const std::vector<uint32_t>& spec_idxs) {
  i::RwHandle<Span<uint32_t>> source_reg = IndexRegisterFor(index_idx);
  i::RwHandle<Span<uint32_t>> dest_reg =
      builder_.AllocateRegister<Span<uint32_t>>();

  // The current row range — used by FilterIn's scan fallback when the IN list
  // is too large for binary search and the index is ignored.
  PERFETTO_CHECK(std::holds_alternative<i::RwHandle<Range>>(indices_reg_));
  const auto& range_reg = base::unchecked_get<i::RwHandle<Range>>(indices_reg_);

  // Allocate the output indices upfront. For In filters, FilterIn writes
  // directly here and CopySpanIntersectingRange runs in-place (safe because
  // its write pointer never advances past its read pointer). For Eq-only
  // filters, dest_reg points into the source index and
  // CopySpanIntersectingRange copies from there into this buffer.
  i::RwHandle<Slab<uint32_t>> output_slab_reg =
      builder_.AllocateRegister<Slab<uint32_t>>();
  i::RwHandle<Span<uint32_t>> output_span_reg =
      builder_.AllocateRegister<Span<uint32_t>>();
  {
    using B = i::AllocateIndices;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::size>() = plan_.params.max_row_count;
    bc.arg<B::dest_slab_register>() = output_slab_reg;
    bc.arg<B::dest_span_register>() = output_span_reg;
  }

  for (uint32_t spec_idx : spec_idxs) {
    FilterSpec& fs = specs[spec_idx];
    const Column& column = GetColumn(fs.col);
    auto non_id = column.storage.type().TryDowncast<NonIdStorageType>();
    PERFETTO_CHECK(non_id);

    if (fs.op.Is<In>()) {
      // Emit IndexedFilterIn for In filters.
      auto value_list_reg = EmitCastFilterValueList(fs, column.storage.type());
      // IndexedFilterIn gathers results from non-contiguous ranges, so it
      // cannot write into the source (which points to the persistent index
      // permutation vector). Write directly into the output span allocated
      // upfront; CopySpanIntersectingRange will then run in-place on it.
      {
        using B = i::FilterInBase;
        auto null_bv_reg = EnsurePrefixPopcountFor(fs.col);
        auto& bc = AddOpcode<B>(
            i::Index<i::FilterIn>(non_id->Upcast<StorageType>(),
                                  NullabilityToSparseNullCollapsedNullability(
                                      column.null_storage.nullability())),
            RowCountModifier{InFilterRowCount{column.duplicate_state,
                                              column.estimated_distinct}},
            i::LogPerRowCost{10});
        bc.arg<B::storage_register>() =
            StorageRegisterFor(fs.col, non_id->Upcast<StorageType>());
        bc.arg<B::null_bv_register>() = null_bv_reg;
        bc.arg<B::value_list_register>() = value_list_reg;
        bc.arg<B::index_register>() = source_reg;
        bc.arg<B::source_range_register>() = range_reg;
        bc.arg<B::source_register>() = {};
        bc.arg<B::dest_register>() = output_span_reg;
      }
      // Override dest_reg so subsequent filters use the output span.
      dest_reg = output_span_reg;
    } else {
      // Emit IndexedFilterEq for Eq filters.
      auto value_reg = EmitCastFilterValue(fs, column.storage.type(),
                                           *fs.op.TryDowncast<NonNullOp>());
      {
        using B = i::IndexedFilterEqBase;
        auto null_bv_reg = EnsurePrefixPopcountFor(fs.col);
        auto& bc = AddOpcode<B>(
            i::Index<i::IndexedFilterEq>(
                *non_id, NullabilityToSparseNullCollapsedNullability(
                             column.null_storage.nullability())),
            RowCountModifier{EqualityFilterRowCount{
                column.duplicate_state, column.estimated_distinct}});
        bc.arg<B::storage_register>() =
            StorageRegisterFor(fs.col, non_id->Upcast<StorageType>());
        bc.arg<B::null_bv_register>() = null_bv_reg;
        bc.arg<B::filter_value_reg>() = value_reg;
        bc.arg<B::source_register>() = source_reg;
        bc.arg<B::dest_register>() = dest_reg;
      }
    }
    // After first filter, subsequent filters read from dest and write back to
    // dest.
    source_reg = dest_reg;
  }

  PERFETTO_CHECK(std::holds_alternative<i::RwHandle<Range>>(indices_reg_));
  const auto& indices_reg =
      base::unchecked_get<i::RwHandle<Range>>(indices_reg_);

  {
    using B = i::CopySpanIntersectingRange;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::source_register>() = dest_reg;
    bc.arg<B::source_range_register>() = indices_reg;
    bc.arg<B::update_register>() = output_span_reg;
  }
  indices_reg_ = output_span_reg;
}

void BytecodeLowering::EmitGuaranteedEmpty() {
  i::RwHandle<Slab<uint32_t>> slab_reg =
      builder_.AllocateRegister<Slab<uint32_t>>();
  i::RwHandle<Span<uint32_t>> span_reg =
      builder_.AllocateRegister<Span<uint32_t>>();
  {
    using B = i::AllocateIndices;
    auto& bc = AddOpcode<B>(ZeroRowCount{});
    bc.arg<B::size>() = 0;
    bc.arg<B::dest_slab_register>() = slab_reg;
    bc.arg<B::dest_span_register>() = span_reg;
  }
  indices_reg_ = span_reg;
}

void BytecodeLowering::EmitDistinct(const std::vector<DistinctSpec>& specs) {
  std::vector<RowLayoutParams> row_layout_params;
  row_layout_params.reserve(specs.size());
  for (const auto& spec : specs) {
    row_layout_params.push_back({spec.col, false});
  }
  uint16_t total_row_stride = CalculateRowLayoutStride(row_layout_params);
  i::RwHandle<Span<uint32_t>> indices = EnsureIndicesAreInSlab();
  auto buffer_reg =
      CopyToRowLayout(total_row_stride, indices, {}, row_layout_params);
  {
    using B = i::Distinct;
    auto& bc = AddOpcode<B>(NonEqualityFilterRowCount{});
    bc.arg<B::buffer_register>() = buffer_reg;
    bc.arg<B::total_row_stride>() = total_row_stride;
    bc.arg<B::indices_register>() = indices;
  }
}

void BytecodeLowering::EmitReverse() {
  auto indices = EnsureIndicesAreInSlab();
  using B = i::Reverse;
  auto& op = AddOpcode<B>(UnchangedRowCount{});
  op.arg<B::update_register>() = indices;
}

void BytecodeLowering::EmitSort(const std::vector<SortSpec>& sort_specs) {
  // main_indices_span will be modified by the final sort operation.
  // EnsureIndicesAreInSlab makes it an RwHandle.
  i::RwHandle<Span<uint32_t>> indices = EnsureIndicesAreInSlab();

  bool has_string_sort_keys = false;
  for (const auto& spec : sort_specs) {
    if (GetColumn(spec.col).storage.type().Is<String>()) {
      has_string_sort_keys = true;
      break;
    }
  }

  using Map = i::StringIdToRankMap;
  i::RwHandle<Map> string_rank_map;
  if (has_string_sort_keys) {
    string_rank_map = builder_.AllocateRegister<Map>();
    {
      using B = i::InitRankMap;
      auto& op = AddOpcode<B>(UnchangedRowCount{});
      op.arg<B::dest_register>() = string_rank_map;
    }

    // For each string column in the sort specification, collect its unique IDs.
    // This involves preparing a temporary set of indices for that column which
    // are non-null and translated to storage indices if originally sparse.
    for (const auto& spec : sort_specs) {
      const Column& col = GetColumn(spec.col);
      if (!col.storage.type().Is<String>()) {
        continue;
      }

      i::RwHandle<Span<uint32_t>> translated;
      if (col.null_storage.nullability().Is<NonNull>()) {
        // If the column is non-null, we can use the main indices directly.
        translated = indices;
      } else {
        // Get a scratch register to prepare indices for this specific column.
        // This ensures that the main_indices_span is not modified, allowing
        // each string column to be processed independently from the original
        // set of rows.
        i::RwHandle<Span<uint32_t>> scratch =
            GetOrCreateScratchSpanRegister(plan_.params.max_row_count);

        // 1. Copy the current indices to our temporary scratch span.
        {
          auto& op = AddOpcode<i::StrideCopy>(UnchangedRowCount{});
          op.arg<i::StrideCopy::source_register>() = indices;
          op.arg<i::StrideCopy::update_register>() = scratch;
          op.arg<i::StrideCopy::stride>() = 1;
        }

        // 2. Prune nulls from this temporary span in-place.
        PruneNullIndices(spec.col, scratch, NullPruneScope::kScratchOnly);

        // 3. Translate these non-null table indices to storage indices if
        // necessary.
        translated = TranslateNonNullIndices(spec.col, scratch, true);
        PERFETTO_CHECK(translated.index == scratch.index);
      }

      // Collect IDs using the prepared (non-null, translated) indices.
      {
        using B = i::CollectIdIntoRankMap;
        auto& op = AddOpcode<B>(UnchangedRowCount{});
        op.arg<B::storage_register>() =
            StorageRegisterFor(spec.col, col.storage.type());
        op.arg<B::source_register>() = translated;
        op.arg<B::rank_map_register>() = string_rank_map;
      }

      // Maybe release the scratch register if we used one.
      MaybeReleaseScratchSpanRegister();
    }

    // Finalize ranks in the map (sorts keys, updates map values to ranks).
    {
      using B = i::FinalizeRanksInMap;
      auto& op = AddOpcode<B>(UnchangedRowCount{});
      op.arg<B::update_register>() = string_rank_map;
    }
  }

  std::vector<RowLayoutParams> row_layout_params;
  row_layout_params.reserve(sort_specs.size());
  for (const auto& spec : sort_specs) {
    row_layout_params.push_back(
        {spec.col, columns_[spec.col]->storage.type().Is<String>(),
         spec.direction == SortDirection::kDescending});
  }
  uint16_t total_row_stride = CalculateRowLayoutStride(row_layout_params);
  auto buffer_reg = CopyToRowLayout(total_row_stride, indices, string_rank_map,
                                    row_layout_params);
  {
    using B = i::SortRowLayout;
    auto& op = AddOpcode<B>(UnchangedRowCount{});
    op.arg<B::buffer_register>() = buffer_reg;
    op.arg<B::total_row_stride>() = total_row_stride;
    op.arg<B::indices_register>() = indices;
  }
}

void BytecodeLowering::EmitMinMax(const SortSpec& sort_spec) {
  uint32_t col_idx = sort_spec.col;
  const auto& col = GetColumn(col_idx);
  StorageType storage_type = col.storage.type();

  i::MinMaxOp mmop = sort_spec.direction == SortDirection::kAscending
                         ? i::MinMaxOp(i::MinOp{})
                         : i::MinMaxOp(i::MaxOp{});

  auto indices = EnsureIndicesAreInSlab();
  using B = i::FindMinMaxIndexBase;
  auto& op = AddOpcode<B>(i::Index<i::FindMinMaxIndex>(storage_type, mmop),
                          OneRowCount{});
  op.arg<B::update_register>() = indices;
  op.arg<B::storage_register>() = StorageRegisterFor(col_idx, storage_type);
}

void BytecodeLowering::EmitOutput(const LimitSpec& limit, uint64_t cols_used) {
  // Structure to track column and offset pairs
  struct ColAndOffset {
    uint32_t col;
    uint32_t offset;
  };

  base::SmallVector<ColAndOffset, 24> null_cols;
  plan_.params.output_per_row = 1;
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    plan_.col_to_output_offset.emplace_back();
  }

  // Process each column that will be used in the output
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    // Any column with index >= 64 uses the 64th bit in cols_used.
    uint64_t mask = 1ULL << std::min(i, 63u);
    if ((cols_used & mask) == 0) {
      continue;
    }
    const auto& col = GetColumn(i);
    switch (col.null_storage.nullability().index()) {
      case Nullability::GetTypeIndex<SparseNull>():
      case Nullability::GetTypeIndex<SparseNullWithPopcountAlways>():
      case Nullability::GetTypeIndex<SparseNullWithPopcountUntilFinalization>():
      case Nullability::GetTypeIndex<DenseNull>(): {
        uint32_t offset = plan_.params.output_per_row++;
        null_cols.emplace_back(ColAndOffset{i, offset});
        plan_.col_to_output_offset[i] = offset;
        break;
      }
      case Nullability::GetTypeIndex<NonNull>():
        // For non-null columns, we can directly use the indices
        plan_.col_to_output_offset[i] = 0;
        break;
      default:
        PERFETTO_FATAL("Unreachable");
    }
  }

  auto in_memory_indices = EnsureIndicesAreInSlab();
  if (limit.limit || limit.offset) {
    auto o = limit.offset.value_or(0);
    auto l = limit.limit.value_or(std::numeric_limits<uint32_t>::max());
    using B = i::LimitOffsetIndices;
    auto& bc = AddOpcode<B>(LimitOffsetRowCount{l, o});
    bc.arg<B::offset_value>() = o;
    bc.arg<B::limit_value>() = l;
    bc.arg<B::update_register>() = in_memory_indices;
  }

  i::RwHandle<Span<uint32_t>> storage_update_register;
  if (plan_.params.output_per_row > 1) {
    i::RwHandle<Slab<uint32_t>> slab_register =
        builder_.AllocateRegister<Slab<uint32_t>>();
    storage_update_register = builder_.AllocateRegister<Span<uint32_t>>();
    {
      using B = i::AllocateIndices;
      auto& bc = AddOpcode<B>(UnchangedRowCount{});
      bc.arg<B::size>() =
          plan_.params.max_row_count * plan_.params.output_per_row;
      bc.arg<B::dest_slab_register>() = slab_register;
      bc.arg<B::dest_span_register>() = storage_update_register;
    }
    {
      using B = i::StrideCopy;
      auto& bc = AddOpcode<B>(UnchangedRowCount{});
      bc.arg<B::source_register>() = in_memory_indices;
      bc.arg<B::update_register>() = storage_update_register;
      bc.arg<B::stride>() = plan_.params.output_per_row;
    }
    for (auto [col, offset] : null_cols) {
      const auto& c = GetColumn(col);
      switch (c.null_storage.nullability().index()) {
        case Nullability::GetTypeIndex<SparseNull>():
        case Nullability::GetTypeIndex<SparseNullWithPopcountAlways>():
        case Nullability::GetTypeIndex<
            SparseNullWithPopcountUntilFinalization>(): {
          using B = i::StrideTranslateAndCopySparseNullIndices;
          auto null_bv_reg = EnsurePrefixPopcountFor(col);
          auto& bc = AddOpcode<B>(UnchangedRowCount{});
          bc.arg<B::update_register>() = storage_update_register;
          bc.arg<B::null_bv_register>() = null_bv_reg;
          bc.arg<B::offset>() = offset;
          bc.arg<B::stride>() = plan_.params.output_per_row;
          break;
        }
        case Nullability::GetTypeIndex<DenseNull>(): {
          using B = i::StrideCopyDenseNullIndices;
          auto& bc = AddOpcode<B>(UnchangedRowCount{});
          bc.arg<B::update_register>() = storage_update_register;
          bc.arg<B::null_bv_register>() = NullBitvectorRegisterFor(col);
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

void BytecodeLowering::PruneNullIndices(uint32_t col,
                                        i::RwHandle<Span<uint32_t>> indices,
                                        NullPruneScope scope) {
  switch (GetColumn(col).null_storage.nullability().index()) {
    case Nullability::GetTypeIndex<SparseNull>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountAlways>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountUntilFinalization>():
    case Nullability::GetTypeIndex<DenseNull>(): {
      using B = i::NullFilter<IsNotNull>;
      RowCountModifier rc = scope == NullPruneScope::kResultRows
                                ? RowCountModifier{NonEqualityFilterRowCount{}}
                                : RowCountModifier{UnchangedRowCount{}};
      i::NullFilterBase& bc = AddOpcode<B>(rc);
      bc.arg<B::null_bv_register>() = NullBitvectorRegisterFor(col);
      bc.arg<B::update_register>() = indices;
      break;
    }
    case Nullability::GetTypeIndex<NonNull>():
      break;
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

i::RwHandle<Span<uint32_t>> BytecodeLowering::TranslateNonNullIndices(
    uint32_t col,
    i::RwHandle<Span<uint32_t>> table_indices_register,
    bool in_place) {
  switch (GetColumn(col).null_storage.nullability().index()) {
    case Nullability::GetTypeIndex<SparseNull>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountAlways>():
    case Nullability::GetTypeIndex<SparseNullWithPopcountUntilFinalization>(): {
      auto update =
          in_place ? table_indices_register
                   : GetOrCreateScratchSpanRegister(plan_.params.max_row_count);
      {
        auto null_bv_reg = EnsurePrefixPopcountFor(col);
        using B = i::TranslateSparseNullIndices;
        auto& bc = AddOpcode<B>(UnchangedRowCount{});
        bc.arg<B::null_bv_register>() = null_bv_reg;
        bc.arg<B::source_register>() = table_indices_register;
        bc.arg<B::update_register>() = update;
      }
      return update;
    }
    case Nullability::GetTypeIndex<DenseNull>():
    case Nullability::GetTypeIndex<NonNull>():
      return table_indices_register;
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

PERFETTO_NO_INLINE i::RwHandle<Span<uint32_t>>
BytecodeLowering::EnsureIndicesAreInSlab() {
  using SpanReg = i::RwHandle<Span<uint32_t>>;
  using SlabReg = i::RwHandle<Slab<uint32_t>>;

  if (PERFETTO_LIKELY(std::holds_alternative<SpanReg>(indices_reg_))) {
    return base::unchecked_get<SpanReg>(indices_reg_);
  }

  using RegRange = i::RwHandle<Range>;
  PERFETTO_DCHECK(std::holds_alternative<RegRange>(indices_reg_));
  auto range_reg = base::unchecked_get<RegRange>(indices_reg_);

  SlabReg slab_reg = builder_.AllocateRegister<Slab<uint32_t>>();
  SpanReg span_reg = builder_.AllocateRegister<Span<uint32_t>>();
  {
    using B = i::AllocateIndices;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::size>() = plan_.params.max_row_count;
    bc.arg<B::dest_slab_register>() = slab_reg;
    bc.arg<B::dest_span_register>() = span_reg;
  }
  {
    using B = i::Iota;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::source_register>() = range_reg;
    bc.arg<B::update_register>() = span_reg;
  }
  indices_reg_ = span_reg;
  return span_reg;
}

PERFETTO_NO_INLINE i::Bytecode& BytecodeLowering::AddRawOpcode(
    uint32_t option,
    RowCountModifier rc,
    i::Cost cost) {
  static constexpr uint32_t kFixedBytecodeCost = 5;
  switch (cost.index()) {
    case base::variant_index<i::Cost, i::FixedCost>(): {
      const auto& c = base::unchecked_get<i::FixedCost>(cost);
      plan_.params.estimated_cost += c.cost;
      break;
    }
    case base::variant_index<i::Cost, i::LogPerRowCost>(): {
      const auto& c = base::unchecked_get<i::LogPerRowCost>(cost);
      plan_.params.estimated_cost +=
          plan_.params.estimated_row_count == 0
              ? kFixedBytecodeCost
              : c.cost * log2(plan_.params.estimated_row_count);
      break;
    }
    case base::variant_index<i::Cost, i::LinearPerRowCost>(): {
      const auto& c = base::unchecked_get<i::LinearPerRowCost>(cost);
      plan_.params.estimated_cost +=
          plan_.params.estimated_row_count == 0
              ? kFixedBytecodeCost
              : c.cost * plan_.params.estimated_row_count;
      break;
    }
    case base::variant_index<i::Cost, i::LogLinearPerRowCost>(): {
      const auto& c = base::unchecked_get<i::LogLinearPerRowCost>(cost);
      plan_.params.estimated_cost +=
          plan_.params.estimated_row_count == 0
              ? kFixedBytecodeCost
              : c.cost * plan_.params.estimated_row_count *
                    log2(plan_.params.estimated_row_count);
      break;
    }
    case base::variant_index<i::Cost, i::PostOperationLinearPerRowCost>():
      break;
    default:
      PERFETTO_FATAL("Unknown cost type");
  }
  switch (rc.index()) {
    case base::variant_index<RowCountModifier, UnchangedRowCount>():
      break;
    case base::variant_index<RowCountModifier, NonEqualityFilterRowCount>():
      if (plan_.params.estimated_row_count > 1) {
        plan_.params.estimated_row_count = plan_.params.estimated_row_count / 2;
      } else {
        // Leave the estimated row count as is if it is already 1 or less.
      }
      break;
    case base::variant_index<RowCountModifier, EqualityFilterRowCount>(): {
      const auto& eq = base::unchecked_get<EqualityFilterRowCount>(rc);
      if (eq.duplicate_state.Is<HasDuplicates>()) {
        if (plan_.params.estimated_row_count > 1) {
          if (!selective_filter_base_row_count_) {
            selective_filter_base_row_count_ = plan_.params.estimated_row_count;
          }
          // Estimate against the pre-selective-filter row count and keep the
          // most selective result: correlated filters on one scan shouldn't
          // compound and collapse the estimate toward 1.
          plan_.params.estimated_row_count =
              std::min(plan_.params.estimated_row_count,
                       std::max(1u, static_cast<uint32_t>(EqualityFilterRows(
                                        *selective_filter_base_row_count_,
                                        eq.estimated_distinct))));
        } else {
          // Leave the estimated row count as is if it is already 1 or less.
        }
      } else {
        PERFETTO_CHECK(eq.duplicate_state.Is<NoDuplicates>());
        plan_.params.estimated_row_count =
            std::min(1u, plan_.params.estimated_row_count);
        plan_.params.max_row_count = std::min(1u, plan_.params.max_row_count);
      }
      break;
    }
    case base::variant_index<RowCountModifier, InFilterRowCount>(): {
      const auto& in = base::unchecked_get<InFilterRowCount>(rc);
      if (in.duplicate_state.Is<HasDuplicates>() &&
          plan_.params.estimated_row_count > 1) {
        if (!selective_filter_base_row_count_) {
          selective_filter_base_row_count_ = plan_.params.estimated_row_count;
        }
        // An IN is a union of equalities over the list values. The list size
        // is unknown at plan time, so scale the single-value estimate by an
        // assumed distinct-value count. As with equality, estimate against the
        // pre-selective-filter row count and keep the most selective result.
        double per_value = EqualityFilterRows(*selective_filter_base_row_count_,
                                              in.estimated_distinct);
        double new_count =
            std::min(static_cast<double>(*selective_filter_base_row_count_),
                     per_value * kAssumedInListSize);
        plan_.params.estimated_row_count =
            std::min(plan_.params.estimated_row_count,
                     std::max(1u, static_cast<uint32_t>(new_count)));
      }
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
      base::variant_index<i::Cost, i::PostOperationLinearPerRowCost>()) {
    const auto& c = base::unchecked_get<i::PostOperationLinearPerRowCost>(cost);
    plan_.params.estimated_cost += c.cost * plan_.params.estimated_cost;
  }
  return builder_.AddRawOpcode(option);
}

i::RwHandle<i::StoragePtr> BytecodeLowering::StorageRegisterFor(
    uint32_t col,
    StorageType type) {
  auto [reg, inserted] =
      cache_.GetOrAllocate<i::StoragePtr>(kStorageReg, columns_[col].get());
  if (inserted) {
    plan_.register_inits.emplace_back(
        RegisterInit{reg.index, type.Upcast<RegisterInit::Type>(),
                     static_cast<uint16_t>(col)});
  }
  return reg;
}

i::ReadHandle<i::NullBitvector> BytecodeLowering::NullBitvectorRegisterFor(
    uint32_t col) {
  if (GetColumn(col).null_storage.nullability().Is<NonNull>()) {
    return {};
  }
  auto [reg, inserted] =
      cache_.GetOrAllocate<i::NullBitvector>(kNullBvReg, columns_[col].get());
  if (inserted) {
    plan_.register_inits.emplace_back(RegisterInit{
        reg.index, RegisterInit::NullBitvector{}, static_cast<uint16_t>(col)});
  }
  return reg;
}

i::ReadHandle<i::NullBitvector> BytecodeLowering::EnsurePrefixPopcountFor(
    uint32_t col) {
  auto nbv_reg = NullBitvectorRegisterFor(col);
  if (!GetColumn(col).null_storage.nullability().IsAnyOf<SparseNullTypes>()) {
    return nbv_reg;
  }
  auto [it, inserted] = prefix_popcount_emitted_.Insert(col, true);
  if (inserted) {
    using B = i::PrefixPopcount;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::null_bv_register>() =
        i::RwHandle<i::NullBitvector>(nbv_reg.index);
  }
  return nbv_reg;
}

i::ReadHandle<const BitVector*> BytecodeLowering::SmallValueEqBvRegisterFor(
    uint32_t col) {
  auto [reg, inserted] = cache_.GetOrAllocate<const BitVector*>(
      kSmallValueEqBvReg, columns_[col].get());
  if (inserted) {
    plan_.register_inits.emplace_back(
        RegisterInit{reg.index, RegisterInit::SmallValueEqBitvector{},
                     static_cast<uint16_t>(col)});
  }
  return reg;
}

i::ReadHandle<Span<const uint32_t>>
BytecodeLowering::SmallValueEqPopcountRegisterFor(uint32_t col) {
  auto [reg, inserted] = cache_.GetOrAllocate<Span<const uint32_t>>(
      kSmallValueEqPopcountReg, columns_[col].get());
  if (inserted) {
    plan_.register_inits.emplace_back(
        RegisterInit{reg.index, RegisterInit::SmallValueEqPopcount{},
                     static_cast<uint16_t>(col)});
  }
  return reg;
}

i::RwHandle<Span<uint32_t>> BytecodeLowering::IndexRegisterFor(uint32_t pos) {
  auto [reg, inserted] =
      cache_.GetOrAllocate<Span<uint32_t>>(kIndexReg, &indexes_[pos]);
  if (inserted) {
    plan_.register_inits.emplace_back(RegisterInit{
        reg.index, RegisterInit::IndexVector{}, static_cast<uint16_t>(pos)});
  }
  return reg;
}

i::RwHandle<Span<uint32_t>> BytecodeLowering::GetOrCreateScratchSpanRegister(
    uint32_t size) {
  auto scratch = builder_.GetOrCreateScratchRegisters(size);
  {
    using B = i::AllocateIndices;
    auto& bc = AddOpcode<B>(UnchangedRowCount{});
    bc.arg<B::size>() = size;
    bc.arg<B::dest_slab_register>() = scratch.slab;
    bc.arg<B::dest_span_register>() = scratch.span;
  }
  builder_.MarkScratchInUse(scratch);
  scratch_ = scratch;
  return scratch.span;
}

void BytecodeLowering::MaybeReleaseScratchSpanRegister() {
  if (scratch_.has_value()) {
    builder_.ReleaseScratch(*scratch_);
    scratch_ = std::nullopt;
  }
}

uint16_t BytecodeLowering::CalculateRowLayoutStride(
    const std::vector<RowLayoutParams>& row_layout_params) {
  PERFETTO_CHECK(!row_layout_params.empty());
  uint16_t calculated_total_row_stride = 0;
  for (const auto& param : row_layout_params) {
    const Column& col = GetColumn(param.column);
    bool is_non_null = col.null_storage.nullability().Is<NonNull>();
    calculated_total_row_stride +=
        (is_non_null ? 0u : 1u) + GetDataSize(col.storage.type());
  }
  return calculated_total_row_stride;
}

i::RwHandle<Slab<uint8_t>> BytecodeLowering::CopyToRowLayout(
    uint16_t row_stride,
    i::RwHandle<Span<uint32_t>> indices,
    i::ReadHandle<i::StringIdToRankMap> rank_map,
    const std::vector<RowLayoutParams>& row_layout_params) {
  uint32_t buffer_size = plan_.params.max_row_count * row_stride;
  i::RwHandle<Slab<uint8_t>> new_buffer_reg =
      builder_.AllocateRegister<Slab<uint8_t>>();
  {
    using B = i::AllocateRowLayoutBuffer;
    auto& op = AddOpcode<B>(UnchangedRowCount{});
    op.arg<B::buffer_size>() = buffer_size;
    op.arg<B::dest_buffer_register>() = new_buffer_reg;
  }
  uint16_t current_offset = 0;
  for (const auto& param : row_layout_params) {
    const Column& col = GetColumn(param.column);
    const auto& nullability = col.null_storage.nullability();
    auto null_bv_reg = EnsurePrefixPopcountFor(param.column);
    {
      using B = i::CopyToRowLayoutBase;
      auto index = i::Index<i::CopyToRowLayout>(
          col.storage.type(),
          NullabilityToSparseNullCollapsedNullability(nullability));
      auto& op = AddOpcode<B>(index, UnchangedRowCount{});
      op.arg<B::storage_register>() =
          StorageRegisterFor(param.column, col.storage.type());
      op.arg<B::null_bv_register>() = null_bv_reg;
      op.arg<B::source_indices_register>() = indices;
      op.arg<B::dest_buffer_register>() = new_buffer_reg;
      op.arg<B::rank_map_register>() = rank_map;
      op.arg<B::row_layout_offset>() = current_offset;
      op.arg<B::row_layout_stride>() = row_stride;
      op.arg<B::invert_copied_bits>() = param.invert_copied_bits;
    }
    current_offset +=
        (nullability.Is<NonNull>() ? 0u : 1u) + GetDataSize(col.storage.type());
  }
  PERFETTO_CHECK(current_offset == row_stride);
  return new_buffer_reg;
}

template <typename T>
T& BytecodeLowering::AddOpcode(RowCountModifier rc) {
  return AddOpcode<T>(i::Index<T>(), rc, T::kCost);
}

}  // namespace perfetto::trace_processor::core::dataframe
