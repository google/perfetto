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
#include "perfetto/ext/base/string_view.h"
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
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
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

// A power of two, so a slot is picked with a shift.
constexpr uint32_t kInternedTextBits = 11;

}  // namespace

SqlScan::SqlScan(SqliteConnection* connection,
                 SqlSource sql,
                 core::Schema columns,
                 StringPool* pool,
                 std::shared_ptr<SqliteConnection::PreparedStatement> prepared)
    : connection_(connection),
      sql_(std::move(sql)),
      columns_(std::move(columns)),
      pool_(pool),
      prepared_(std::move(prepared)) {
  readers_.reserve(columns_.size());
  for (const core::ColumnSchema& column : columns_) {
    if (!column.type) {
      readers_.push_back(Reader::kVariant);
      continue;
    }
    switch (column.type->index()) {
      case StorageType::GetTypeIndex<core::Uint32>():
        readers_.push_back(Reader::kUint32);
        break;
      case StorageType::GetTypeIndex<core::Int32>():
        readers_.push_back(Reader::kInt32);
        break;
      case StorageType::GetTypeIndex<Int64>():
        readers_.push_back(Reader::kInt64);
        break;
      case StorageType::GetTypeIndex<Double>():
        readers_.push_back(Reader::kDouble);
        break;
      case StorageType::GetTypeIndex<String>():
        readers_.push_back(Reader::kString);
        break;
      default:
        // An Id was already materialised as a Uint32 by ResolveTypes.
        PERFETTO_FATAL("Unreachable");
    }
  }
}

SqlScan::~SqlScan() = default;
SqlScan::State::~State() = default;

std::unique_ptr<core::exec::OperatorState> SqlScan::MakeState() const {
  auto state = std::make_unique<State>();
  Prepare(*state);
  state->interned.resize(1u << kInternedTextBits);
  state->columns.reserve(columns_.size());
  state->data.reserve(columns_.size());
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    auto column = std::make_shared<ColumnChunk>();
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
      column->validity = core::BitVector::CreateWithSize(kMaxBatchRows);
    }
    state->columns.push_back(std::move(column));
    state->data.push_back(data);
  }
  return state;
}

void SqlScan::Prepare(State& state) const {
  if (prepared_) {
    state.statement.emplace(std::move(*prepared_));
    prepared_.reset();
  } else {
    state.statement.emplace(connection_->PrepareStatement(sql_));
  }
  state.status = state.statement->status();
  state.done = false;
  state.stepped = false;
  state.shape_checked = false;
}

bool SqlScan::CheckShape(State& state) const {
  sqlite3_stmt* stmt = state.statement->sqlite_stmt();
  bool same = sqlite::column::Count(stmt) == columns_.size();
  for (uint32_t i = 0; same && i < columns_.size(); ++i) {
    const char* name = sqlite::column::Name(stmt, i);
    same = columns_[i].name == (name ? name : "");
  }
  if (!same) {
    state.status =
        base::ErrStatus("SQL source: result shape changed between executions");
  }
  return same;
}

base::Status SqlScan::status(const core::exec::OperatorState& state) const {
  return state.Cast<const State>().status;
}

void SqlScan::Rewind(core::exec::OperatorState& state) const {
  State& s = state.Cast<State>();
  // A statement which could not be prepared is tried again; one which was
  // never stepped is already where a rewind would leave it.
  if (!s.statement->sqlite_stmt()) {
    Prepare(s);
    return;
  }
  if (!s.stepped) {
    return;
  }
  s.statement->Reset();
  s.status = base::OkStatus();
  s.done = false;
  s.stepped = false;
  s.shape_checked = false;
}

// Interning hashes the text and takes the pool's lock, but most text SQLite
// returns is not new: a string column of a dataframe hands SQLite a pointer
// into the pool, which comes back here untouched. Text found at the address a
// pool string is stored at is that string, so it needs neither.
StringPool::Id SqlScan::InternText(State& s, sqlite3_value* value) const {
  const char* text = sqlite::value::Text(value);
  auto address = reinterpret_cast<uintptr_t>(text);
  InternedText& slot =
      s.interned[(address * 0x9E3779B97F4A7C15ull) >> (64 - kInternedTextBits)];
  // SQLite reuses the buffers it owns, so the same address can hold other
  // text later. A pool string never changes and is only ever at one address.
  if (slot.text == text && pool_->Get(slot.id).data() == text) {
    return slot.id;
  }
  auto size = static_cast<size_t>(sqlite::value::Bytes(value));
  slot.text = text;
  slot.id = pool_->InternString(base::StringView(text, size));
  return slot.id;
}

bool SqlScan::ReadValue(State& s,
                        sqlite3_value* value,
                        uint32_t index,
                        uint32_t row) const {
  switch (readers_[index]) {
    case Reader::kVariant:
      break;
    case Reader::kUint32:
      return ReadTypedValue<uint32_t, sqlite::Type::kInteger>(s, value, index,
                                                              row);
    case Reader::kInt32:
      return ReadTypedValue<int32_t, sqlite::Type::kInteger>(s, value, index,
                                                             row);
    case Reader::kInt64:
      return ReadTypedValue<int64_t, sqlite::Type::kInteger>(s, value, index,
                                                             row);
    case Reader::kDouble:
      return ReadTypedValue<double, sqlite::Type::kFloat>(s, value, index, row);
    case Reader::kString:
      return ReadTypedValue<StringPool::Id, sqlite::Type::kText>(s, value,
                                                                 index, row);
  }
  auto* data = static_cast<Variant*>(s.data[index]);
  switch (sqlite::value::Type(value)) {
    case sqlite::Type::kInteger:
      data[row] = Variant::Int64(sqlite::value::Int64(value));
      return true;
    case sqlite::Type::kFloat:
      data[row] = Variant::Double(sqlite::value::Double(value));
      return true;
    case sqlite::Type::kText:
      data[row] = Variant::String(InternText(s, value));
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

template <typename T, sqlite::Type SqliteType>
bool SqlScan::ReadTypedValue(State& s,
                             sqlite3_value* value,
                             uint32_t index,
                             uint32_t row) const {
  auto* data = static_cast<T*>(s.data[index]);
  sqlite::Type type = sqlite::value::Type(value);
  if (PERFETTO_LIKELY(type == SqliteType)) {
    if constexpr (std::is_same_v<T, StringPool::Id>) {
      data[row] = InternText(s, value);
    } else if constexpr (std::is_same_v<T, double>) {
      data[row] = sqlite::value::Double(value);
    } else {
      data[row] = static_cast<T>(sqlite::value::Int64(value));
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
  for (const std::shared_ptr<ColumnChunk>& column : s.columns) {
    if (column->validity.size() != 0) {
      column->validity.ClearAllBits();
    }
  }
  sqlite3_stmt* stmt = s.statement->sqlite_stmt();
  uint32_t count = 0;
  s.stepped = true;
  while (count < kMaxBatchRows && s.statement->Step()) {
    if (PERFETTO_UNLIKELY(!s.shape_checked)) {
      if (!CheckShape(s)) {
        return false;
      }
      s.shape_checked = true;
    }
    // One lookup of the cell, rather than one for each thing asked of it.
    // SQLite calls such a value unprotected: nothing locks the connection
    // while it is read. Nothing has to, as a connection is only ever used by
    // one thread at a time, and the value is not kept past the next step.
    for (uint32_t i = 0; i < s.columns.size(); ++i) {
      if (!ReadValue(s, sqlite::column::Value(stmt, i), i, count)) {
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

  out.Reset();
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
