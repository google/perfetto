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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_DISTINCT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_DISTINCT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

class Context;

// Keeps the first row of each set of rows equal in `columns` and drops the
// rest. Rows are compared as in GroupTable, and the rows kept stay in order.
//
// Rows are dropped by narrowing the selection, so nothing is copied. Only
// the keys of the rows kept are stored, so memory grows with the number of
// distinct rows, not the number of input rows. There can be at most 64
// columns.
class Distinct final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit Distinct(std::vector<uint32_t> columns);
  ~Distinct() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool Process(RowBatch&, OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  friend class PipelineFormatter;

  struct State : OperatorState {
    State() : OperatorState(ResetEachRun{}) {}
    ~State() override;
    void Reset() override;

    // Every key seen so far, numbered in the order first seen.
    GroupTable keys;

    // Scratch for the current batch: the key number of each row, and the
    // positions of the rows kept.
    FlexVector<uint32_t> row_keys;
    FlexVector<uint32_t> kept;

    base::Status status = base::OkStatus();
  };

  std::vector<uint32_t> columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_DISTINCT_H_
