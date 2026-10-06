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

#include "src/trace_processor/core/exec/aggregate.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

Aggregate::Aggregate(AggregateSpec spec)
    : Operator({BatchPreference::kThroughput}),
      spec_(std::move(spec)),
      aggregation_(spec_.aggregates) {}

Aggregate::~Aggregate() = default;
Aggregate::State::~State() = default;

std::unique_ptr<OperatorState> Aggregate::MakeState() const {
  auto state = std::make_unique<State>();
  state->totals = aggregation_.MakeTotals();
  state->completed_aggregates = aggregation_.MakeResults();
  return state;
}

OpResult Aggregate::Execute(const RowBatch& in,
                            RowBatch& out,
                            OperatorState& state) const {
  auto& s = static_cast<State&>(state);
  out.Reset();
  s.completed = 0;
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  if (!aggregation_.CanRead(in)) {
    s.status = base::ErrStatus("AGGREGATE: summed columns must be Int64");
    return OpResult::kError;
  }
  Aggregation::Readers values;
  aggregation_.Read(in, &values);
  bool keyed = !spec_.key_columns.empty();
  const ColumnView* groups = keyed ? &in.column(spec_.group_column) : nullptr;
  uint32_t rows = in.size();
  // Each row can complete at most the group before it.
  Reserve(s, rows);
  // A group carried over has its key first, ahead of this batch's rows.
  uint32_t first_key = s.in_group ? 1 : 0;
  s.key_row = 0;
  bool started = false;
  uint32_t sums = aggregation_.sums();
  for (uint32_t row = 0; row < rows; ++row) {
    uint32_t group = keyed ? groups->Value<uint32_t>(row) : 0;
    if (!s.in_group || group != s.group) {
      if (s.in_group) {
        Complete(s);
      }
      s.in_group = true;
      s.group = group;
      s.key_row = first_key + row;
      s.totals.Clear();
      started = true;
    }
    ++s.totals.count;
    for (uint32_t i = 0; i < sums; ++i) {
      if (PERFETTO_UNLIKELY(!s.totals.sums[i].Add(Sum::Of(values[i], row)))) {
        s.status = base::ErrStatus("AGGREGATE: integer overflow");
        return OpResult::kError;
      }
    }
  }
  // Without a new group nothing completed, and the carried key still holds.
  if (keyed && started) {
    if (!RetainKeys(in, s)) {
      return OpResult::kError;
    }
    // Only the current group's key must survive reuse of the input buffers.
    s.key_rows.View(&s.saved_key, s.key_row, 1);
  }
  return Yield(out, s);
}

OpResult Aggregate::Finish(RowBatch& out, OperatorState& state) const {
  auto& s = static_cast<State&>(state);
  out.Reset();
  s.completed = 0;
  if (!s.status.ok()) {
    return OpResult::kError;
  }
  bool keyed = !spec_.key_columns.empty();
  // All rows are one group without keys, even when there are none.
  if (s.finished || (keyed && !s.in_group)) {
    return OpResult::kNeedMoreInput;
  }
  s.finished = true;
  Reserve(s, 1);
  s.key_row = 0;
  Complete(s);
  s.in_group = false;
  if (keyed) {
    s.key_rows.Clear();
    s.status = s.key_rows.Append(s.saved_key);
    if (!s.status.ok()) {
      return OpResult::kError;
    }
  }
  return Yield(out, s);
}

void Aggregate::Reserve(State& s, uint32_t groups) const {
  s.completed_key_rows.resize(groups);
  s.completed_aggregates.Resize(groups);
}

PERFETTO_ALWAYS_INLINE void Aggregate::Complete(State& s) const {
  uint32_t n = s.completed++;
  s.completed_key_rows[n] = s.key_row;
  s.completed_aggregates.Set(n, s.totals);
}

bool Aggregate::RetainKeys(const RowBatch& in, State& s) const {
  s.key_rows.Clear();
  if (s.saved_key.size()) {
    s.status = s.key_rows.Append(s.saved_key);
    if (!s.status.ok()) {
      return false;
    }
  }
  s.retained.Reset();
  for (uint32_t column : spec_.key_columns) {
    s.retained.AddColumn(in.column(column), in.owner(column));
  }
  s.retained.SetCardinality(in.size());
  s.status = s.key_rows.Append(s.retained);
  return s.status.ok();
}

OpResult Aggregate::Yield(RowBatch& out, State& s) const {
  uint32_t count = s.completed;
  if (count == 0) {
    return OpResult::kNeedMoreInput;
  }
  if (!spec_.key_columns.empty()) {
    const uint32_t* rows = s.completed_key_rows.data();
    s.key_rows.View(&s.served_keys, {rows, rows + count});
    for (uint32_t k = 0; k < s.served_keys.column_count(); ++k) {
      out.AddColumn(s.served_keys.column(k), s.served_keys.owner(k));
    }
  }
  aggregation_.AddColumns(s.completed_aggregates, &out);
  out.SetCardinality(count);
  return OpResult::kNeedMoreInput;
}

void Aggregate::State::Reset() {
  State& s = *this;
  s.status = base::OkStatus();
  s.finished = false;
  s.in_group = false;
  s.group = 0;
  s.totals.Clear();
  s.key_rows.Clear();
  s.retained.Reset();
  s.saved_key.Reset();
  s.key_row = 0;
  s.completed = 0;
  s.served_keys.Reset();
}

base::Status Aggregate::status(const OperatorState& state) const {
  return static_cast<const State&>(state).status;
}

}  // namespace perfetto::trace_processor::core::exec
