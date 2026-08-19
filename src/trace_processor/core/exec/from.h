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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_FROM_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_FROM_H_

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/transient_column.h"

namespace perfetto::trace_processor::core::exec {

// Implements FROM by referencing the selected source rows in data chunks.
//
// The emitted batch is a member reused for every chunk: operators only ever
// change a column's row view, so refreshing a chunk means restoring the views
// rather than rebuilding the columns.
class From : public Source {
 public:
  From(std::vector<TransientColumn> columns, RowSelection rows, uint32_t count);

  void Reset() override { emitted_ = 0; }
  RowBatch* Next() override;

 private:
  // The columns as handed in, holding the row view each chunk starts from.
  std::vector<TransientColumn> columns_;
  RowBatch batch_;
  RowSelection rows_;
  uint32_t count_;
  uint32_t emitted_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_FROM_H_
