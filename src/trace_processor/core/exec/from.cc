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

#include "src/trace_processor/core/exec/from.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

From::From(std::vector<ColumnView> columns, RowSelection rows, uint32_t count)
    : columns_(std::move(columns)), rows_(rows), count_(count) {}

From::~From() = default;
From::State::~State() = default;

std::unique_ptr<OperatorState> From::MakeState() const {
  return std::make_unique<State>();
}

void From::Rewind(OperatorState& state) const {
  state.Cast<State>().emitted = 0;
}

bool From::GetData(RowBatch& out, OperatorState& state) const {
  State& s = state.Cast<State>();
  if (s.emitted == count_) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, count_ - s.emitted);
  RowSelection selection = RowSelection::Range(rows_.GetIndex(s.emitted));
  if (!rows_.is_range()) {
    const uint32_t* begin = rows_.data() + s.emitted;
    selection =
        RowSelection::Indices(Span<const uint32_t>(begin, begin + count));
  }
  out.Reset();
  for (const ColumnView& column : columns_) {
    out.AddColumn(column);
  }
  out.Compose(selection, count);
  out.SetCardinality(count);
  s.emitted += count;
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
