/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/core/dataframe/cursor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/column_ref.h"
#include "src/trace_processor/core/exec/filter.h"
#include "src/trace_processor/core/exec/from.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/transient_column.h"
#include "src/trace_processor/core/interpreter/bytecode_interpreter_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::dataframe {

void Cursor::BuildOperatorTree(const Column* const* column_ptrs) {
  operator_state_.reset();
  sink_ = nullptr;
  operator_streaming_ = false;
  if (!operator_plan_.supported) {
    return;
  }
  auto state = std::make_unique<OperatorState>(operator_plan_.row_count);

  std::vector<exec::TransientColumn> columns;
  columns.reserve(operator_plan_.batch_columns.size() +
                  OperatorPlan::kFirstDataBatchColumn);
  columns.push_back(
      exec::TransientColumn::Reference(StorageType{Id{}}, nullptr, nullptr));
  for (uint32_t column : operator_plan_.batch_columns) {
    columns.push_back(BorrowColumn(*column_ptrs[column]));
  }
  state->source = std::make_unique<exec::From>(std::move(columns),
                                               exec::RowSelection::Range(0),
                                               operator_plan_.row_count);

  std::vector<std::unique_ptr<exec::Operator>> filters;
  filters.reserve(operator_plan_.filters.size());
  for (size_t i = 0; i < operator_plan_.filters.size(); ++i) {
    const OperatorPlan::Filter& f = operator_plan_.filters[i];
    filters.push_back(exec::MakeFilter(f.batch_column, f.storage, f.op,
                                       f.value_index, state->ScratchFor(i),
                                       /*contiguous_input=*/i == 0));
  }
  state->pipeline =
      std::make_unique<exec::PullPipeline>(*state->source, std::move(filters));
  state->sink = std::make_unique<exec::RowCursor>(*state->pipeline);

  operator_state_ = std::move(state);
  sink_ = operator_state_->sink.get();
}

void Cursor::ExecuteOnOperators(ValueFetcher& fetcher) {
  PERFETTO_DCHECK(operator_plan_.supported);
  if (operator_plan_.empty) {
    SetEmptyOperatorResult();
    return;
  }
  operator_state_->pipeline->Reset();
  // Each filter reads its own value here, and one that cannot match anything
  // leaves the pipeline producing nothing.
  operator_state_->pipeline->Open(fetcher);
  operator_streaming_ = true;
  sink_->Open();
}

void Cursor::Execute(ValueFetcher& filter_value_fetcher) {
  if (operator_plan_.supported) {
    ExecuteOnOperators(filter_value_fetcher);
    return;
  }
  operator_streaming_ = false;
  using S = Span<uint32_t>;
  interpreter_.Execute(filter_value_fetcher);

  const auto& span =
      *interpreter_.template GetRegisterValue<S>(params_.output_register);
  pos_ = span.b;
  end_ = span.e;
}

}  // namespace perfetto::trace_processor::core::dataframe
