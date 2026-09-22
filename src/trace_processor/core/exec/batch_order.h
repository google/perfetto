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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_ORDER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_ORDER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// Orders the rows of each batch by the given columns, ascending, nulls first,
// with equal rows kept in their input order. The order holds within a batch
// only: nothing is said about rows of different batches.
//
// The columns must be flat Int64. The batch's values are not copied: the
// ordering columns are read into keys, and the batch is composed with a
// permutation of its rows; a batch already in order passes through as it
// is. Rows are ordered by a radix sort over the bytes of each
// column which differ between rows, least significant column first, so the
// cost grows with the spread of the values rather than the width of the key.
class BatchOrder : public Operator {
 public:
  // `columns` from most to least significant. `ordered_by_last` is how many of
  // the trailing columns every batch already arrives ordered by; only the
  // columns before them are sorted, which keeps that order among equal rows.
  explicit BatchOrder(std::vector<uint32_t> columns,
                      uint32_t ordered_by_last = 0);
  ~BatchOrder() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  std::vector<uint32_t> columns_;
  uint32_t ordered_by_last_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BATCH_ORDER_H_
