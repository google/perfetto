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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_QUERY_SCAN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_QUERY_SCAN_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/dataframe_scan.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {

// The rows of `dataframe` every condition holds for, in increasing order,
// found by the dataframe's own planner, as a query on the dataframe finds them:
// so an index or a sorted column serves them. Each condition tests the
// dataframe's column at its `column`, with its values or those its parameter
// has in `params`.
FlexVector<uint32_t> RunDataframeQuery(
    const dataframe::Dataframe& dataframe,
    const std::vector<Filter::Condition>& conditions,
    const Filter::Params& params);

// Reads the rows of a dataframe a query keeps, running the query as each run
// starts: its values can be parameters, which change between runs.
class DataframeQueryScan : public DataframeScan {
 public:
  // Reads `columns` of `dataframe` where every condition holds. The
  // dataframe and `params` must outlive the scan.
  DataframeQueryScan(
      const dataframe::Dataframe* dataframe,
      std::vector<std::shared_ptr<const dataframe::Column>> columns,
      std::vector<Filter::Condition> conditions,
      const Filter::Params* params);
  ~DataframeQueryScan() override;

 protected:
  std::unique_ptr<OperatorState> MakeRowsState() const override;
  std::shared_ptr<const FlexVector<uint32_t>> FindRows(
      OperatorState* rows_state) const override;

 private:
  struct RowsState;

  const dataframe::Dataframe* dataframe_;
  std::vector<Filter::Condition> conditions_;
  const Filter::Params* params_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_QUERY_SCAN_H_
