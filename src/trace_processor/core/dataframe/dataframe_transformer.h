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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_DATAFRAME_TRANSFORMER_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_DATAFRAME_TRANSFORMER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/query_plan.h"
#include "src/trace_processor/core/dataframe/register_cache.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/interpreter/bytecode_builder.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::dataframe {

// Persistent wrapper around stateless QueryPlanBuilder::Filter.
//
// Owns a mutable column set that evolves across chained operations. Handles
// all column/index/register plumbing so callers (TreeTransformer) only deal
// with column indices, names, and register handles.
//
// Uses a pointer-keyed RegisterCache for register deduplication. When Column
// objects change (e.g. after gather creates new Column instances), the new
// pointers naturally miss the cache and fresh registers are allocated.
class DataframeTransformer {
 public:
  DataframeTransformer(interpreter::BytecodeBuilder& builder,
                       const Dataframe& df);

  // Emit filter bytecodes against current column state.
  // Specs must already have value_index set by the caller.
  // On first call, filters with InitRange(row_count).
  // On subsequent calls (after GatherAllColumns), filters with
  // InitRangeFromSpan.
  base::StatusOr<interpreter::RwHandle<BitVector>> Filter(
      std::vector<FilterSpec>& specs);

  // Get storage register for column. If the column's storage register has
  // not been allocated yet, allocates one and emits a RegisterInit.
  interpreter::ReadHandle<interpreter::StoragePtr> StorageRegisterFor(
      uint32_t col_idx);

  // Get column storage type by index.
  StorageType GetStorageType(uint32_t col_idx) const;

  // Get column nullability by index.
  Nullability GetNullability(uint32_t col_idx) const;

  // Get null bitvector register for column. If the column's null bv register
  // has not been allocated yet, allocates one and emits a RegisterInit.
  interpreter::ReadHandle<const BitVector*> NullBitvectorRegisterFor(
      uint32_t col_idx);

  // Find column index by name.
  std::optional<uint32_t> FindColumn(const std::string& name) const;

  // Add a virtual column backed by a pre-allocated register.
  // Returns the new column index.
  uint32_t AddColumn(
      const std::string& name,
      StorageType type,
      Nullability nullability,
      interpreter::RwHandle<interpreter::StoragePtr> storage_reg);

  // Gather ALL columns at the given indices after a compaction (FilterTree).
  // Emits GatherColumn bytecodes that create new storage from surviving rows.
  void GatherAllColumns(interpreter::ReadHandle<Span<uint32_t>> indices);

  // Upper-bound row count (for scratch buffer allocation).
  uint32_t max_row_count() const { return max_row_count_; }

  // Register initialization specs accumulated across all operations.
  // Used by TreeTransformer::ToDataframe() to initialize the interpreter.
  const base::SmallVector<RegisterInit, 16>& register_inits() const {
    return register_inits_;
  }

 private:
  // Creates a Column with empty storage of the given type and nullability.
  static std::shared_ptr<Column> MakeColumn(StorageType type,
                                            Nullability nullability);

  interpreter::BytecodeBuilder& builder_;

  std::vector<std::shared_ptr<Column>> columns_;
  std::vector<std::string> column_names_;
  std::vector<Index> indexes_;

  // Pointer-keyed register cache for deduplication.
  RegisterCache cache_;

  bool gathered_ = false;
  interpreter::ReadHandle<Span<uint32_t>> row_count_span_;
  uint32_t max_row_count_;

  base::SmallVector<RegisterInit, 16> register_inits_;

  struct GatherState {
    interpreter::RwHandle<Slab<uint8_t>> slab_reg;
    interpreter::RwHandle<BitVector> null_bv_reg;
    interpreter::RwHandle<const BitVector*> null_bv_ptr_reg;
    interpreter::RwHandle<interpreter::StoragePtr> storage_reg;
  };
  std::vector<GatherState> gather_state_;
};

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_DATAFRAME_TRANSFORMER_H_
