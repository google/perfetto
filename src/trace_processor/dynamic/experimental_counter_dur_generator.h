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

#ifndef SRC_TRACE_PROCESSOR_DYNAMIC_EXPERIMENTAL_COUNTER_DUR_GENERATOR_H_
#define SRC_TRACE_PROCESSOR_DYNAMIC_EXPERIMENTAL_COUNTER_DUR_GENERATOR_H_

#include "src/trace_processor/dynamic/dynamic_table_generator.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class ExperimentalCounterDurGenerator : public DynamicTableGenerator {
 public:
  using CounterTable = tables::CounterTable;

  explicit ExperimentalCounterDurGenerator(const CounterTable& table);
  virtual ~ExperimentalCounterDurGenerator() override;

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::Status ValidateConstraints(const QueryConstraints&) override;
  base::Status ComputeTable(const std::vector<Constraint>& cs,
                            const std::vector<Order>& ob,
                            const BitVector& cols_used,
                            std::unique_ptr<Table>& table_return) override;

  // public + static for testing
  static ColumnStorage<int64_t> ComputeDurColumn(const CounterTable& table);
  static ColumnStorage<double> ComputeDeltaColumn(const CounterTable& table);

 private:
  const CounterTable* counter_table_ = nullptr;
  std::unique_ptr<Table> counter_dur_table_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DYNAMIC_EXPERIMENTAL_COUNTER_DUR_GENERATOR_H_
