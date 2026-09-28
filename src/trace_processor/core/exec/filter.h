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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {

// Keeps the rows of each batch for which every condition holds. Nothing is
// copied: the batch out selects the rows kept.
//
// A condition means exactly what the same filter on a dataframe means: values
// are converted to the column's type, and compared, by the dataframe
// interpreter's own code. So a comparison is never true of a null, and an
// integer column compared with 2.5 keeps the rows SQL would.
class Filter : public Operator {
 public:
  // A value a column is compared with, as SQL would pass it.
  using Value = std::variant<int64_t, double, std::string>;
  struct Condition {
    uint32_t column = 0;
    // =, !=, <, <=, >, >= with one value, IS [NOT] NULL with none, or IN with
    // any number.
    Op op{Eq{}};
    std::vector<Value> values;
  };
  // A condition's IN list, cast once for all the batches of a run.
  struct CastList {
    filter::CastFilterValueListResult list;
    // The storage type index plus one it was cast to; 0 before it is.
    uint32_t type = 0;
    bool fresh = false;
  };

  // `pool` holds the column's strings and must outlive the filter.
  Filter(std::vector<Condition> conditions, const StringPool* pool);
  ~Filter() override;

  std::unique_ptr<OperatorState> MakeState() const override;
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override;

 private:
  struct State : OperatorState {
    ~State() override;
    // The rows of the batch still kept, in order.
    std::array<uint32_t, kMaxBatchRows> rows;
    // Where each kept row's value is stored in its column.
    std::array<uint32_t, kMaxBatchRows> storage;
    // By condition.
    std::vector<CastList> lists;
  };

  std::vector<Condition> conditions_;
  const StringPool* pool_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_
