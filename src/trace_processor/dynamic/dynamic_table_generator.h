/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DYNAMIC_DYNAMIC_TABLE_GENERATOR_H_
#define SRC_TRACE_PROCESSOR_DYNAMIC_DYNAMIC_TABLE_GENERATOR_H_

#include "perfetto/base/status.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/sqlite/query_constraints.h"

namespace perfetto {
namespace trace_processor {

// Interface which can be subclassed to allow generation of tables dynamically
// at filter time.
// This class is used to implement table-valued functions and other similar
// tables.
class DynamicTableGenerator {
 public:
  virtual ~DynamicTableGenerator();

  // Returns the schema of the table that will be returned by ComputeTable.
  virtual Table::Schema CreateSchema() = 0;

  // Returns the name of the dynamic table.
  // This will be used to register the table with SQLite.
  virtual std::string TableName() = 0;

  // Returns the estimated number of rows the table would generate.
  virtual uint32_t EstimateRowCount() = 0;

  // Checks that the constraint set is valid.
  //
  // Returning base::OkStatus means that the required constraints are present
  // in |qc| for dynamically computing the table (e.g. any required
  // constraints on hidden columns for table-valued functions are present).
  virtual base::Status ValidateConstraints(const QueryConstraints& qc) = 0;

  // Dynamically computes the table given the constraints and order by
  // vectors.
  // The table is returned via |table_return|. There are no guarantees on
  // its value if the method returns a non-ok status.
  virtual base::Status ComputeTable(const std::vector<Constraint>& cs,
                                    const std::vector<Order>& ob,
                                    const BitVector& cols_used,
                                    std::unique_ptr<Table>& table_return) = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DYNAMIC_DYNAMIC_TABLE_GENERATOR_H_
