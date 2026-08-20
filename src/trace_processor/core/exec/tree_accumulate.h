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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_

#include <cstdint>
#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// TREE ACCUMULATE UP over a stream of chunks.
//
// A breaker: a node's total is not known until the last of its descendants
// has gone past. The parent column holds each row's parent as a position in
// the stream, or a negative value for a root, and parents precede children.
class TreeAccumulateUp : public Breaker {
 public:
  TreeAccumulateUp(const Source& input,
                   uint32_t parent_column,
                   uint32_t value_column);
  ~TreeAccumulateUp() override;

  base::Status Consume(RowBatch&, OperatorState&) const override;
  base::Status Finish(OperatorState&) const override;

 protected:
  std::unique_ptr<BreakerState> MakeBreakerState() const override;
  bool Emit(RowBatch& out, BreakerState&) const override;
  void Rewind(BreakerState&) const override;

 private:
  struct State : BreakerState {
    ~State() override;

    RowStore rows;
    // One per row, in the store's order. Shared so a batch keeps it alive.
    std::shared_ptr<FlexVector<int64_t>> totals =
        std::make_shared<FlexVector<int64_t>>();
    uint32_t emitted = 0;
  };

  uint32_t parent_column_;
  uint32_t value_column_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_
