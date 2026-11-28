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

#include "src/trace_processor/core/exec/top_k.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/span.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/common/string_ranks.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sorted_rows.h"

namespace perfetto::trace_processor::core::exec {

TopK::TopK(TopKSpec spec) : spec_(spec) {}
OperatorState* TopK::NewSinkState(Context& context) const {
  auto* state = context.Make<State>();
  state->rows.SetUnread(unread_);
  state->spare.SetUnread(unread_);
  state->passing = context.NewSpan<uint32_t>(kMaxBatchRows);
  state->strings = context.NewSpan<FlatColumnReader<StringPool::Id>>(
      static_cast<uint32_t>(spec_.keys.size()));
  state->passed = context.NewBatch(*state);
  return state;
}

bool TopK::Consume(const RowBatch& in, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  if (in.size() == 0) {
    return true;
  }
  // Keys are checked even when no row is output.
  base::Status status = s.rows.CheckKeys(in, spec_.keys, spec_.pool, s);
  if (status.ok() && spec_.count > 0) {
    status = Add(in, s);
  }
  if (!status.ok()) {
    s.context().Fail(base::ErrStatus("SORT: %s", status.c_message()));
    return false;
  }
  return true;
}

base::Status TopK::Add(const RowBatch& in, State& s) const {
  if (s.has_cutoff) {
    const RowBatch& passed = BeforeCutoff(in, s);
    base::Status status =
        s.rows.AppendLaidOut(passed, spec_.keys, spec_.pool, s, s.keys.data());
    // Not to keep the batch's buffers until the next one.
    s.passed->Reset();
    RETURN_IF_ERROR(status);
  } else {
    RETURN_IF_ERROR(s.rows.Append(in, spec_.keys, spec_.pool, s));
  }
  if (s.rows.size() >=
      uint64_t{spec_.count} + std::max(spec_.count, kMaxBatchRows)) {
    return Prune(s);
  }
  return base::OkStatus();
}

const RowBatch& TopK::BeforeCutoff(const RowBatch& in, State& s) const {
  uint32_t stride = s.rows.stride();
  s.keys.resize(s, uint64_t{in.size()} * stride);
  uint8_t* keys = s.keys.data();
  s.rows.LayOutKeys(in, spec_.keys, keys);
  uint32_t strings = 0;
  for (uint32_t k = 0; k < spec_.keys.size(); ++k) {
    if (s.rows.is_string(k)) {
      s.strings[strings++] =
          FlatColumnReader<StringPool::Id>(in, spec_.keys[k].column);
    }
  }
  uint32_t passing = 0;
  for (uint32_t row = 0, n = in.size(); row < n; ++row) {
    const uint8_t* row_keys = keys + uint64_t{row} * stride;
    // A row equal to the cutoff comes after it, having arrived later.
    if (CompareToCutoff(row, row_keys, s) >= 0) {
      continue;
    }
    uint8_t* to = keys + uint64_t{passing} * stride;
    if (to != row_keys) {
      memcpy(to, row_keys, stride);
    }
    s.passing[passing++] = row;
  }
  s.passed->CopyFrom(in);
  s.passed->mutable_selection().Keep(base::Span<const uint32_t>(
      s.passing.begin(), s.passing.begin() + passing));
  return *s.passed;
}

int TopK::CompareToCutoff(uint32_t row,
                          const uint8_t* keys,
                          const State& s) const {
  const SortedRows& rows = s.rows;
  if (s.cutoff_strings.empty()) {
    return memcmp(keys, s.cutoff.data(), rows.stride());
  }
  const FlatColumnReader<StringPool::Id>* string = s.strings.begin();
  for (uint32_t k = 0; k < spec_.keys.size(); ++k) {
    const RowLayout::Slot& slot = rows.slot(k);
    int c;
    if (rows.is_string(k)) {
      StringPool::Id id;
      bool present = (string++)->Read(row, &id);
      const std::optional<StringPool::Id>& cutoff = s.cutoff_strings[k];
      if (!present || !cutoff) {
        // Nulls come first, as in Sort.
        c = int{present} - int{cutoff.has_value()};
      } else {
        c = StringRanks::Compare(*spec_.pool, id, *cutoff);
      }
      c = slot.descending ? -c : c;
    } else {
      uint32_t end =
          k + 1 < spec_.keys.size() ? rows.slot(k + 1).offset : rows.stride();
      c = memcmp(keys + slot.offset, s.cutoff.data() + slot.offset,
                 end - slot.offset);
    }
    if (c != 0) {
      return c;
    }
  }
  return 0;
}

base::Status TopK::Prune(State& s) const {
  SortedRows& rows = s.rows;
  rows.Sort(s);
  rows.order().resize(s, spec_.count);
  RowBatch first;
  for (uint32_t at = 0; at < spec_.count;) {
    at += rows.GatherInPlace(&first, at, s);
    RETURN_IF_ERROR(s.spare.Append(first, spec_.keys, spec_.pool, s));
  }
  rows.Clear();
  std::swap(s.rows, s.spare);

  const SortedRows& kept = s.rows;
  uint32_t last = spec_.count - 1;
  uint32_t stride = kept.stride();
  s.cutoff.resize(s, stride);
  memcpy(s.cutoff.data(), kept.keys(last), stride);
  s.cutoff_strings.clear();
  for (uint32_t k = 0; k < spec_.keys.size(); ++k) {
    if (kept.is_string(k)) {
      if (s.cutoff_strings.empty()) {
        s.cutoff_strings.assign(s, spec_.keys.size(), std::nullopt);
      }
      s.cutoff_strings[k] = kept.string(k, last);
    }
  }
  s.has_cutoff = true;
  return base::OkStatus();
}

bool TopK::Finalize(Breaker::State& state) const {
  SortedRows& rows = static_cast<State&>(state).rows;
  rows.Sort(state);
  if (spec_.count < rows.size()) {
    rows.order().resize(state, spec_.count);
  }
  return true;
}

bool TopK::Serve(RowBatch& out, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  if (s.emitted == s.rows.order().size()) {
    return false;
  }
  s.emitted += s.rows.GatherInPlace(&out, s.emitted, s);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
