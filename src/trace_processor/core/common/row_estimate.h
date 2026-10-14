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

#ifndef SRC_TRACE_PROCESSOR_CORE_COMMON_ROW_ESTIMATE_H_
#define SRC_TRACE_PROCESSOR_CORE_COMMON_ROW_ESTIMATE_H_

#include <cstdint>
#include <optional>

#include "src/trace_processor/core/common/duplicate_types.h"

// How many rows a query is expected to produce as it filters them: the rules
// a planner uses to estimate, shared by everything which plans queries.
namespace perfetto::trace_processor::core {

// The number of rows flowing into or out of an operation.
struct RowEstimate {
  // The most rows there can possibly be.
  uint32_t max = 0;

  // The number of rows the planner expects.
  uint32_t estimated = 0;

  bool operator==(const RowEstimate& o) const {
    return max == o.max && estimated == o.estimated;
  }
};

// Tracks how many rows are left as operations are applied.
class RowModel {
 public:
  explicit RowModel(uint32_t row_count) : rows_{row_count, row_count} {}

  RowEstimate rows() const { return rows_; }

  // Filters whose selectivity we cannot reason about keep half the rows.
  void ApplyNonEqualityFilter();
  void ApplyEqualityFilter(DuplicateState duplicate_state,
                           uint32_t estimated_distinct);
  void ApplyInFilter(DuplicateState duplicate_state,
                     uint32_t estimated_distinct);
  void ApplyOneRow();
  void ApplyZeroRows();
  void ApplyLimitOffset(uint32_t limit, uint32_t offset);

 private:
  void SetSelectiveBase();

  RowEstimate rows_;

  // Row count before the first selective (equality/IN) filter was applied.
  // Used to avoid compounding the selectivity of multiple such filters: only
  // the most selective one determines the estimate.
  std::optional<uint32_t> selective_base_;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_ROW_ESTIMATE_H_
