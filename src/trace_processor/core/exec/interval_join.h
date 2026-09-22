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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_JOIN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_JOIN_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

// How an operand interval [a, b) has to relate to an input interval [s, e) for
// the two to be joined. An interval of zero width is a point: it is covered by
// an interval containing it, and only covers or lies within an equal point.
enum class IntervalRelationship : uint8_t {
  // The two share some time.
  kOverlappingBounds,
  // The operand contains s.
  kCoveringBegin,
  // The operand contains the last instant before e, so that covering both
  // ends is covering the bounds.
  kCoveringEnd,
  // The operand contains all of [s, e).
  kCoveringBounds,
  // The operand lies inside [s, e).
  kWithinBounds,
};

struct IntervalJoinSpec {
  // The columns of one side. All of them must be flat Int64; a row holding a
  // null in any of them joins with nothing.
  struct Side {
    uint32_t ts_column = 0;
    // A side without a duration is a side of points.
    std::optional<uint32_t> dur_column;
    // Two rows can only join if these are equal, pairwise across the sides.
    std::vector<uint32_t> key_columns;
  };
  Side input;
  Side operand;
  IntervalRelationship relationship = IntervalRelationship::kOverlappingBounds;
  // How many columns the operand has, which an empty operand cannot say.
  uint32_t operand_column_count = 0;
  // Keeps an input row which joins with nothing, with nulls for the operand.
  bool keep_unmatched = false;
};

// Joins each input row with every operand row its interval relates to.
//
// The input streams through and needs no order. The operand is read in full
// the first time it is needed and held, per key, as an IntervalIntersector
// over its intervals sorted by start, so it should be the smaller side; the
// intersector picks how to search them from whether they overlap. A negative
// duration is an interval which never ends.
//
// Output rows are the input columns followed by the operand columns. They keep
// the order of the input; the matches of one input row are ordered by operand
// start, then by operand row.
class IntervalJoin : public Operator {
 public:
  // `operand` must outlive this.
  IntervalJoin(const Source& operand, IntervalJoinSpec);
  ~IntervalJoin() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch&, RowBatch&, OperatorState&) const override;
  void Rewind(OperatorState&) const override;
  base::Status status(const OperatorState&) const override;

 private:
  const Source& operand_;
  IntervalJoinSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_JOIN_H_
