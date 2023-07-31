/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_EXPERIMENTAL_ANNOTATED_STACK_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_EXPERIMENTAL_ANNOTATED_STACK_H_

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// The "experimental_annotated_callstack" dynamic table.
//
// Given a leaf callsite id, returns the full callstack (including the leaf),
// with optional (currently Android-specific) annotations. A given callsite will
// always have the same annotation.
class ExperimentalAnnotatedStack : public StaticTableFunction {
 public:
  explicit ExperimentalAnnotatedStack(TraceProcessorContext* context)
      : context_(context) {}

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::Status ValidateConstraints(const QueryConstraints&) override;
  base::Status ComputeTable(const std::vector<Constraint>& cs,
                            const std::vector<Order>& ob,
                            const BitVector& cols_used,
                            std::unique_ptr<Table>& table_return) override;

 private:
  TraceProcessorContext* context_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_EXPERIMENTAL_ANNOTATED_STACK_H_
