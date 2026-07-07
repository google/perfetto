/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_SQL_LOWERING_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_SQL_LOWERING_H_

#include <cstdint>
#include <string>
#include <vector>

namespace perfetto::trace_processor::duckdb_integration {

// A single byte-range replacement against a source string: replace the `len`
// bytes at `offset` with `repl` (`len == 0` is a pure insertion). Offsets are
// into the string passed to ApplyEdits().
struct SpanEdit {
  uint32_t offset;
  uint32_t len;
  std::string repl;
};

// Splices `edits` into `src` in one pass. Edits are sorted by offset; exact
// duplicates collapse; any edit overlapping an already-applied one is dropped
// (fail-safe: a structurally impossible overlap never corrupts output). The
// relative order of a zero-length insertion and a replacement at the same
// offset is preserved from `edits` (stable sort), so callers can insert before
// a span they also rewrite.
std::string ApplyEdits(const std::string& src, std::vector<SpanEdit> edits);

// Lowers SQLite-dialect SQL to DuckDB-dialect SQL in ONE syntaqlite-AST pass,
// replacing the run-path string/token rewrites:
//   - format(...)  -> printf(...)            (DuckDB printf is C-style)
//   - char(X)      -> chr(CAST(X AS INTEGER))(single-arg only; DuckDB has no
//                                             char and chr takes a 32-bit INT)
//   - _auto_id     -> (row_number() OVER ()-1) for an unqualified reference in
//   a
//                     1:1 single-relation projection (bailed otherwise)
//
// `sql` must be macro-expanded (the run path expands macros upstream). If it
// fails to parse, the input is returned unchanged: DuckDB then errors on it and
// the caller falls back to SQLite exactly as before, so this pass can only
// rewrite what it fully understood.
std::string LowerSqlForDuckDb(const std::string& sql);

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_SQL_LOWERING_H_
