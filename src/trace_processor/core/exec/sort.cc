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

#include "src/trace_processor/core/exec/sort.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/row_layout.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/layout_column.h"
#include "src/trace_processor/core/util/ops.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

std::optional<RowLayout::Type> LayoutType(const ColumnView& column) {
  switch (column.kind()) {
    case ColumnView::Kind::kSequence:
      return RowLayout::Type::kUint32;
    case ColumnView::Kind::kFlat:
      switch (column.type().index()) {
        case StorageType::GetTypeIndex<Uint32>():
          return RowLayout::Type::kUint32;
        case StorageType::GetTypeIndex<Int32>():
          return RowLayout::Type::kInt32;
        case StorageType::GetTypeIndex<Int64>():
          return RowLayout::Type::kInt64;
        case StorageType::GetTypeIndex<Double>():
          return RowLayout::Type::kDouble;
        default:
          return std::nullopt;
      }
    case ColumnView::Kind::kVariant:
      return std::nullopt;
  }
  PERFETTO_FATAL("Unknown column kind");
}

}  // namespace

Sort::Sort(SortSpec spec) : spec_(std::move(spec)) {}
Sort::~Sort() = default;
Sort::State::~State() = default;

std::unique_ptr<Breaker::State> Sort::CreateState() const {
  auto state = std::make_unique<State>();
  state->types.resize(spec_.keys.size());
  return state;
}

bool Sort::Consume(const RowBatch& in, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  uint32_t count = in.size();
  if (count == 0) {
    return true;
  }
  bool first = !s.types.empty() && !s.types[0];
  for (uint32_t k = 0; k < spec_.keys.size(); ++k) {
    std::optional<RowLayout::Type> type =
        LayoutType(in.column(spec_.keys[k].column));
    if (!type || (s.types[k] && *s.types[k] != *type)) {
      s.status = base::ErrStatus(
          "SORT: key %u must be a column of numbers of one type", k + 1);
      return false;
    }
  }
  if (first) {
    // Record types only after every key is valid, so an error cannot leave
    // partially initialized types without a matching row layout.
    // Nullability is only known batch by batch.
    std::vector<RowLayout::Column> columns;
    for (uint32_t k = 0; k < spec_.keys.size(); ++k) {
      s.types[k] = LayoutType(in.column(spec_.keys[k].column));
      columns.push_back({*s.types[k], true, spec_.keys[k].descending});
    }
    s.layout = RowLayout(columns);
  }

  uint64_t at = uint64_t{s.rows.size()} * s.layout.stride();
  s.keys.resize(at + uint64_t{count} * s.layout.stride());
  uint8_t* rows = s.keys.data() + at;
  for (uint32_t k = 0; k < spec_.keys.size(); ++k) {
    const ColumnView& column = in.column(spec_.keys[k].column);
    const RowLayout::Slot& slot = s.layout.slot(k);
    if (column.kind() == ColumnView::Kind::kSequence) {
      WriteLayoutColumn<uint32_t>(column, slot, count, rows,
                                  [](uint32_t index) { return index; });
      continue;
    }
    const void* data = column.data();
    switch (*s.types[k]) {
      case RowLayout::Type::kUint32:
        WriteLayoutColumn<uint32_t>(
            column, slot, count, rows, [data](uint32_t index) {
              return static_cast<const uint32_t*>(data)[index];
            });
        break;
      case RowLayout::Type::kInt32:
        WriteLayoutColumn<int32_t>(
            column, slot, count, rows, [data](uint32_t index) {
              return static_cast<const int32_t*>(data)[index];
            });
        break;
      case RowLayout::Type::kInt64:
        WriteLayoutColumn<int64_t>(
            column, slot, count, rows, [data](uint32_t index) {
              return static_cast<const int64_t*>(data)[index];
            });
        break;
      case RowLayout::Type::kDouble:
        WriteLayoutColumn<double>(
            column, slot, count, rows, [data](uint32_t index) {
              return static_cast<const double*>(data)[index];
            });
        break;
    }
  }
  s.status = s.rows.Append(in);
  return s.status.ok();
}

bool Sort::Finalize(Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  uint32_t rows = s.rows.size();
  s.order.resize(rows);
  std::iota(s.order.data(), s.order.data() + rows, 0u);
  Span<uint32_t> order(s.order.data(), s.order.data() + s.order.size());
  core::ops::SortRowLayout(
      Span<const uint8_t>(s.keys.data(), s.keys.data() + s.keys.size()),
      s.layout.stride(), &order);
  return true;
}

bool Sort::Serve(RowBatch& out, Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  if (s.emitted == s.rows.size()) {
    return false;
  }
  uint32_t count = std::min(kMaxBatchRows, s.rows.size() - s.emitted);
  const uint32_t* begin = s.order.data() + s.emitted;
  s.emitted += s.rows.View(&out, Span<const uint32_t>(begin, begin + count));
  return true;
}

void Sort::Reset(Breaker::State& state) const {
  auto& s = static_cast<State&>(state);
  std::fill(s.types.begin(), s.types.end(), std::nullopt);
  s.keys.clear();
  s.rows.Clear();
  s.order.clear();
  s.emitted = 0;
}

}  // namespace perfetto::trace_processor::core::exec
