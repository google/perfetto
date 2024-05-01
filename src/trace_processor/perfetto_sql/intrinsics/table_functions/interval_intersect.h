/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_INTERVAL_INTERSECT_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_INTERVAL_INTERSECT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"

namespace perfetto::trace_processor {

// An SQL table-function which computes the intersection of intervals from two
// tables.
//
// Given two sets of sorted non-overlapping intervals (with id, timestamp and
// duration) returns intervals that are intersections between those two sets,
// with ids to state what intervals are intersected.
//
// LEFT         . - - - - - - .
// RIGHT        - - . - - . - -
// Intersection . - . - - . - .
//
// Arguments are RepeatedBuilderResult protos containing a column of
// numerics values:
// 1) |in_left_ids|(uint32_t): Ids from the left table.
// 2) |in_left_tses|(uint64_t): Timestamps (starts) of intervals from
// the left table.
// 3) |in_left_durs|(uint64_t): Durations of intervals
// from the left table.
// 4) |in_right_ids|(uint32_t): Ids from the right table.
// 5) |in_right_tses|(uint64_t): Timestamps (starts) of intervals
// from the right table.
// 6) |in_right_durs|(uint64_t): Durations of intervals from the right table.
//
// NOTES:
// - The first 3 arguments have to have the same number of values.
// - Timestamps in left and right columns have to be sorted.
//
// Returns:
// 1) |ts|: Start of the intersection.
// 2) |dur|: Duration of the intersection.
// 3) |left_id|: Id of the slice that was intersected in the first table.
// 4) |right_id|: Id of the slice that was intersected in the second table.
class IntervalIntersect : public StaticTableFunction {
 public:
  explicit IntervalIntersect(StringPool*);
  virtual ~IntervalIntersect() override;

  // StaticTableFunction implementation.
  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::StatusOr<std::unique_ptr<Table>> ComputeTable(
      const std::vector<SqlValue>& arguments) override;

 private:
  StringPool* pool_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_INTERVAL_INTERSECT_H_
