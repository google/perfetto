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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ITERATOR_IMPL_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ITERATOR_IMPL_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/iterator_impl.h"

namespace perfetto::trace_processor::duckdb_integration {

// The result of executing a single relational statement inside DuckDB. Mirrors
// `PerfettoSqlConnection::ExecutionResult` (the SQLite-path equivalent) but
// carries a DuckDB-owned `duckdb_result` instead of a prepared SQLite stmt.
//
// `result` is a value struct whose underlying storage is owned by the iterator
// that wraps it; it is destroyed via `duckdb_destroy_result` in the iterator's
// destructor. Moving the struct transfers that ownership (a moved-from instance
// must NOT be destroyed again).
struct DuckDbExecutionResult {
  duckdb_result result{};
  std::vector<std::string> column_names;
  // Same shape as PerfettoSqlConnection::ExecutionStats: column count + the
  // statement counters. DuckDB executes exactly one statement here.
  uint32_t column_count = 0;
  uint32_t statement_count = 1;
  uint32_t statement_count_with_output = 1;
  std::string last_statement_sql;

  // Optional sql_stats bookkeeping hooks. `lib` populates these (capturing the
  // TraceProcessorImpl + sql_stats row) so the iterator records FirstNext / End
  // exactly like SqliteIteratorImpl, without `duckdb/` having to depend on
  // (and form a cycle with) `lib`. `on_destroy` runs at most once (it is moved
  // out into the iterator and cleared on the moved-from result).
  std::function<void()> on_first_next;
  std::function<void()> on_destroy;
};

// A third `IteratorImpl` (alongside SqliteIteratorImpl and RemoteIteratorImpl),
// backed by a DuckDB result. It streams the result via `duckdb_fetch_chunk`,
// reading cells out of the live `duckdb_data_chunk`.
//
// IMPORTANT: driven exclusively through DuckDB's C API so no C++ exception can
// unwind across the boundary into Perfetto's `-fno-exceptions` code.
//
// String/blob lifetime: `Get` returns a `SqlValue` whose `string_value` /
// `bytes_value` points into a per-column owned buffer (`string_buffers_`) that
// stays valid until the next `Next()` for that column. We MUST copy (not borrow
// the chunk's `duckdb_string_t` data directly) because a `duckdb_string_t` is
// NOT NUL-terminated, whereas the public `SqlValue::string_value` contract is a
// NUL-terminated C string (consumers call `strlen`/`%s` on it, e.g. the shell's
// PrintStats and the CSV/JSON serializers). Returning the raw chunk pointer
// caused reads past the end of the string into adjacent chunk bytes (observed
// as a "garbage byte" appended to stat names; previously misdiagnosed as a
// StringPool corruption). The owned copy is NUL-terminated and length-correct.
//
// This impl does NOT override `AsSqlite()` (returns the base `nullptr`), so the
// QueryResultSerializer fast path simply isn't taken - it goes through the
// virtual interface, exactly like RemoteIteratorImpl.
class DuckDbIteratorImpl final : public IteratorImpl {
 public:
  explicit DuckDbIteratorImpl(DuckDbExecutionResult result);
  ~DuckDbIteratorImpl() override;

  DuckDbIteratorImpl(const DuckDbIteratorImpl&) = delete;
  DuckDbIteratorImpl& operator=(const DuckDbIteratorImpl&) = delete;
  DuckDbIteratorImpl(DuckDbIteratorImpl&&) = delete;
  DuckDbIteratorImpl& operator=(DuckDbIteratorImpl&&) = delete;

  bool Next() override;
  SqlValue Get(uint32_t col) const override;
  std::string GetColumnName(uint32_t col) const override;
  base::Status Status() const override;
  uint32_t ColumnCount() const override;
  uint32_t StatementCount() const override;
  uint32_t StatementCountWithOutput() const override;
  std::string LastStatementSql() override;

 private:
  // Converts the cell at `row` of `vec` to a SqlValue. `buffer_col` selects the
  // owned string-buffer slot to use for VARCHAR/BLOB results. Handles the
  // integer/double/string/blob/decimal types and recurses for UNION (reading
  // the tag and unwrapping the active member), so a polymorphic UDF result
  // (e.g. extract_arg) surfaces in its natural SqlValue type per row.
  SqlValue ReadCell(duckdb_vector vec, idx_t row, uint32_t buffer_col) const;

  DuckDbExecutionResult result_;

  // Current chunk being iterated; null before the first Next() and after EOF.
  duckdb_data_chunk chunk_ = nullptr;
  idx_t chunk_size_ = 0;
  idx_t row_in_chunk_ = 0;

  bool called_next_ = false;
  bool exhausted_ = false;
  // Whether at least one row has been returned by Next(). Drives
  // StatementCountWithOutput() so an empty final SELECT reports 0 (matching the
  // SQLite path), avoiding the shell's "multiple queries returned rows" error.
  bool produced_row_ = false;
  base::Status status_ = base::OkStatus();

  // Per-column owned, NUL-terminated copies of the current row's string/blob
  // cells. Indexed by column; lazily grown to the column count. Each `Get(col)`
  // of a VARCHAR/BLOB overwrites slot `col` and returns a pointer into it, so
  // the returned `string_value`/`bytes_value` stays valid until the next
  // `Get(col)` for that column (i.e. at least until the next `Next()`),
  // matching the public `SqlValue` lifetime contract. `mutable` because `Get`
  // is const.
  mutable std::vector<std::string> string_buffers_;
};

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_DUCKDB_ITERATOR_IMPL_H_
