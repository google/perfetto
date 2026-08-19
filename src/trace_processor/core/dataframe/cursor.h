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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_CURSOR_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_CURSOR_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/query_plan.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/from.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"

namespace perfetto::trace_processor::core::dataframe {

// Namespace alias for the interpreter types.

// Callback for receiving cell values
struct CellCallback {
  void OnCell(int64_t);
  void OnCell(double);
  void OnCell(NullTermStringView);
  void OnCell(std::nullptr_t);
  void OnCell(uint32_t);
  void OnCell(int32_t);
};

// Cursor provides a mechanism to iterate through dataframe query results
// and access column values.
class Cursor {
 public:
  Cursor() = default;

  // Initializes the cursor from a query plan and dataframe columns.
  void Initialize(const QueryPlanImpl& plan,
                  uint32_t column_count,
                  const Column* const* column_ptrs,
                  const Index* indexes,
                  const StringPool* pool) {
    interpreter_.Initialize(plan.bytecode, plan.params.register_count, pool);
    params_ = plan.params;
    col_to_output_offset_ = plan.col_to_output_offset;
    pool_ = pool;

    column_storage_data_ptrs_.clear();
    column_storage_data_ptrs_.reserve(column_count);
    for (uint32_t i = 0; i < column_count; ++i) {
      column_storage_data_ptrs_.push_back(column_ptrs[i]->storage.data());
    }

    // A cursor is re-prepared for every execution when the caller re-enters
    // it, as a join does for each outer row, so only build when the plan
    // actually changed.
    if (!operator_state_ || !(plan.operator_plan == operator_plan_)) {
      operator_plan_ = plan.operator_plan;
      BuildOperatorTree(column_ptrs);
    }

    // Process register initialization specs from the plan.
    // This sets up registers with pointers extracted from columns/indexes.
    for (const auto& init : plan.register_inits) {
      auto val =
          QueryPlanImpl::GetRegisterInitValue(init, column_ptrs, indexes);
      interpreter_.SetRegisterValue(interpreter::HandleBase{init.dest_register},
                                    std::move(val));
    }
  }

  // Executes the query and prepares the cursor for iteration.
  // This initializes the cursor's position to the first row of results.
  //
  // Parameters:
  //   fvf: A subclass of `ValueFetcher` that defines the logic for fetching
  //        filter values for each filter spec.
  void Execute(ValueFetcher&);

  // Runs the query on the operator executor. Only valid when the lowering
  // accepted the plan, which is what makes it infallible.
  void ExecuteOnOperators(ValueFetcher&);

  // Returns the index of the row in the table this cursor is pointing to.
  PERFETTO_ALWAYS_INLINE uint32_t RowIndex() const {
    return operator_streaming_ ? sink_->row() : *pos_;
  }

  // Advances the cursor to the next row of results.
  PERFETTO_ALWAYS_INLINE void Next() {
    if (operator_streaming_) {
      sink_->Next();
      return;
    }
    PERFETTO_DCHECK(pos_ < end_);
    pos_ += params_.output_per_row;
  }

  // Returns true if the cursor has reached the end of the result set.
  PERFETTO_ALWAYS_INLINE bool Eof() const {
    return operator_streaming_ ? sink_->eof() : pos_ == end_;
  }

  // Returns the value of the column at the current cursor position.
  // The visitor pattern allows type-safe access to heterogeneous column types.
  //
  // Parameters:
  //   col:    The index of the column to access.
  //   callback: A subclass of `CellCallback` that defines the logic for
  //             processing the value of the column at the current cursor
  //             position.
  template <typename CellCallbackImpl>
  PERFETTO_ALWAYS_INLINE void Cell(uint32_t col,
                                   CellCallbackImpl& cell_callback_impl) {
    static_assert(std::is_base_of_v<CellCallback, CellCallbackImpl>,
                  "CellCallbackImpl must be a subclass of CellCallback");
    PERFETTO_DCHECK(col < col_to_output_offset_.size());
    const Storage::DataPointer& p = column_storage_data_ptrs_[col];
    // Nothing the executor accepts yet can be null, so a row is every one of
    // its columns' value position.
    uint32_t idx =
        operator_streaming_ ? sink_->row() : pos_[col_to_output_offset_[col]];
    if (idx == std::numeric_limits<uint32_t>::max()) {
      cell_callback_impl.OnCell(nullptr);
      return;
    }
    switch (p.index()) {
      case StorageType::GetTypeIndex<Id>():
        cell_callback_impl.OnCell(idx);
        break;
      case StorageType::GetTypeIndex<Uint32>():
        cell_callback_impl.OnCell(Storage::CastDataPtr<Uint32>(p)[idx]);
        break;
      case StorageType::GetTypeIndex<Int32>():
        cell_callback_impl.OnCell(Storage::CastDataPtr<Int32>(p)[idx]);
        break;
      case StorageType::GetTypeIndex<Int64>():
        cell_callback_impl.OnCell(Storage::CastDataPtr<Int64>(p)[idx]);
        break;
      case StorageType::GetTypeIndex<Double>():
        cell_callback_impl.OnCell(Storage::CastDataPtr<Double>(p)[idx]);
        break;
      case StorageType::GetTypeIndex<String>():
        cell_callback_impl.OnCell(
            pool_->Get(Storage::CastDataPtr<String>(p)[idx]));
        break;
      default:
        PERFETTO_FATAL("Invalid storage spec");
    }
  }

 private:
  // Assembles the operator tree for `operator_plan_`, once per query plan.
  // Values are not known yet; each execution hands them in when it arms
  // the tree.
  void BuildOperatorTree(const Column* const* column_ptrs);

  // Leaves the cursor on an empty result set without touching the pipeline,
  // for a plan the planner already proved produces nothing.
  void SetEmptyOperatorResult() {
    operator_streaming_ = false;
    pos_ = nullptr;
    end_ = nullptr;
  }

  // Bytecode interpreter that executes the query.
  interpreter::Interpreter interpreter_;

  // Operator executor state, allocated only for a plan the executor can run.
  // SQLite constructs a cursor per re-entry of a query, so anything held inline
  // here would be paid for by every query, including those that do not run on
  // the operator executor at all.
  struct OperatorState {
    OperatorState(uint32_t rows)
        : scratch(size_t{ChunkRows(rows)} * 2), chunk_rows(ChunkRows(rows)) {}

    // A chunk is never longer than the table, so scratch sized past that is
    // memory touched for nothing.
    static uint32_t ChunkRows(uint32_t rows) {
      return rows < exec::kMaxBatchRows ? rows : exec::kMaxBatchRows;
    }
    Span<uint32_t> ScratchFor(size_t filter) {
      uint32_t* begin = scratch.data() + (filter % 2) * size_t{chunk_rows};
      return Span<uint32_t>(begin, begin + chunk_rows);
    }

    std::vector<uint32_t> scratch;
    uint32_t chunk_rows;
    std::unique_ptr<exec::From> source;
    std::unique_ptr<exec::PullPipeline> pipeline;
    // Reads the rows the pipeline produces, one at a time.
    std::unique_ptr<exec::RowCursor> sink;
  };
  OperatorPlan operator_plan_;
  std::unique_ptr<OperatorState> operator_state_;
  // operator_state_->sink, held directly because every row goes through it.
  exec::RowCursor* sink_ = nullptr;
  bool operator_streaming_ = false;

  // Parameters for query execution.
  QueryPlanImpl::ExecutionParams params_;
  // Maps column indices to their output offsets in the result set.
  base::SmallVector<uint32_t, 24> col_to_output_offset_;
  // Variant of pointers to the storage data.
  std::vector<Storage::DataPointer> column_storage_data_ptrs_;
  // String pool for string values.
  const StringPool* pool_;

  // Current position in the result set.
  const uint32_t* pos_;
  // End position in the result set.
  const uint32_t* end_;
};

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_CURSOR_H_
