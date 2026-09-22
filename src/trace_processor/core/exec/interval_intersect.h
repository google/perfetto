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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_INTERSECT_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_INTERSECT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// One operand of an intersection: where its rows come from and which of its
// columns carry the interval and the key.
struct IntervalIntersectOperand {
  // Read in full before any region is found.
  const Source* source = nullptr;
  // All must be flat Int64 columns of `source`'s batches.
  uint32_t ts_column = 0;
  uint32_t dur_column = 0;
  // Compared pairwise across the operands, in this order.
  std::vector<uint32_t> key_columns;
};

// Emits a row over each region every operand covers.
//
// An interval is [ts, ts + dur) and a region is the part two or more of them
// have in common, so the rows out are the regions rather than the rows in.
// Each carries the region's own bounds followed by the columns of the operand
// row it came from, one set per operand. `PER` columns confine the regions to
// rows which agree on them, where two rows holding no value there agree on it
// as they would under `GROUP BY`; a key no other operand has covers nothing.
//
// A duration below zero is refused rather than read as a span, and so is a
// timestamp below zero, which is what the intrinsic this replaces does. A row
// of no width is a point: it meets an interval holding the instant it sits at
// and another point at the same instant.
class IntervalIntersect : public Source {
 public:
  // Every operand's source must outlive this. Two are the fewest which can
  // have a region in common.
  explicit IntervalIntersect(std::vector<IntervalIntersectOperand>);
  ~IntervalIntersect() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  bool GetData(RowBatch&, OperatorState&) const override;
  void Rewind(OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  std::vector<IntervalIntersectOperand> operands_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_INTERSECT_H_
