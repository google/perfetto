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

#include "src/trace_processor/perfetto_sql/exec/collected_rows.h"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string_view>
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

namespace perfetto::trace_processor::exec {
namespace {

using core::StorageType;
using core::exec::ColumnChunk;
using core::exec::ColumnView;
using core::exec::kMaxBatchRows;
using core::exec::RowBatch;
using core::exec::RowSelection;
using core::exec::Variant;

StringPool::Id InternText(StringPool* pool, sqlite3_value* value) {
  const auto* text = reinterpret_cast<const char*>(sqlite3_value_text(value));
  return pool->InternString(
      std::string_view(text, static_cast<size_t>(sqlite3_value_bytes(value))));
}

}  // namespace

CollectedRows::CollectedRows(core::Schema columns, StringPool* pool)
    : columns_(std::move(columns)), pool_(pool) {}

CollectedRows::~CollectedRows() = default;

void CollectedRows::AddBatch() {
  Batch batch;
  for (const core::ColumnSchema& column : columns_) {
    auto chunk = std::make_shared<ColumnChunk>();
    void* data = nullptr;
    if (!column.type) {
      data = chunk->Values<Variant>().data();
    } else {
      switch (column.type->index()) {
        case StorageType::GetTypeIndex<core::Uint32>():
          data = chunk->Values<uint32_t>().data();
          break;
        case StorageType::GetTypeIndex<core::Int32>():
          data = chunk->Values<int32_t>().data();
          break;
        case StorageType::GetTypeIndex<core::Int64>():
          data = chunk->Values<int64_t>().data();
          break;
        case StorageType::GetTypeIndex<core::Double>():
          data = chunk->Values<double>().data();
          break;
        case StorageType::GetTypeIndex<core::String>():
          data = chunk->Values<StringPool::Id>().data();
          break;
        default:
          // An Id has no storage of its own, so it is collected as a Uint32.
          PERFETTO_FATAL("Unreachable");
      }
      chunk->validity.resize(kMaxBatchRows);
    }
    batch.columns.push_back(std::move(chunk));
    batch.data.push_back(data);
  }
  batches_.push_back(std::move(batch));
}

base::Status CollectedRows::Append(sqlite3_value** values) {
  if (batches_.empty() || batches_.back().count == kMaxBatchRows) {
    AddBatch();
  }
  Batch& batch = batches_.back();
  for (uint32_t i = 0; i < columns_.size(); ++i) {
    base::Status status = Read(batch, i, values[i]);
    if (PERFETTO_UNLIKELY(!status.ok())) {
      return status;
    }
  }
  ++batch.count;
  return base::OkStatus();
}

base::Status CollectedRows::Read(Batch& batch,
                                 uint32_t column,
                                 sqlite3_value* value) {
  if (!columns_[column].type) {
    auto* data = static_cast<Variant*>(batch.data[column]);
    Variant& out = data[batch.count];
    switch (sqlite3_value_type(value)) {
      case SQLITE_INTEGER:
        out = Variant::Int64(sqlite3_value_int64(value));
        return base::OkStatus();
      case SQLITE_FLOAT:
        out = Variant::Double(sqlite3_value_double(value));
        return base::OkStatus();
      case SQLITE_TEXT:
        out = Variant::String(InternText(pool_, value));
        return base::OkStatus();
      case SQLITE_NULL:
        out = Variant::Null();
        return base::OkStatus();
      default:
        return base::ErrStatus(
            "column '%s' holds a blob, which a pipeline cannot carry",
            columns_[column].name.c_str());
    }
  }
  switch (columns_[column].type->index()) {
    case StorageType::GetTypeIndex<core::Uint32>():
      return ReadTyped<uint32_t, SQLITE_INTEGER>(batch, column, value);
    case StorageType::GetTypeIndex<core::Int32>():
      return ReadTyped<int32_t, SQLITE_INTEGER>(batch, column, value);
    case StorageType::GetTypeIndex<core::Int64>():
      return ReadTyped<int64_t, SQLITE_INTEGER>(batch, column, value);
    case StorageType::GetTypeIndex<core::Double>():
      return ReadTyped<double, SQLITE_FLOAT>(batch, column, value);
    case StorageType::GetTypeIndex<core::String>():
      return ReadTyped<StringPool::Id, SQLITE_TEXT>(batch, column, value);
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

template <typename T, int kSqliteType>
base::Status CollectedRows::ReadTyped(Batch& batch,
                                      uint32_t column,
                                      sqlite3_value* value) {
  auto* data = static_cast<T*>(batch.data[column]);
  uint32_t row = batch.count;
  int type = sqlite3_value_type(value);
  if (PERFETTO_LIKELY(type == kSqliteType)) {
    if constexpr (std::is_same_v<T, StringPool::Id>) {
      data[row] = InternText(pool_, value);
    } else if constexpr (std::is_same_v<T, double>) {
      data[row] = sqlite3_value_double(value);
    } else {
      data[row] = static_cast<T>(sqlite3_value_int64(value));
    }
    batch.columns[column]->validity.set(row);
    return base::OkStatus();
  }
  if (type == SQLITE_NULL) {
    // The row is null, but write the slot anyway. A flat column's storage is
    // readable at every row, so a reader summing it needs no per-row branch.
    if constexpr (std::is_same_v<T, StringPool::Id>) {
      data[row] = StringPool::Id::Null();
    } else {
      data[row] = T{};
    }
    return base::OkStatus();
  }
  // Only reachable if the type the column was traced back to was wrong.
  return base::ErrStatus("column '%s' does not hold what it was traced back to",
                         columns_[column].name.c_str());
}

void CollectedRows::View(uint32_t i, RowBatch& out) const {
  const Batch& batch = batches_[i];
  out.Reset();
  for (uint32_t c = 0; c < columns_.size(); ++c) {
    const std::shared_ptr<ColumnChunk>& chunk = batch.columns[c];
    if (!columns_[c].type) {
      out.AddColumn(
          ColumnView::Variants(static_cast<const Variant*>(batch.data[c])),
          chunk);
    } else {
      out.AddColumn(ColumnView::Reference(*columns_[c].type, batch.data[c],
                                          &chunk->validity),
                    chunk);
    }
  }
  out.Compose(RowSelection::Range(0), batch.count);
  out.SetCardinality(batch.count);
}

CollectedRowsScan::CollectedRowsScan(const Inputs& inputs, uint32_t index)
    : inputs_(inputs), index_(index) {}

CollectedRowsScan::~CollectedRowsScan() = default;
CollectedRowsScan::State::~State() = default;

std::unique_ptr<core::exec::OperatorState> CollectedRowsScan::MakeState()
    const {
  return std::make_unique<State>();
}

bool CollectedRowsScan::GetData(RowBatch& out,
                                core::exec::OperatorState& state) const {
  State& s = state.Cast<State>();
  const CollectedRows& rows = *inputs_[index_];
  if (s.next_batch == rows.batch_count()) {
    return false;
  }
  rows.View(s.next_batch++, out);
  return true;
}

void CollectedRowsScan::Rewind(core::exec::OperatorState& state) const {
  state.Cast<State>().next_batch = 0;
}

base::Status CollectedRowsScan::status(const core::exec::OperatorState&) const {
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::exec
