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

// SPIKE v2: the general pieces for running dataframe queries in the executor.

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_SPIKE_ROWS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_SPIKE_ROWS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/ext/base/small_vector.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::exec {

// Rows of a dataframe, in an order: [begin, end) of `permutation`, or of the
// dataframe's own order when there is none.
struct Rows {
  const uint32_t* permutation = nullptr;
  uint32_t begin = 0;
  uint32_t end = 0;
};

// A dataframe column as a view of all its rows. A sparse column's prefix
// popcount is appended to `prefixes`, which must outlive the view.
ColumnView ViewOfColumn(const dataframe::Column&,
                        std::vector<Slab<uint32_t>>* prefixes);

// Narrows `rows`, ordered by `column` ascending with nulls first, to those
// where `op` holds against `value`.
Rows Narrow(const ColumnView& column,
            Op op,
            const filter::CastFilterValueResult& value,
            Rows rows);

// Narrow() for `op` on `column`, over an index's permutation or the table's
// own order, chosen once for the plan rather than per call.
using Narrower = Rows (*)(const ColumnView&,
                          Op,
                          const filter::CastFilterValueResult&,
                          Rows);
Narrower NarrowerFor(const ColumnView& column, Op op, bool permuted);

// Reads rows of a dataframe as views of its own storage: no value is copied,
// whatever the column's null layout. Opening it finds the rows a run reads,
// narrowing the table's own order, or an index's, by the predicates given.
class RowsScan final : public Source {
 public:
  // A predicate narrowing the rows, with the value the caller casts into
  // `value` before each run.
  struct Narrowing {
    // What a run reads first, ahead of the view.
    Narrower narrow = nullptr;
    const filter::CastFilterValueResult* value = nullptr;
    Op op;
    ColumnView column;
  };

  RowsScan(const dataframe::Dataframe&,
           std::vector<uint32_t> columns,
           const uint32_t* permutation,
           std::vector<Narrowing> narrowings,
           bool empty);
  ~RowsScan() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  void Rewind(OperatorState&) const override;
  RowBatch* Open(RowBatch& scratch, OperatorState&) const override;
  // Returns a batch of its own, whose columns it only points anew each call.
  RowBatch* Next(RowBatch& scratch, OperatorState&) const override;
  bool GetData(RowBatch& out, OperatorState&) const override;

 private:
  struct State : OperatorState {
    // Ahead of the batch, so a run's position shares its first line.
    uint32_t next = 0;
    uint32_t end = 0;
    bool started = false;
    RowBatch batch;
  };
  Rows Narrowed() const;
  RowBatch* Produce(State&, uint32_t next, uint32_t end) const;
  // Redoes what changed about `out` since this last produced it.
  void Settle(RowBatch& out) const;

  // What a run reads, first and inline, so it spans as few lines as it can.
  const uint32_t* permutation_;
  uint32_t row_count_;
  uint32_t column_count_;
  base::SmallVector<Narrowing, 2> narrowings_;
  std::vector<ColumnView> views_;
  std::vector<Slab<uint32_t>> prefixes_;
};

// Keeps the rows where `op` holds between `column` and `*value`, which the
// caller sets before each run. Rows with no value never match a comparison.
class ColumnFilter final : public Transform {
 public:
  ColumnFilter(uint32_t column,
               StorageType type,
               Op op,
               const filter::CastFilterValueResult* value,
               const StringPool* pool);

  // Whether a filter on a column of `type` with `op` can run here.
  static bool Supports(StorageType type, Op op);

  std::unique_ptr<OperatorState> MakeState() const override;
  bool Process(RowBatch& batch, OperatorState& state) const override;
  ProcessFn process_fn() const override { return &Run; }
  bool Rewinds() const override { return false; }

 private:
  struct State : OperatorState {
    // The batch's rows being kept, and the storage index each reads.
    FlexVector<uint32_t> rows;
    FlexVector<uint32_t> indices;
  };
  // Keeps, of the rows at [out, out + (end - begin)), those whose value at
  // the storage index at the same place in [begin, end) compares true;
  // returns the end. The one loop per type and comparison, shared with the
  // interpreter's (filter_kernels.h): all else a filter does is in Run(),
  // once for every type.
  using Kernel = uint32_t* (*)(const void* data,
                               const uint32_t* begin,
                               const uint32_t* end,
                               uint32_t* out,
                               const filter::CastFilterValueResult& value,
                               const StringPool* pool);
  // Writes the rows of `count` whose value, the row's in `data`, equals the
  // filter's; returns the end. The interpreter's LinearFilterEq: equality on
  // a run of present values, the common case, needs no index list.
  using LinearKernel = uint32_t* (*)(const void* data,
                                     uint32_t count,
                                     uint32_t* out,
                                     const filter::CastFilterValueResult& value,
                                     const StringPool* pool);
  // Process(), as the plain function a pipeline calls.
  static bool Run(const Transform& self, RowBatch& batch, OperatorState& state);
  // The rows of `count` holding a value comparing true; returns the end.
  uint32_t* Keep(const ColumnView& view, uint32_t count, State& s) const;

  uint32_t column_;
  // Null for a null check, which keeps nulls or not.
  Kernel kernel_ = nullptr;
  // Set for equality on a stored column.
  LinearKernel linear_ = nullptr;
  bool keep_null_ = false;
  // Whether null string ids have to be dropped before comparing: only !=
  // would keep them, as they equal no interned string.
  bool drop_null_ids_ = false;
  // Bytes a value of the column takes.
  uint8_t width_ = 0;
  const filter::CastFilterValueResult* value_;
  const StringPool* pool_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SPIKE_ROWS_H_
