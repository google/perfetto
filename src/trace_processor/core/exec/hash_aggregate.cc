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

#include "src/trace_processor/core/exec/hash_aggregate.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"

namespace perfetto::trace_processor::core::exec {

HashAggregate::HashAggregate(HashAggregateSpec spec) : spec_(std::move(spec)) {
  for (const AggregateCall& call : spec_.aggregates) {
    functions_.push_back(MakeAggregateFunction(call.function));
    offsets_.push_back(state_words_);
    state_words_ += functions_.back()->state_words();
  }
}
HashAggregate::~HashAggregate() = default;
HashAggregate::State::~State() = default;

std::unique_ptr<Breaker::State> HashAggregate::CreateState() const {
  auto state = std::make_unique<State>();
  state->groups.set_payload_words(state_words_);
  state->seen.resize(functions_.size());
  state->total.push_back_multiple(0, state_words_);
  return state;
}

int64_t* HashAggregate::StateBase(State& s, uint64_t* stride) const {
  if (spec_.key_columns.empty()) {
    *stride = state_words_;
    return s.total.data();
  }
  *stride = s.groups.row_stride();
  return s.groups.payload(0);
}

uint32_t HashAggregate::GroupCount(const State& s) const {
  return spec_.key_columns.empty() ? 1 : s.groups.size();
}

bool HashAggregate::Consume(const RowBatch& in, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  uint32_t rows = in.size();
  if (rows == 0) {
    return true;
  }
  // Each row's group. Groups are numbered as first seen, so a row starts a
  // new group when its group is the next number.
  s.consumed = true;
  s.row_groups.resize(rows);
  uint32_t* row_groups = s.row_groups.data();
  s.new_rows.clear();
  if (spec_.key_columns.empty()) {
    std::fill(row_groups, row_groups + rows, 0u);
  } else {
    uint32_t before = s.groups.size();
    if (std::optional<uint32_t> bad =
            s.groups.FindOrCreate(in, spec_.key_columns, row_groups)) {
      s.status = base::ErrStatus(
          "AGGREGATE: key %u must be a column holding one type", *bad + 1);
      return false;
    }
    for (uint32_t row = 0, next = before; row < rows; ++row) {
      if (row_groups[row] == next) {
        s.new_rows.push_back(row);
        ++next;
      }
    }
  }

  uint64_t stride;
  int64_t* payloads = StateBase(s, &stride);
  bool overflow = false;
  for (uint32_t a = 0; a < functions_.size(); ++a) {
    const AggregateFunction& function = *functions_[a];
    AggregateInput input;
    input.rows = rows;
    if (function.reads_input() &&
        !s.input.Load(in.column(spec_.aggregates[a].column), in.selection(),
                      nullptr, rows, &input)) {
      s.status = base::ErrStatus("AGGREGATE: aggregated columns must be Int64");
      return false;
    }
    GroupStates states{payloads + offsets_[a], stride};
    if (function.tracks_seen()) {
      states.seen = s.seen[a].For(GroupCount(s), input.valid != nullptr);
    }
    function.Update(input, row_groups, states, &overflow);
  }
  if (overflow) {
    s.status = base::ErrStatus("AGGREGATE: integer overflow");
    return false;
  }

  // The first row of each new group holds its key.
  if (!s.new_rows.empty()) {
    s.retained.Project(in, spec_.key_columns);
    const uint32_t* new_rows = s.new_rows.data();
    s.retained.mutable_selection().Keep(
        {new_rows, new_rows + s.new_rows.size()});
    s.status = s.key_rows.Append(s.retained);
  }
  return s.status.ok();
}

bool HashAggregate::Finalize(Breaker::State&) const {
  return true;
}

bool HashAggregate::Serve(RowBatch& out, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  uint32_t groups = GroupCount(s);
  if (s.emitted == groups) {
    return false;
  }
  // A full batch of groups, whose keys may span batches of the input.
  uint32_t count = std::min(kMaxBatchRows, groups - s.emitted);
  s.served_rows.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    s.served_rows[i] = s.emitted + i;
  }
  const uint32_t* rows = s.served_rows.data();
  if (!spec_.key_columns.empty()) {
    s.key_rows.View(&s.served_keys, {rows, rows + count}, *s.context);
    for (uint32_t k = 0; k < s.served_keys.column_count(); ++k) {
      out.AddColumn(s.served_keys.column(k), s.served_keys.buffer(k));
    }
  }
  uint64_t stride;
  int64_t* payloads = StateBase(s, &stride);
  for (uint32_t a = 0; a < functions_.size(); ++a) {
    GroupStates states{payloads + offsets_[a], stride};
    if (functions_[a]->tracks_seen()) {
      // The group with no keys exists without rows: then it holds no value.
      states.seen = s.seen[a].For(groups, !s.consumed);
    }
    ColumnBuffer buffer =
        FinalizeToBuffer(*s.context, *functions_[a], states, rows, count);
    // Viewed before the move: the order arguments are evaluated in is
    // unspecified.
    ColumnView view = ResultView(buffer);
    out.AddColumn(view, std::move(buffer));
  }
  out.SetRowCount(count);
  s.emitted += count;
  return true;
}

void HashAggregate::State::Reset() {
  Breaker::State::Reset();
  groups.Clear();
  std::fill(total.begin(), total.end(), 0);
  consumed = false;
  key_rows.Clear();
  for (SeenBytes& bytes : seen) {
    bytes.Clear();
  }
  emitted = 0;
}

}  // namespace perfetto::trace_processor::core::exec
