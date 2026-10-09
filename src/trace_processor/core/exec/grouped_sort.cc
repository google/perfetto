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

#include "src/trace_processor/core/exec/grouped_sort.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/sorted_rows.h"

namespace perfetto::trace_processor::core::exec {

GroupedSort::GroupedSort(GroupedSortSpec spec) : spec_(std::move(spec)) {}
GroupedSort::~GroupedSort() = default;
GroupedSort::State::~State() = default;

std::unique_ptr<Breaker::State> GroupedSort::CreateState() const {
  return std::make_unique<State>();
}

bool GroupedSort::Consume(const RowBatch& in, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  uint32_t count = in.size();
  if (count == 0) {
    return true;
  }
  uint64_t at = s.groups.size();
  s.groups.resize(at + count);
  if (std::optional<uint32_t> bad =
          s.group_table.FindOrCreate(in, spec_.groups, s.groups.data() + at)) {
    s.groups.resize(at);
    s.status = base::ErrStatus(
        "GROUPED SORT: group %u must be a column holding one type", *bad + 1);
    return false;
  }
  base::Status status = s.rows.Append(in, spec_.keys);
  if (!status.ok()) {
    s.status = base::ErrStatus("GROUPED SORT: %s", status.c_message());
    return false;
  }
  return true;
}

// Sorts by the keys, if any, then brings each group's rows together keeping
// that order: a stable counting sort of the order by group.
bool GroupedSort::Finalize(Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  bool sorted = !spec_.keys.empty();
  if (sorted) {
    s.rows.Sort();
  }
  FlexVector<uint32_t>& order = s.rows.order();
  uint32_t rows = s.rows.size();
  uint32_t groups = s.group_table.size();
  const uint32_t* group_of_row = s.groups.data();
  s.next.clear();
  s.next.push_back_multiple(0, groups);
  uint32_t* PERFETTO_RESTRICT next = s.next.data();
  for (uint32_t row = 0; row < rows; ++row) {
    ++next[group_of_row[row]];
  }
  s.ordered_groups.resize(rows);
  uint32_t* PERFETTO_RESTRICT ordered_groups = s.ordered_groups.data();
  uint32_t start = 0;
  for (uint32_t group = 0; group < groups; ++group) {
    uint32_t end = start + next[group];
    std::fill(ordered_groups + start, ordered_groups + end, group);
    next[group] = start;
    start = end;
  }
  if (!sorted) {
    // In the input's order, the rows are their own numbers.
    order.resize(rows);
    uint32_t* PERFETTO_RESTRICT grouped = order.data();
    for (uint32_t row = 0; row < rows; ++row) {
      grouped[next[group_of_row[row]]++] = row;
    }
    return true;
  }
  s.scratch.resize(rows);
  uint32_t* PERFETTO_RESTRICT grouped = s.scratch.data();
  for (uint32_t i = 0; i < rows; ++i) {
    uint32_t row = order[i];
    grouped[next[group_of_row[row]]++] = row;
  }
  std::swap(order, s.scratch);
  return true;
}

bool GroupedSort::Serve(RowBatch& out, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  if (s.emitted == s.rows.size()) {
    return false;
  }
  uint32_t count = s.rows.View(&out, s.emitted, *s.context);
  out.AddBorrowedColumn(ColumnView::Reference(
      StorageType{Uint32{}}, s.ordered_groups.data(), nullptr, s.emitted));
  s.emitted += count;
  return true;
}

void GroupedSort::State::Reset() {
  Breaker::State::Reset();
  group_table.Clear();
  rows.Clear();
  groups.clear();
  ordered_groups.clear();
  emitted = 0;
}

}  // namespace perfetto::trace_processor::core::exec
