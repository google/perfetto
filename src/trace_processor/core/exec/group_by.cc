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

#include "src/trace_processor/core/exec/group_by.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

GroupBy::GroupBy(std::vector<uint32_t> key_columns)
    : key_columns_(std::move(key_columns)) {}
GroupBy::~GroupBy() = default;
GroupBy::State::~State() {
  group_keys.Clear();
}

std::unique_ptr<Breaker::State> GroupBy::CreateState() const {
  return std::make_unique<State>();
}

bool GroupBy::Consume(const RowBatch& in, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  uint32_t count = in.size();
  if (count == 0) {
    return true;
  }
  if (std::optional<uint32_t> bad = s.keys.Encode(in, key_columns_)) {
    s.status = base::ErrStatus(
        "GROUP BY: key %u must be a column holding one type", *bad + 1);
    return false;
  }
  uint64_t at = s.groups.size();
  s.groups.resize(at + count);
  uint32_t* groups = s.groups.data() + at;
  for (uint32_t row = 0; row < count; ++row) {
    std::string_view key = s.keys.Key(row);
    uint32_t group;
    if (const uint32_t* found = s.group_of.Find(key)) {
      group = *found;
    } else {
      group = static_cast<uint32_t>(s.group_of.size());
      void* copy;
      s.group_keys.Alloc(static_cast<uint32_t>(base::AlignUp<8>(key.size())),
                         &copy);
      memcpy(copy, key.data(), key.size());
      s.group_of.Insert(
          std::string_view(static_cast<const char*>(copy), key.size()), group);
    }
    groups[row] = group;
  }
  s.status = s.rows.Append(in);
  return s.status.ok();
}

bool GroupBy::Finalize(Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  // A counting sort on the group.
  auto groups = static_cast<uint32_t>(s.group_of.size());
  auto rows = static_cast<uint32_t>(s.groups.size());
  s.next.clear();
  s.next.push_back_multiple(0, groups);
  s.order.resize(rows);
  s.ordered_groups.resize(rows);
  const uint32_t* group_of_row = s.groups.data();
  uint32_t* PERFETTO_RESTRICT next = s.next.data();
  uint32_t* PERFETTO_RESTRICT order = s.order.data();
  uint32_t* PERFETTO_RESTRICT ordered_groups = s.ordered_groups.data();
  for (uint32_t row = 0; row < rows; ++row) {
    ++next[group_of_row[row]];
  }
  uint32_t start = 0;
  for (uint32_t group = 0; group < groups; ++group) {
    uint32_t end = start + next[group];
    std::fill(ordered_groups + start, ordered_groups + end, group);
    next[group] = start;
    start = end;
  }
  for (uint32_t row = 0; row < rows; ++row) {
    order[next[group_of_row[row]]++] = row;
  }
  return true;
}

bool GroupBy::Serve(RowBatch& out, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  if (s.emitted == s.rows.size()) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, s.rows.size() - s.emitted);
  const uint32_t* begin = s.order.data() + s.emitted;
  count =
      s.rows.View(&out, Span<const uint32_t>(begin, begin + count), *s.context);
  out.AddBorrowedColumn(ColumnView::Reference(
      StorageType{Uint32{}}, s.ordered_groups.data(), nullptr, s.emitted));
  s.emitted += count;
  return true;
}

void GroupBy::State::Reset() {
  Breaker::State::Reset();
  State& s = *this;
  s.group_of.Clear();
  s.group_keys.Clear();
  s.groups.clear();
  s.rows.Clear();
  s.order.clear();
  s.ordered_groups.clear();
  s.emitted = 0;
}

}  // namespace perfetto::trace_processor::core::exec
