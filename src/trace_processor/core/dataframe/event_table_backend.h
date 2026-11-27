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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_EVENT_TABLE_BACKEND_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_EVENT_TABLE_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor::core::dataframe {

// An event table keeps the last `kEventTableTailRows` to
// `2 * kEventTableTailRows` rows inserted writable in place; older rows are
// sealed and handed to its backend.
inline constexpr uint32_t kEventTableTailRows = 8192;

// Where the sealed rows of event tables go, and the writes made to them later.
// A backend may keep them in memory, or move them elsewhere, such as to disk.
class EventTableBackend {
 public:
  // A value for a cell: null, or a value of one of the types columns store.
  using Cell = std::variant<std::monostate,
                            uint32_t,
                            int32_t,
                            int64_t,
                            double,
                            StringPool::Id>;

  template <typename T>
  static Cell ToCell(const T& value) {
    return Cell(value);
  }
  template <typename T>
  static Cell ToCell(const std::optional<T>& value) {
    return value ? Cell(*value) : Cell();
  }

  virtual ~EventTableBackend();

  // Whether sealed rows stay in their tables and are written in place, so that
  // tables can write them directly rather than through WriteSealedCell().
  bool writes_sealed_rows_in_place() const {
    return writes_sealed_rows_in_place_;
  }

  // Rows [begin, end) of `table` are sealed: they will not be written in place
  // again.
  virtual void SealRows(Dataframe& table, uint32_t begin, uint32_t end) = 0;

  // Writes `cell` into `column` of `row`, a sealed row of `table`.
  virtual void WriteSealedCell(Dataframe& table,
                               uint32_t column,
                               uint32_t row,
                               Cell cell) = 0;

  // Makes every sealed row of every table in memory and up to date, so that
  // the tables can be read.
  virtual void Flush() = 0;

 protected:
  explicit EventTableBackend(bool writes_sealed_rows_in_place)
      : writes_sealed_rows_in_place_(writes_sealed_rows_in_place) {}

 private:
  const bool writes_sealed_rows_in_place_;
};

// Keeps sealed rows in their tables, and writes to them in place.
class InMemoryEventTableBackend : public EventTableBackend {
 public:
  InMemoryEventTableBackend() : EventTableBackend(true) {}
  void SealRows(Dataframe&, uint32_t, uint32_t) override {}
  void WriteSealedCell(Dataframe& table,
                       uint32_t column,
                       uint32_t row,
                       Cell cell) override;
  void Flush() override {}
};

// Keeps sealed rows in their tables, but buffers the writes to them until
// Flush(), as a backend which moves sealed rows elsewhere would: anything which
// reads a sealed row without flushing first sees a stale value.
class BufferingEventTableBackend : public EventTableBackend {
 public:
  BufferingEventTableBackend() : EventTableBackend(false) {}
  void SealRows(Dataframe&, uint32_t, uint32_t) override {}
  void WriteSealedCell(Dataframe& table,
                       uint32_t column,
                       uint32_t row,
                       Cell cell) override;
  void Flush() override;

 private:
  struct Write {
    Dataframe* table;
    uint32_t column;
    uint32_t row;
    Cell cell;
  };
  std::vector<Write> writes_;
};

// Returns the backend an event table uses until it is given another one.
EventTableBackend* DefaultEventTableBackend();

// What an event table keeps next to its dataframe.
struct EventTableState {
  // Where the table's sealed rows go.
  EventTableBackend* backend = DefaultEventTableBackend();
  // Rows below this one are sealed.
  uint32_t sealed_row_count = 0;
};

// The space a table takes in storage: an event table keeps its state right
// after its dataframe.
inline constexpr size_t kTableSlotSize =
    sizeof(Dataframe) + sizeof(EventTableState);
static_assert(sizeof(Dataframe) % alignof(EventTableState) == 0);

// Writes `cell` into `column` of `row` of `table`, whatever the type and
// nullability of the column.
void SetCell(Dataframe& table,
             uint32_t column,
             uint32_t row,
             const EventTableBackend::Cell& cell);

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_EVENT_TABLE_BACKEND_H_
