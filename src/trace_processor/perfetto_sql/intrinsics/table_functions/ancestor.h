/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_ANCESTOR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_ANCESTOR_H_

#include <optional>

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// Implements the following dynamic tables:
// * ancestor_slice
// * experimental_ancestor_stack_profile_callsite
// * ancestor_slice_by_stack
//
// See docs/analysis/trace-processor for usage.
class Ancestor : public StaticTableFunction {
 public:
  enum class Type { kSlice = 1, kStackProfileCallsite = 2, kSliceByStack = 3 };

  Ancestor(Type type, const TraceStorage* storage);

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::Status ValidateConstraints(const QueryConstraints&) override;
  base::Status ComputeTable(const std::vector<Constraint>& cs,
                            const std::vector<Order>& ob,
                            const BitVector& cols_used,
                            std::unique_ptr<Table>& table_return) override;

  // Returns a vector of rows numbers which are ancestors of |slice_id|.
  // Returns std::nullopt if an invalid |slice_id| is given. This is used by
  // ConnectedFlow to traverse flow indirectly connected flow events.
  static std::optional<std::vector<tables::SliceTable::RowNumber>>
  GetAncestorSlices(const tables::SliceTable& slices, SliceId slice_id);

 private:
  Type type_;
  const TraceStorage* storage_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_ANCESTOR_H_
