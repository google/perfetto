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

#include "src/trace_processor/core/exec/filter.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/condition.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"

namespace perfetto::trace_processor::core::exec {
Filter::Filter(std::vector<Condition> conditions, const StringPool* pool)
    : conditions_(std::move(conditions)), pool_(pool) {
  for (const Condition& c : conditions_) {
    PERFETTO_CHECK(c.op.IsAnyOf<NonStringOp>() || c.op.IsAnyOf<NullOp>() ||
                   c.op.Is<In>() || c.op.Is<Glob>());
  }
}

Filter::~Filter() = default;
Filter::State::~State() = default;

std::unique_ptr<OperatorState> Filter::MakeState(Context& context) const {
  return std::make_unique<State>(context);
}

bool Filter::Process(RowBatch& batch, OperatorState& state) const {
  State& s = state.Cast<State>();
  const FilterValue* values = s.context->filter_values().b;
  const Selection& selection = batch.selection();
  ConditionRows r{s.rows.data(), selection.size()};
  for (uint32_t i = 0; i < r.count; ++i) {
    r.rows[i] = selection[i];
  }
  for (uint32_t i = 0; i < conditions_.size() && r.count != 0; ++i) {
    const Condition& c = conditions_[i];
    const ColumnView& column = batch.column(c.column);
    if (c.op.IsAnyOf<NullOp>()) {
      KeepNulls(column, c.op.Is<IsNull>(), r);
      continue;
    }
    PERFETTO_DCHECK(column.kind() != ColumnView::Kind::kVariant);
    PERFETTO_DCHECK(c.value < s.context->filter_values().size());
    ApplyCondition(c.op, values[c.value], column, *pool_, r);
  }
  batch.mutable_selection().KeepRows(
      base::Span<const uint32_t>(r.rows, r.rows + r.count));
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
