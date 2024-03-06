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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_EXPERIMENTAL_COUNTER_DUR_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_EXPERIMENTAL_COUNTER_DUR_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/db/column_storage.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/static_table_function.h"
#include "src/trace_processor/tables/counter_tables_py.h"

namespace perfetto::trace_processor {

class ExperimentalCounterDur : public StaticTableFunction {
 public:
  using CounterTable = tables::CounterTable;

  explicit ExperimentalCounterDur(const CounterTable& table);
  virtual ~ExperimentalCounterDur() override;

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::StatusOr<std::unique_ptr<Table>> ComputeTable(
      const std::vector<SqlValue>& arguments) override;

  // public + static for testing
  static ColumnStorage<int64_t> ComputeDurColumn(const CounterTable& table);
  static ColumnStorage<double> ComputeDeltaColumn(const CounterTable& table);

 private:
  const CounterTable* counter_table_ = nullptr;
  std::unique_ptr<Table> counter_dur_table_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_INTRINSICS_TABLE_FUNCTIONS_EXPERIMENTAL_COUNTER_DUR_H_
