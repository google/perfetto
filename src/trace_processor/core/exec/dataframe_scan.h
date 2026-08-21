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

#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Reads a dataframe's rows without going through SQL.
//
// The batches point straight at the dataframe's own storage, so a query which
// reads a table and does nothing to it copies nothing at all. Whether a query
// is that is a question for whatever builds the plan: a relation which
// filters, joins, groups or computes has work for SQLite to do and goes to
// SqlScan instead.
//
// The exception is a column which does not store one value per row. Those are
// expanded once into storage the execution owns, so a relation can be free
// for most of its columns and pay for a few.
class DataframeScan : public Source {
 public:
  DataframeScan(const dataframe::Dataframe*, std::vector<uint32_t> columns);
  ~DataframeScan() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  bool GetData(RowBatch& out, OperatorState& state) const override;
  void Rewind(OperatorState& state) const override;

 private:
  struct State : OperatorState {
    ~State() override;
    std::vector<ColumnView> columns;
    // One per column: what keeps an expanded column alive, null where the
    // view points at the dataframe itself.
    std::vector<std::shared_ptr<const void>> owners;
    uint32_t emitted = 0;
  };

  const dataframe::Dataframe* dataframe_;
  std::vector<uint32_t> columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_SCAN_H_
