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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

GroupBy::GroupBy(std::vector<uint32_t> key_columns)
    : key_columns_(std::move(key_columns)) {}
GroupBy::~GroupBy() = default;
GroupBy::State::~State() = default;

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
  for (uint32_t row = 0; row < count; ++row) {
    std::string_view key = s.keys.Key(row);
    uint32_t group;
    if (const uint32_t* found = s.group_of.Find(key)) {
      group = *found;
    } else {
      group = static_cast<uint32_t>(s.group_of.size());
      s.group_of.Insert(std::string(key), group);
    }
    s.groups.push_back(group);
  }
  s.status = s.rows.Append(in);
  return s.status.ok();
}

bool GroupBy::Finalize(Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  // A counting sort on the group.
  auto groups = static_cast<uint32_t>(s.group_of.size());
  auto next = FlexVector<uint32_t>::CreateFilled(groups, 0);
  for (uint32_t group : s.groups) {
    ++next[group];
  }
  uint32_t start = 0;
  for (uint32_t& at : next) {
    uint32_t rows = at;
    at = start;
    start += rows;
  }
  s.order.resize(s.groups.size());
  s.ordered_groups.resize(s.groups.size());
  for (uint32_t row = 0; row < s.groups.size(); ++row) {
    uint32_t at = next[s.groups[row]]++;
    s.order[at] = row;
    s.ordered_groups[at] = s.groups[row];
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
  count = s.rows.View(&out, Span<const uint32_t>(begin, begin + count));
  ColumnView group =
      ColumnView::Reference(StorageType{Uint32{}}, s.ordered_groups.data());
  group.SetRange(s.emitted);
  out.AddColumn(group);
  s.emitted += count;
  return true;
}

void GroupBy::Reset(Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  s.group_of.Clear();
  s.groups.clear();
  s.rows.Clear();
  s.order.clear();
  s.ordered_groups.clear();
  s.emitted = 0;
}

}  // namespace perfetto::trace_processor::core::exec
