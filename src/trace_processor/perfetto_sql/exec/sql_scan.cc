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

#include "src/trace_processor/perfetto_sql/exec/sql_scan.h"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/sqlite/bindings/sqlite_column.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_connection.h"

namespace perfetto::trace_processor::exec {
namespace {

using core::Double;
using core::Int64;
using core::StorageType;
using core::String;
using core::exec::ColumnChunk;
using core::exec::ColumnView;
using core::exec::kMaxBatchRows;
using core::exec::RowBatch;
using core::exec::RowSelection;
using core::exec::Variant;
}  // namespace

SqlScan::SqlScan(SqliteConnection* connection,
                 SqlSource sql,
                 core::Schema columns,
                 StringPool* pool)
    : connection_(connection),
      sql_(std::move(sql)),
      columns_(std::move(columns)),
      pool_(pool) {}

SqlScan::~SqlScan() = default;
SqlScan::State::~State() = default;

std::unique_ptr<core::exec::OperatorState> SqlScan::MakeState() const {
  auto state = std::make_unique<State>();
  Prepare(*state);
  return state;
}

void SqlScan::PrepareColumns(State& state) const {
  state.columns.clear();
  state.data.clear();
  state.buffers.resize(columns_.size());
  state.columns.reserve(columns_.size());
  state.data.reserve(columns_.size());
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    auto column = state.buffers[i].Acquire();
    void* data = nullptr;
    if (!columns_[i].type) {
      data = column->Values<Variant>().data();
    } else {
      switch (columns_[i].type->index()) {
        case StorageType::GetTypeIndex<core::Uint32>():
          data = column->Values<uint32_t>().data();
          break;
        case StorageType::GetTypeIndex<core::Int32>():
          data = column->Values<int32_t>().data();
          break;
        case StorageType::GetTypeIndex<Int64>():
          data = column->Values<int64_t>().data();
          break;
        case StorageType::GetTypeIndex<Double>():
          data = column->Values<double>().data();
          break;
        case StorageType::GetTypeIndex<String>():
          data = column->Values<StringPool::Id>().data();
          break;
        default:
          // An Id was already materialised as a Uint32 by ResolveTypes.
          PERFETTO_FATAL("Unreachable");
      }
      column->validity.resize(kMaxBatchRows);
    }
    state.columns.push_back(std::move(column));
    state.data.push_back(data);
  }
}

void SqlScan::Prepare(State& state) const {
  state.statement.emplace(connection_->PrepareStatement(sql_));
  state.status = state.statement->status();
  state.done = false;
  if (!state.status.ok()) {
    return;
  }
  sqlite3_stmt* stmt = state.statement->sqlite_stmt();
  uint32_t count = sqlite::column::Count(stmt);
  if (count != columns_.size()) {
    state.status =
        base::ErrStatus("SQL source: result shape changed between executions");
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const char* name = sqlite::column::Name(stmt, i);
    if (columns_[i].name != (name ? name : "")) {
      state.status = base::ErrStatus(
          "SQL source: result shape changed between executions");
      return;
    }
  }
}

base::Status SqlScan::status(const core::exec::OperatorState& state) const {
  return state.Cast<const State>().status;
}

void SqlScan::Rewind(core::exec::OperatorState& state) const {
  Prepare(state.Cast<State>());
}

bool SqlScan::ReadValue(State& s,
                        sqlite3_stmt* stmt,
                        uint32_t index,
                        uint32_t row) const {
  if (!columns_[index].type) {
    auto* data = static_cast<Variant*>(s.data[index]);
    switch (sqlite::column::Type(stmt, index)) {
      case sqlite::Type::kInteger:
        data[row] = Variant::Int64(sqlite::column::Int64(stmt, index));
        return true;
      case sqlite::Type::kFloat:
        data[row] = Variant::Double(sqlite::column::Double(stmt, index));
        return true;
      case sqlite::Type::kText:
        data[row] = Variant::String(
            pool_->InternString(sqlite::column::Text(stmt, index)));
        return true;
      case sqlite::Type::kNull:
        data[row] = Variant::Null();
        return true;
      case sqlite::Type::kBlob:
        s.status = base::ErrStatus(
            "SQL source: column '%s' holds a blob, which a pipeline cannot "
            "carry",
            columns_[index].name.c_str());
        return false;
    }
    PERFETTO_FATAL("For GCC");
  }
  switch (columns_[index].type->index()) {
    case StorageType::GetTypeIndex<core::Uint32>():
      return ReadTypedValue<uint32_t, sqlite::Type::kInteger>(s, stmt, index,
                                                              row);
    case StorageType::GetTypeIndex<core::Int32>():
      return ReadTypedValue<int32_t, sqlite::Type::kInteger>(s, stmt, index,
                                                             row);
    case StorageType::GetTypeIndex<Int64>():
      return ReadTypedValue<int64_t, sqlite::Type::kInteger>(s, stmt, index,
                                                             row);
    case StorageType::GetTypeIndex<Double>():
      return ReadTypedValue<double, sqlite::Type::kFloat>(s, stmt, index, row);
    case StorageType::GetTypeIndex<String>():
      return ReadTypedValue<StringPool::Id, sqlite::Type::kText>(s, stmt, index,
                                                                 row);
    default:
      // An Id was already materialised as a Uint32 by ResolveTypes.
      PERFETTO_FATAL("Unreachable");
  }
}

template <typename T, sqlite::Type SqliteType>
bool SqlScan::ReadTypedValue(State& s,
                             sqlite3_stmt* stmt,
                             uint32_t index,
                             uint32_t row) const {
  auto* data = static_cast<T*>(s.data[index]);
  sqlite::Type type = sqlite::column::Type(stmt, index);
  if (PERFETTO_LIKELY(type == SqliteType)) {
    if constexpr (std::is_same_v<T, StringPool::Id>) {
      data[row] = pool_->InternString(sqlite::column::Text(stmt, index));
    } else if constexpr (std::is_same_v<T, double>) {
      data[row] = sqlite::column::Double(stmt, index);
    } else {
      data[row] = static_cast<T>(sqlite::column::Int64(stmt, index));
    }
    s.columns[index]->validity.set(row);
    return true;
  }
  if (type == sqlite::Type::kNull) {
    // The row is null, but write the slot anyway. A flat column's storage is
    // readable at every row, so a reader summing it needs no per-row branch and
    // never sees a value left over from the previous batch.
    if constexpr (std::is_same_v<T, StringPool::Id>) {
      data[row] = StringPool::Id::Null();
    } else {
      data[row] = T{};
    }
    return true;
  }
  // Only reachable if the type lineage established turned out to be wrong.
  s.status = base::ErrStatus(
      "SQL source: column '%s' does not hold what it was traced back to",
      columns_[index].name.c_str());
  return false;
}

bool SqlScan::GetData(RowBatch& out, core::exec::OperatorState& state) const {
  State& s = state.Cast<State>();
  if (s.done || !s.status.ok()) {
    return false;
  }
  out.Reset();
  PrepareColumns(s);
  for (const std::shared_ptr<ColumnChunk>& column : s.columns) {
    if (column->validity.size() != 0) {
      column->validity.ClearAllBits();
    }
  }
  sqlite3_stmt* stmt = s.statement->sqlite_stmt();
  uint32_t count = 0;
  while (count < kMaxBatchRows && s.statement->Step()) {
    for (uint32_t i = 0; i < s.columns.size(); ++i) {
      if (!ReadValue(s, stmt, i, count)) {
        return false;
      }
    }
    ++count;
  }
  if (!s.statement->status().ok()) {
    s.status = s.statement->status();
    return false;
  }
  s.done = count < kMaxBatchRows;
  if (count == 0) {
    return false;
  }

  for (uint32_t i = 0; i < s.columns.size(); ++i) {
    const std::shared_ptr<ColumnChunk>& column = s.columns[i];
    if (!columns_[i].type) {
      out.AddColumn(
          ColumnView::Variants(static_cast<const Variant*>(s.data[i])), column);
    } else {
      out.AddColumn(ColumnView::Reference(*columns_[i].type, s.data[i],
                                          &column->validity),
                    column);
    }
  }
  out.Compose(RowSelection::Range(0), count);
  out.SetCardinality(count);
  return true;
}

}  // namespace perfetto::trace_processor::exec
