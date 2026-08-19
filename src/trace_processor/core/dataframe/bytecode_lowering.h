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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_BYTECODE_LOWERING_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_BYTECODE_LOWERING_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/core/dataframe/dataframe_register_cache.h"
#include "src/trace_processor/core/dataframe/query_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/interpreter/bytecode_builder.h"
#include "src/trace_processor/core/interpreter/bytecode_core.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::dataframe {

// Turns access-path decisions made by QueryPlanBuilder into bytecode.
//
// This class owns everything mechanical about emission: register allocation
// and caching, scratch lifetimes, materializing a range into an index list,
// pruning nulls, translating sparse-null indices, row layout buffers, and the
// running row-count and cost estimates.
//
// It makes no access-path decisions of its own. Each Emit* method corresponds
// to one strategy the planner has already chosen, and expands it into however
// many bytecodes that strategy needs.
class BytecodeLowering {
 public:
  BytecodeLowering(uint32_t row_count,
                   const std::vector<std::shared_ptr<Column>>& columns,
                   const std::vector<Index>& indexes);

  // === State the planner needs to make decisions ===

  // Whether the set of matching rows is still a contiguous range, as opposed
  // to a materialized list of indices. Some strategies are only available
  // while this holds.
  bool IsIndicesRange() const {
    return std::holds_alternative<interpreter::RwHandle<Range>>(indices_reg_);
  }

  uint32_t max_row_count() const { return plan_.params.max_row_count; }
  uint32_t estimated_row_count() const {
    return plan_.params.estimated_row_count;
  }

  // === Filter values ===

  // Reserves the next filter value slot and records it on `spec`. Emits
  // nothing; used by strategies which need SQLite to pass a value they do not
  // themselves read.
  uint32_t ReserveFilterValueSlot(FilterSpec& spec);

  // Emits the cast of a scalar filter value, recording the slot on `spec`.
  interpreter::ReadHandle<interpreter::CastFilterValueResult>
  EmitCastFilterValue(FilterSpec& spec, const StorageType& type, NonNullOp op);

  // Emits the cast of an IN list filter value, recording the slot on `spec`.
  interpreter::RwHandle<std::unique_ptr<interpreter::CastFilterValueListResult>>
  EmitCastFilterValueList(FilterSpec& spec, const StorageType& type);

  // === Filter strategies ===

  // Binary search over a sorted column, narrowing the range in place.
  void EmitSortedFilter(
      const FilterSpec& spec,
      const StorageType& type,
      const RangeOp& op,
      const interpreter::ReadHandle<interpreter::CastFilterValueResult>& value);

  // Equality on a SetId-sorted uint32 column.
  void EmitSetIdSortedEq(
      const FilterSpec& spec,
      const interpreter::ReadHandle<interpreter::CastFilterValueResult>& value);

  // Equality served by a column's SmallValueEq specialized storage.
  void EmitSmallValueEq(
      const FilterSpec& spec,
      const interpreter::ReadHandle<interpreter::CastFilterValueResult>& value);

  // Equality scan over a range, materializing the matches into an index list.
  void EmitLinearFilterEq(
      const FilterSpec& spec,
      const NonIdStorageType& type,
      const interpreter::ReadHandle<interpreter::CastFilterValueResult>& value);

  // Scan filter over an index list for a non-string column.
  void EmitNonStringFilter(
      const FilterSpec& spec,
      const NonStringType& type,
      const NonStringOp& op,
      const interpreter::ReadHandle<interpreter::CastFilterValueResult>& value);

  // Scan filter over an index list for a string column.
  void EmitStringFilter(
      const FilterSpec& spec,
      const StringOp& op,
      const interpreter::ReadHandle<interpreter::CastFilterValueResult>& value);

  // IS NULL / IS NOT NULL over an index list.
  void EmitNullFilter(const FilterSpec& spec, const NullOp& op);

  // IN over an index list, without an index to accelerate it.
  void EmitScanFilterIn(
      const FilterSpec& spec,
      interpreter::RwHandle<
          std::unique_ptr<interpreter::CastFilterValueListResult>> values);

  // Applies a whole group of Eq/In filters through `index_idx`, chaining them
  // so each reads the previous one's output. `spec_idxs` indexes into `specs`
  // and is ordered by the index's own column order.
  void EmitIndexedFilters(std::vector<FilterSpec>& specs,
                          uint32_t index_idx,
                          const std::vector<uint32_t>& spec_idxs);

  // Replaces the row set with the empty set. Used when a filter cannot match.
  void EmitGuaranteedEmpty();

  // === Post-filter stages ===

  void EmitDistinct(const std::vector<DistinctSpec>& specs);

  // Reverses the row set. Used when the data is already sorted, but opposite
  // to the requested direction.
  void EmitReverse();

  void EmitSort(const std::vector<SortSpec>& specs);

  void EmitMinMax(const SortSpec& spec);

  void EmitOutput(const LimitSpec& limit, uint64_t cols_used);

  // Finalizes and returns the plan.
  QueryPlanImpl Build() &&;

 private:
  // How a bytecode changes the row-count estimate. See AddRawOpcode.
  struct UnchangedRowCount {};
  struct NonEqualityFilterRowCount {};

  // An equality filter with the given duplicate state and estimated
  // distinct-value count (0 = unknown).
  struct EqualityFilterRowCount {
    DuplicateState duplicate_state;
    uint32_t estimated_distinct = 0;
  };

  // An IN filter over a value list. Unlike a scalar equality, an IN matches
  // multiple distinct values; since the list size is not known at plan time
  // (the RHS may be a subquery), we assume it selects a fixed number of
  // distinct values. `estimated_distinct` is the per-column distinct-value
  // count (0 = unknown).
  struct InFilterRowCount {
    DuplicateState duplicate_state;
    uint32_t estimated_distinct = 0;
  };

  struct OneRowCount {};
  struct ZeroRowCount {};

  // Produces `limit` rows starting at `offset`.
  struct LimitOffsetRowCount {
    uint32_t limit;
    uint32_t offset;
  };
  using RowCountModifier = std::variant<UnchangedRowCount,
                                        NonEqualityFilterRowCount,
                                        InFilterRowCount,
                                        EqualityFilterRowCount,
                                        OneRowCount,
                                        ZeroRowCount,
                                        LimitOffsetRowCount>;

  // Register holding the set of matching rows, either as a contiguous range
  // or as a materialized list of indices.
  using IndicesReg = std::variant<interpreter::RwHandle<Range>,
                                  interpreter::RwHandle<Span<uint32_t>>>;

  // Parameters for conversion to row layout.
  struct RowLayoutParams {
    // The column to be copied.
    uint32_t column;

    // Whether, instead of copying the string column, we should replace it
    // with a rank of the string.
    bool replace_string_with_rank = false;

    // Whether the bits when copied should be inverted.
    bool invert_copied_bits = false;
  };

  // Alias for scratch register type.
  using Scratch = interpreter::BytecodeBuilder::ScratchRegisters;

  // Whether the span being pruned holds the query's result rows, or only a
  // temporary copy of them.
  enum class NullPruneScope { kResultRows, kScratchOnly };

  // Given a list of indices, prunes any indices that point to null rows
  // in the given column. The indices are pruned in-place.
  void PruneNullIndices(uint32_t col,
                        interpreter::RwHandle<Span<uint32_t>> indices,
                        NullPruneScope scope);

  // Given a list of table indices pointing to *only* non-null rows,
  // if necessary, translates them to the storage indices for the given column.
  // If no translation is needed, the indices are returned as-is.
  // If translation *is* needed, the value of `in_place` determines
  // whether the translation is done in-place or whether the data is stored
  // in the scratch register.
  interpreter::RwHandle<Span<uint32_t>> TranslateNonNullIndices(
      uint32_t col,
      interpreter::RwHandle<Span<uint32_t>> indices_register,
      bool in_place);

  // Ensures indices are stored in a Slab, converting from Range if necessary.
  PERFETTO_NO_INLINE interpreter::RwHandle<Span<uint32_t>>
  EnsureIndicesAreInSlab();

  // Adds a new bytecode instruction of type T to the plan.
  template <typename T>
  T& AddOpcode(RowCountModifier rc);

  // Adds a new bytecode instruction of type T with the given option value.
  template <typename T>
  T& AddOpcode(uint32_t option, RowCountModifier rc) {
    return static_cast<T&>(AddRawOpcode(option, rc, T::kCost));
  }

  // Adds a new bytecode instruction of type T with the given option value.
  template <typename T>
  T& AddOpcode(uint32_t option, RowCountModifier rc, interpreter::Cost cost) {
    return static_cast<T&>(AddRawOpcode(option, rc, cost));
  }

  PERFETTO_NO_INLINE interpreter::Bytecode&
  AddRawOpcode(uint32_t option, RowCountModifier rc, interpreter::Cost cost);

  // Allocates a register for column data pointer and adds RegisterInit entry.
  interpreter::RwHandle<interpreter::StoragePtr> StorageRegisterFor(
      uint32_t col,
      StorageType storage_type);

  // Returns the index register for the given position.
  interpreter::RwHandle<Span<uint32_t>> IndexRegisterFor(uint32_t pos);

  // Returns the null bitvector register for the given column.
  // For NonNull columns, returns an empty handle.
  interpreter::ReadHandle<interpreter::NullBitvector> NullBitvectorRegisterFor(
      uint32_t col);

  // Returns the null bitvector register for the given column, ensuring a
  // PrefixPopcount bytecode has been emitted for SparseNull columns.
  // No-op for non-SparseNull columns or if already emitted.
  interpreter::ReadHandle<interpreter::NullBitvector> EnsurePrefixPopcountFor(
      uint32_t col);

  // Returns the SmallValueEq bitvector register for the given column.
  interpreter::ReadHandle<const BitVector*> SmallValueEqBvRegisterFor(
      uint32_t col);

  // Returns the SmallValueEq popcount register for the given column.
  interpreter::ReadHandle<Span<const uint32_t>> SmallValueEqPopcountRegisterFor(
      uint32_t col);

  interpreter::RwHandle<Span<uint32_t>> GetOrCreateScratchSpanRegister(
      uint32_t size);

  void MaybeReleaseScratchSpanRegister();

  uint16_t CalculateRowLayoutStride(
      const std::vector<RowLayoutParams>& row_layout_params);

  interpreter::RwHandle<Slab<uint8_t>> CopyToRowLayout(
      uint16_t row_stride,
      interpreter::RwHandle<Span<uint32_t>> indices,
      interpreter::ReadHandle<interpreter::StringIdToRankMap> rank_map,
      const std::vector<RowLayoutParams>& row_layout_params);

  const Column& GetColumn(uint32_t idx) { return *columns_[idx]; }

  // Reference to the columns being queried.
  const std::vector<std::shared_ptr<Column>>& columns_;

  // Reference to the indexes available.
  const std::vector<Index>& indexes_;

  // The query plan being built.
  QueryPlanImpl plan_;

  // Low-level bytecode builder for register allocation, bytecode storage,
  // and scratch management.
  interpreter::BytecodeBuilder builder_;

  // Register cache for caching column/index registers.
  DataframeRegisterCache cache_;

  // Current register holding the set of matching indices.
  IndicesReg indices_reg_;

  // Tracks which columns have had PrefixPopcount bytecode emitted.
  base::FlatHashMap<uint32_t, bool> prefix_popcount_emitted_;

  // Last scratch registers returned by GetOrCreateScratchSpanRegister.
  std::optional<Scratch> scratch_;

  // Row count before the first selective (equality/IN) filter was applied. Used
  // to avoid compounding the selectivity of multiple such filters: only the
  // most selective one determines the estimate.
  std::optional<uint32_t> selective_filter_base_row_count_;
};

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_BYTECODE_LOWERING_H_
