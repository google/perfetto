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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_SCAN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_SCAN_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// Reads a dataframe's rows without going through SQL.
//
// The batches point straight at the dataframe's own storage, so a query which
// reads a table and does nothing else to it copies nothing.
//
// The exception is a column which does not store one value per row. Such a
// column is expanded a batch at a time into a fixed-size buffer owned by the
// execution, so a relation can be free for most of its columns and pay a
// bounded amount for the rest. Nothing is materialised ahead of being asked
// for, so a query which reads one batch and stops does one batch of work.
//
// A scan can also read only some of the rows, listed in increasing order,
// such as those a filter kept: the batches then select those rows of the
// storage, still without copying it.
//
// The scan holds shared ownership of the columns rather than a pointer to the
// dataframe, so it keeps working if the table is replaced. The dataframe must
// have been finalized before its columns were captured.
class DataframeScan : public Source {
 public:
  // Reads rows [0, row_count), or, if `rows` is not null, the `row_count`
  // rows it lists.
  DataframeScan(std::vector<std::shared_ptr<const dataframe::Column>> columns,
                uint32_t row_count,
                std::shared_ptr<const FlexVector<uint32_t>> rows);
  ~DataframeScan() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

  // Fills one batch of a column which does not store one value per row.
  // Defined in the .cc: an implementation detail with no callers outside it.
  class Expander;

 protected:
  // What a subclass keeps across runs to find their rows; null by default.
  virtual std::unique_ptr<OperatorState> MakeRowsState() const;

  // The rows a run reads, in increasing order, or null for every row. Found
  // as each run starts, so a subclass's rows can depend on what changes
  // between runs. `rows_state` is what MakeRowsState made.
  virtual std::shared_ptr<const FlexVector<uint32_t>> FindRows(
      OperatorState* rows_state) const;

 private:
  struct State : OperatorState {
    ~State() override;
    std::vector<ColumnView> columns;
    // One per column, null unless the column has to be expanded.
    std::vector<std::unique_ptr<Expander>> expanders;
    // The rows this run reads, or null for all `row_count` rows.
    std::shared_ptr<const FlexVector<uint32_t>> rows;
    std::unique_ptr<OperatorState> rows_state;
    uint32_t row_count = 0;
    uint32_t emitted = 0;
  };

  // Starts a run over the rows FindRows gives.
  void StartRun(State&) const;

  std::vector<std::shared_ptr<const dataframe::Column>> columns_;
  uint32_t row_count_;
  std::shared_ptr<const FlexVector<uint32_t>> rows_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_SCAN_H_
