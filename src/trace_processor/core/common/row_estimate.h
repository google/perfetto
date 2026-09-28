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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "perfetto/base/logging.h"
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

// Assumed number of distinct values matched by an IN filter when the list size
// is not known at plan time. Scales the single-value equality estimate. Matches
// SQLite's own tuning constant for "x IN (SELECT ...)" (see whereLoopAddBtree).
inline constexpr double kAssumedInListSize = 25;

// Rows surviving a scalar equality filter on a HasDuplicates column with
// `estimated_distinct` distinct values (0 = unknown). With a known count, a
// uniform column keeps ~1/estimated_distinct of the rows; otherwise fall back
// to the data-blind heuristic.
inline double EqualityFilterRows(uint32_t row_count,
                                 uint32_t estimated_distinct) {
  if (estimated_distinct > 0) {
    return static_cast<double>(row_count) / estimated_distinct;
  }
  return row_count / (2 * log2(row_count));
}

// Tracks how many rows are left as operations are applied.
class RowModel {
 public:
  explicit RowModel(uint32_t row_count) : rows_{row_count, row_count} {}

  RowEstimate rows() const { return rows_; }

  // Filters whose selectivity we cannot reason about keep half the rows.
  void ApplyNonEqualityFilter() {
    if (rows_.estimated > 1) {
      rows_.estimated = rows_.estimated / 2;
    }
  }

  void ApplyEqualityFilter(DuplicateState duplicate_state,
                           uint32_t estimated_distinct) {
    if (duplicate_state.Is<HasDuplicates>()) {
      if (rows_.estimated > 1) {
        // Estimate against the pre-selective-filter row count and keep the
        // most selective result: correlated filters on one scan shouldn't
        // compound and collapse the estimate toward 1.
        SetSelectiveBase();
        rows_.estimated =
            std::min(rows_.estimated,
                     std::max(1u, static_cast<uint32_t>(EqualityFilterRows(
                                      *selective_base_, estimated_distinct))));
      }
      return;
    }
    PERFETTO_CHECK(duplicate_state.Is<NoDuplicates>());
    rows_.estimated = std::min(1u, rows_.estimated);
    rows_.max = std::min(1u, rows_.max);
  }

  void ApplyInFilter(DuplicateState duplicate_state,
                     uint32_t estimated_distinct) {
    if (!duplicate_state.Is<HasDuplicates>() || rows_.estimated <= 1) {
      return;
    }
    // An IN is a union of equalities over the list values. The list size is
    // unknown at plan time, so scale the single-value estimate by an assumed
    // distinct-value count. As with equality, estimate against the
    // pre-selective-filter row count and keep the most selective result.
    SetSelectiveBase();
    double per_value = EqualityFilterRows(*selective_base_, estimated_distinct);
    double new_count = std::min(static_cast<double>(*selective_base_),
                                per_value * kAssumedInListSize);
    rows_.estimated = std::min(rows_.estimated,
                               std::max(1u, static_cast<uint32_t>(new_count)));
  }

  void ApplyOneRow() {
    rows_.estimated = std::min(1u, rows_.estimated);
    rows_.max = std::min(1u, rows_.max);
  }

  void ApplyZeroRows() {
    rows_.estimated = 0;
    rows_.max = 0;
  }

  void ApplyLimitOffset(uint32_t limit, uint32_t offset) {
    // Offset will cut out `offset` rows from the start of indices.
    rows_.max -= std::min(rows_.max, offset);

    // Limit will only preserve at most `limit` rows.
    rows_.max = std::min(limit, rows_.max);

    // The max row count is also the best possible estimate we can make for
    // the row count.
    rows_.estimated = rows_.max;
  }

 private:
  void SetSelectiveBase() {
    if (!selective_base_) {
      selective_base_ = rows_.estimated;
    }
  }

  RowEstimate rows_;

  // Row count before the first selective (equality/IN) filter was applied.
  // Used to avoid compounding the selectivity of multiple such filters: only
  // the most selective one determines the estimate.
  std::optional<uint32_t> selective_base_;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_ROW_ESTIMATE_H_
