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
#include <vector>

#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/dataframe_windows.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {

// Reads a dataframe's rows without going through SQL.
//
// The batches point straight at the dataframe's own storage, so a query which
// reads a table and does nothing else to it copies nothing. Deciding whether a
// query is one of those belongs to whoever builds the plan: a relation which
// filters, joins, groups or computes has work for SQLite to do and goes to
// SqlScan instead.
//
// The exception is a column which does not store one value per row. Such a
// column is expanded a batch at a time into a fixed-size buffer owned by the
// execution, so a relation can be free for most of its columns and pay a
// bounded amount for the rest. Nothing is materialised ahead of being asked
// for, so a query which reads one batch and stops does one batch of work.
//
// The scan reads the columns directly rather than the dataframe, so it keeps
// working if the table is replaced, as long as whoever built the scan keeps the
// columns alive. The dataframe must be finalized before its columns are
// captured.
class DataframeScan final : public Source {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  DataframeScan(core::ArenaSpan<const dataframe::Column*> columns,
                uint32_t row_count,
                ReadDirection direction);

  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    RangeReader reader;
  };

  OperatorState* NewState(Context& context) const override;

  core::ArenaSpan<const dataframe::Column*> columns_;
  uint32_t row_count_;
  ReadDirection direction_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_SCAN_H_
