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

#include "src/trace_processor/core/dataframe/event_table_backend.h"

#include <cstdint>
#include <optional>
#include <variant>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/no_destructor.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

template <typename T, typename V>
void SetTypedCell(Dataframe& table,
                  uint32_t column,
                  uint32_t row,
                  const EventTableBackend::Cell& cell) {
  Nullability n = table.GetNullabilityLegacy(column);
  if (n.Is<NonNull>()) {
    table.SetCellUncheckedLegacy<T, NonNull>(column, row, std::get<V>(cell));
    return;
  }
  std::optional<V> value;
  if (!std::holds_alternative<std::monostate>(cell)) {
    value = std::get<V>(cell);
  }
  if (n.Is<DenseNull>()) {
    table.SetCellUncheckedLegacy<T, DenseNull>(column, row, value);
  } else if (n.Is<SparseNullWithPopcountAlways>()) {
    table.SetCellUncheckedLegacy<T, SparseNullWithPopcountAlways>(column, row,
                                                                  value);
  } else if (n.Is<SparseNullWithPopcountUntilFinalization>()) {
    table.SetCellUncheckedLegacy<T, SparseNullWithPopcountUntilFinalization>(
        column, row, value);
  } else {
    PERFETTO_FATAL("Unsupported nullability for a sealed cell");
  }
}

}  // namespace

EventTableBackend::~EventTableBackend() = default;

EventTableBackend* DefaultEventTableBackend() {
  static base::NoDestructor<InMemoryEventTableBackend> backend;
  return &backend.ref();
}

void InMemoryEventTableBackend::WriteSealedCell(Dataframe& table,
                                                uint32_t column,
                                                uint32_t row,
                                                Cell cell) {
  SetCell(table, column, row, cell);
}

void BufferingEventTableBackend::WriteSealedCell(Dataframe& table,
                                                 uint32_t column,
                                                 uint32_t row,
                                                 Cell cell) {
  writes_.push_back(Write{&table, column, row, cell});
}

void BufferingEventTableBackend::Flush() {
  for (const Write& write : writes_) {
    SetCell(*write.table, write.column, write.row, write.cell);
  }
  writes_.clear();
}

void SetCell(Dataframe& table,
             uint32_t column,
             uint32_t row,
             const EventTableBackend::Cell& cell) {
  switch (table.column_type(column).index()) {
    case StorageType::GetTypeIndex<Uint32>():
      SetTypedCell<Uint32, uint32_t>(table, column, row, cell);
      break;
    case StorageType::GetTypeIndex<Int32>():
      SetTypedCell<Int32, int32_t>(table, column, row, cell);
      break;
    case StorageType::GetTypeIndex<Int64>():
      SetTypedCell<Int64, int64_t>(table, column, row, cell);
      break;
    case StorageType::GetTypeIndex<Double>():
      SetTypedCell<Double, double>(table, column, row, cell);
      break;
    case StorageType::GetTypeIndex<String>():
      SetTypedCell<String, StringPool::Id>(table, column, row, cell);
      break;
    default:
      PERFETTO_FATAL("Unsupported type for a sealed cell");
  }
}

}  // namespace perfetto::trace_processor::core::dataframe
