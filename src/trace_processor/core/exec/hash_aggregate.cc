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
#include <numeric>
#include <optional>
#include <utility>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// The output type of a key column. Id columns become Uint32.
StorageType KeyType(const ColumnView& column) {
  if (column.kind() == ColumnView::Kind::kSequence || column.type().Is<Id>()) {
    return StorageType{Uint32{}};
  }
  return column.type();
}

template <typename T>
void WriteValues(const GroupTable& groups,
                 uint32_t k,
                 uint32_t first,
                 uint32_t count,
                 T* out) {
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = static_cast<T>(groups.Key(first + i, k));
  }
}

// Writes key `k` of groups [first, first + count) into `buffer` as `type`.
ColumnView WriteKeys(const GroupTable& groups,
                     uint32_t k,
                     uint32_t first,
                     uint32_t count,
                     StorageType type,
                     ColumnBuffer& buffer) {
  ColumnChunk& chunk = buffer.chunk();
  void* data = chunk.Values<int64_t>();
  if (type.Is<Uint32>() || type.Is<String>()) {
    // Strings are stored as their interned id.
    WriteValues(groups, k, first, count, static_cast<uint32_t*>(data));
  } else if (type.Is<Int32>()) {
    WriteValues(groups, k, first, count, static_cast<int32_t*>(data));
  } else {
    // Int64 values are stored as is and doubles as their bits.
    WriteValues(groups, k, first, count, static_cast<int64_t*>(data));
  }
  bool nulls = false;
  for (uint32_t i = 0; i < count && !nulls; ++i) {
    nulls = groups.IsNull(first + i, k);
  }
  if (!nulls) {
    return ColumnView::Reference(type, data);
  }
  chunk.SizeValidity();
  for (uint32_t i = 0; i < count; ++i) {
    chunk.validity.change(i, !groups.IsNull(first + i, k));
  }
  return ColumnView::Reference(type, data, &chunk.validity);
}

}  // namespace

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
  // Find each row's group. Groups are numbered in the order first seen, so a
  // row starts a new group when its number equals the group count. With no
  // keys, every row is in the one group.
  s.consumed = true;
  uint32_t* row_groups = nullptr;
  if (!spec_.key_columns.empty()) {
    s.row_groups.resize(rows);
    row_groups = s.row_groups.data();
    if (std::optional<uint32_t> bad =
            s.groups.FindOrCreate(in, spec_.key_columns, row_groups)) {
      s.status = base::ErrStatus(
          "AGGREGATE: key %u must be a column holding one type", *bad + 1);
      return false;
    }
    if (s.key_types.empty()) {
      for (uint32_t column : spec_.key_columns) {
        s.key_types.push_back(KeyType(in.column(column)));
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
    if (function.reads_input()) {
      s.input.Load(in.column(spec_.aggregates[a].column), in.selection(),
                   nullptr, rows, &input);
    }
    GroupStates states{payloads + offsets_[a], stride};
    if (function.tracks_seen()) {
      states.seen = s.seen[a].For(GroupCount(s), input.valid != nullptr);
    }
    if (row_groups) {
      function.Update(input, row_groups, states, &overflow);
    } else {
      function.UpdateGroup(input, 0, states, &overflow);
    }
  }
  if (overflow) {
    s.status = base::ErrStatus("AGGREGATE: integer overflow");
    return false;
  }

  return true;
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
  // Output up to a full batch of groups.
  uint32_t count = std::min(kMaxBatchRows, groups - s.emitted);
  s.served_rows.resize(count);
  std::iota(s.served_rows.begin(), s.served_rows.end(), s.emitted);
  const uint32_t* rows = s.served_rows.data();
  for (uint32_t k = 0; k < spec_.key_columns.size(); ++k) {
    ColumnBuffer buffer = s.context->TakeBuffer();
    ColumnView view =
        WriteKeys(s.groups, k, s.emitted, count, s.key_types[k], buffer);
    out.AddColumn(view, std::move(buffer));
  }
  uint64_t stride;
  int64_t* payloads = StateBase(s, &stride);
  for (uint32_t a = 0; a < functions_.size(); ++a) {
    GroupStates states{payloads + offsets_[a], stride};
    if (functions_[a]->tracks_seen()) {
      // Without keys there is always one group, even with no rows. In that
      // case it has no value.
      states.seen = s.seen[a].For(groups, !s.consumed);
    }
    ColumnBuffer buffer =
        FinalizeToBuffer(*s.context, *functions_[a], states, rows, count);
    // Take the view before moving the buffer, since argument evaluation order
    // is unspecified.
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
  key_types.clear();
  for (SeenBytes& bytes : seen) {
    bytes.Clear();
  }
  emitted = 0;
}

}  // namespace perfetto::trace_processor::core::exec
