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

#include "src/trace_processor/duckdb/duckdb_iterator_impl.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "duckdb.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"

namespace perfetto::trace_processor::duckdb_integration {

DuckDbIteratorImpl::DuckDbIteratorImpl(DuckDbExecutionResult result)
    : result_(std::move(result)) {
  // Pre-size the per-column string buffers to the column count ONCE, so the
  // vector never reallocates while a row is being read. This is load-bearing: a
  // caller reads several columns of the same row (e.g. PrintStats reads `name`
  // then `description`), and if reading a later column grew/reallocated the
  // vector, the `const char*` returned for an EARLIER column would dangle
  // (especially for short SSO strings whose data lives inside the moved
  // std::string object). Fixed size => stable buffer addresses for the row.
  string_buffers_.resize(result_.column_count);
}

DuckDbIteratorImpl::~DuckDbIteratorImpl() {
  if (chunk_) {
    duckdb_destroy_data_chunk(&chunk_);
  }
  duckdb_destroy_result(&result_.result);
  if (result_.on_destroy) {
    result_.on_destroy();
  }
}

bool DuckDbIteratorImpl::Next() {
  // Mirror SqliteIteratorImpl::Next(): the FIRST Next() positions on the first
  // row (it must not advance past it - Get is valid right after the first Next()
  // returns true); subsequent Next()s advance one row, crossing chunk
  // boundaries as needed.
  if (!called_next_) {
    called_next_ = true;
    if (result_.on_first_next) {
      result_.on_first_next();
    }
    if (!status_.ok()) {
      return false;
    }
    chunk_ = duckdb_fetch_chunk(result_.result);
    if (!chunk_) {
      exhausted_ = true;
      return false;
    }
    chunk_size_ = duckdb_data_chunk_get_size(chunk_);
    row_in_chunk_ = 0;
    if (chunk_size_ == 0) {
      // An empty (but non-null) chunk: treat as EOF for simplicity. DuckDB
      // normally returns nullptr at end, but guard against a zero-row chunk.
      duckdb_destroy_data_chunk(&chunk_);
      chunk_ = nullptr;
      exhausted_ = true;
      return false;
    }
    produced_row_ = true;
    return true;
  }

  if (exhausted_ || !status_.ok()) {
    return false;
  }

  ++row_in_chunk_;
  if (row_in_chunk_ < chunk_size_) {
    return true;
  }

  // End of the current chunk: destroy it (this is the moment that invalidates
  // any borrowed string/blob from the previous chunk - safe under the "valid
  // until next Next()" contract) and fetch the next one.
  for (;;) {
    duckdb_destroy_data_chunk(&chunk_);
    chunk_ = duckdb_fetch_chunk(result_.result);
    if (!chunk_) {
      exhausted_ = true;
      return false;
    }
    chunk_size_ = duckdb_data_chunk_get_size(chunk_);
    row_in_chunk_ = 0;
    if (chunk_size_ > 0) {
      return true;
    }
    // Zero-row chunk: loop to fetch the next one (defensive; DuckDB usually
    // returns nullptr instead).
  }
}

SqlValue DuckDbIteratorImpl::Get(uint32_t col) const {
  PERFETTO_DCHECK(chunk_);
  PERFETTO_DCHECK(row_in_chunk_ < chunk_size_);
  duckdb_vector vec = duckdb_data_chunk_get_vector(chunk_, col);
  return ReadCell(vec, row_in_chunk_, col);
}

SqlValue DuckDbIteratorImpl::ReadCell(duckdb_vector vec,
                                      idx_t row,
                                      uint32_t col) const {
  // A null validity mask means every row in the vector is valid (no NULLs).
  uint64_t* validity = duckdb_vector_get_validity(vec);
  if (validity && !duckdb_validity_row_is_valid(validity, row)) {
    return SqlValue();  // kNull
  }

  duckdb_logical_type logical = duckdb_vector_get_column_type(vec);
  duckdb_type type_id = duckdb_get_type_id(logical);

  // UNION (e.g. the extract_arg result): physically a STRUCT with child 0 the
  // (UTINYINT) tag and child `member+1` each member's vector. Read the tag and
  // recurse into the active member so the value surfaces in its natural type.
  if (type_id == DUCKDB_TYPE_UNION) {
    duckdb_destroy_logical_type(&logical);
    duckdb_vector tag_vec = duckdb_struct_vector_get_child(vec, 0);
    auto* tag_data = static_cast<uint8_t*>(duckdb_vector_get_data(tag_vec));
    auto member = static_cast<idx_t>(tag_data[row]);
    duckdb_vector member_vec =
        duckdb_struct_vector_get_child(vec, member + 1);
    return ReadCell(member_vec, row, col);
  }

  void* data = duckdb_vector_get_data(vec);
  SqlValue value;

  // DECIMAL is stored as an integer of an internal width; SQLite has no decimal
  // type, so the legacy path surfaces these as a DOUBLE (e.g. a `5.5` literal,
  // typed DECIMAL by DuckDB, passed through `trunc`/`round`/etc.). Convert to
  // double to match. Handled before the int-widening switch since its
  // DUCKDB_TYPE_DECIMAL needs the width/scale from the (still-live) logical type.
  if (type_id == DUCKDB_TYPE_DECIMAL) {
    uint8_t width = duckdb_decimal_width(logical);
    uint8_t scale = duckdb_decimal_scale(logical);
    duckdb_type internal = duckdb_decimal_internal_type(logical);
    duckdb_hugeint hv{0, 0};
    // Switch on the underlying int to avoid -Wswitch-enum (the internal type is
    // always one of the four integer widths below).
    switch (static_cast<int>(internal)) {
      case DUCKDB_TYPE_SMALLINT:
        hv.lower = static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int16_t*>(data)[row]));
        hv.upper = static_cast<int16_t*>(data)[row] < 0 ? -1 : 0;
        break;
      case DUCKDB_TYPE_INTEGER:
        hv.lower = static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int32_t*>(data)[row]));
        hv.upper = static_cast<int32_t*>(data)[row] < 0 ? -1 : 0;
        break;
      case DUCKDB_TYPE_BIGINT: {
        int64_t v = static_cast<int64_t*>(data)[row];
        hv.lower = static_cast<uint64_t>(v);
        hv.upper = v < 0 ? -1 : 0;
        break;
      }
      case DUCKDB_TYPE_HUGEINT:
        hv = static_cast<duckdb_hugeint*>(data)[row];
        break;
      default:
        break;
    }
    duckdb_decimal dec{width, scale, hv};
    value = SqlValue::Double(duckdb_decimal_to_double(dec));
    duckdb_destroy_logical_type(&logical);
    return value;
  }
  duckdb_destroy_logical_type(&logical);
  // Switch on the underlying int to avoid -Wswitch-enum demanding every one of
  // DuckDB's ~40 logical types be listed; unsupported types hit `default`.
  switch (static_cast<int>(type_id)) {
    // All integral types widen to kLong to match SQLite's observable output.
    case DUCKDB_TYPE_BOOLEAN:
      value = SqlValue::Long(static_cast<bool*>(data)[row] ? 1 : 0);
      break;
    case DUCKDB_TYPE_TINYINT:
      value = SqlValue::Long(static_cast<int8_t*>(data)[row]);
      break;
    case DUCKDB_TYPE_SMALLINT:
      value = SqlValue::Long(static_cast<int16_t*>(data)[row]);
      break;
    case DUCKDB_TYPE_INTEGER:
      value = SqlValue::Long(static_cast<int32_t*>(data)[row]);
      break;
    case DUCKDB_TYPE_BIGINT:
      value = SqlValue::Long(static_cast<int64_t*>(data)[row]);
      break;
    case DUCKDB_TYPE_UTINYINT:
      value = SqlValue::Long(static_cast<uint8_t*>(data)[row]);
      break;
    case DUCKDB_TYPE_USMALLINT:
      value = SqlValue::Long(static_cast<uint16_t*>(data)[row]);
      break;
    case DUCKDB_TYPE_UINTEGER:
      value = SqlValue::Long(static_cast<uint32_t*>(data)[row]);
      break;
    case DUCKDB_TYPE_UBIGINT:
      // 64-bit unsigned widened (reinterpreted) into a signed 64-bit, matching
      // SQLite which has no unsigned type.
      value = SqlValue::Long(
          static_cast<int64_t>(static_cast<uint64_t*>(data)[row]));
      break;
    case DUCKDB_TYPE_HUGEINT: {
      // DuckDB's sum(BIGINT) etc. widen to HUGEINT (128-bit). SQLite returns an
      // int64; reconstruct the low-64 two's-complement value (exact when the
      // result fits int64, which it does for the sched aggregates here and
      // matches SQLite's own int64 wraparound).
      auto h = static_cast<duckdb_hugeint*>(data)[row];
      value = SqlValue::Long(static_cast<int64_t>(h.lower));
      break;
    }
    case DUCKDB_TYPE_FLOAT: {
      double d = static_cast<double>(static_cast<float*>(data)[row]);
      // SQLite cannot represent NaN: sqlite3_result_double(NaN) stores NULL, so
      // a NaN floating-point column surfaces as NULL in the legacy lane. Match
      // that here so DuckDB's output is byte-identical (DuckDB would otherwise
      // surface a real NaN that renders as "nan").
      value = std::isnan(d) ? SqlValue() : SqlValue::Double(d);
      break;
    }
    case DUCKDB_TYPE_DOUBLE: {
      double d = static_cast<double*>(data)[row];
      value = std::isnan(d) ? SqlValue() : SqlValue::Double(d);
      break;
    }
    case DUCKDB_TYPE_VARCHAR: {
      // A duckdb_string_t is NOT NUL-terminated; copy length-correct bytes into
      // an owned per-column buffer (NUL-terminated by std::string) so consumers
      // that treat string_value as a C string don't read past the end. The
      // buffer stays valid until the next Get(col)/Next() (see header).
      auto* s = &static_cast<duckdb_string_t*>(data)[row];
      PERFETTO_DCHECK(col < string_buffers_.size());
      string_buffers_[col].assign(duckdb_string_t_data(s),
                                  duckdb_string_t_length(*s));
      value = SqlValue::String(string_buffers_[col].c_str());
      break;
    }
    case DUCKDB_TYPE_BLOB: {
      // Copy into the owned per-column buffer too: the chunk's bytes are only
      // valid until the next chunk fetch, and an owned copy gives a stable
      // pointer for the SqlValue's lifetime contract.
      auto* s = &static_cast<duckdb_string_t*>(data)[row];
      PERFETTO_DCHECK(col < string_buffers_.size());
      string_buffers_[col].assign(duckdb_string_t_data(s),
                                  duckdb_string_t_length(*s));
      value = SqlValue::Bytes(string_buffers_[col].data(),
                              string_buffers_[col].size());
      break;
    }
    default:
      // Unsupported logical type: surface as NULL rather than reading garbage.
      // (The support predicate in the router restricts queries to known types;
      // this is a defensive fallback.)
      value = SqlValue();
      break;
  }
  return value;
}

std::string DuckDbIteratorImpl::GetColumnName(uint32_t col) const {
  if (col >= result_.column_names.size()) {
    return "";
  }
  return result_.column_names[col];
}

base::Status DuckDbIteratorImpl::Status() const {
  return status_;
}

uint32_t DuckDbIteratorImpl::ColumnCount() const {
  return result_.column_count;
}

uint32_t DuckDbIteratorImpl::StatementCount() const {
  return result_.statement_count;
}

uint32_t DuckDbIteratorImpl::StatementCountWithOutput() const {
  // A statement counts as "with output" only if it actually produced at least
  // one row, matching the SQLite path (IncrementCountForStmt skips a statement
  // whose first step is already done). The shell reads this strictly after the
  // first Next(), so `produced_row_` is settled by then; a final SELECT that
  // returns zero rows must report 0 here, otherwise the shell mistakes it for a
  // prior statement having produced output ("Result rows were returned for
  // multiple queries").
  return produced_row_ ? result_.statement_count_with_output : 0;
}

std::string DuckDbIteratorImpl::LastStatementSql() {
  return result_.last_statement_sql;
}

}  // namespace perfetto::trace_processor::duckdb_integration
