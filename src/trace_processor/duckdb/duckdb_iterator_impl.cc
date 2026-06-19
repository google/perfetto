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
    : result_(std::move(result)) {}

DuckDbIteratorImpl::~DuckDbIteratorImpl() {
  if (chunk_) {
    duckdb_destroy_data_chunk(&chunk_);
  }
  duckdb_destroy_result(&result_.result);
}

bool DuckDbIteratorImpl::Next() {
  // Mirror SqliteIteratorImpl::Next(): the FIRST Next() positions on the first
  // row (it must not advance past it - Get is valid right after the first Next()
  // returns true); subsequent Next()s advance one row, crossing chunk
  // boundaries as needed.
  if (!called_next_) {
    called_next_ = true;
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

  // A null validity mask means every row in the vector is valid (no NULLs).
  uint64_t* validity = duckdb_vector_get_validity(vec);
  if (validity && !duckdb_validity_row_is_valid(validity, row_in_chunk_)) {
    return SqlValue();  // kNull
  }

  duckdb_logical_type logical = duckdb_vector_get_column_type(vec);
  duckdb_type type_id = duckdb_get_type_id(logical);
  duckdb_destroy_logical_type(&logical);

  void* data = duckdb_vector_get_data(vec);
  const idx_t row = row_in_chunk_;
  SqlValue value;
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
    case DUCKDB_TYPE_FLOAT:
      value = SqlValue::Double(
          static_cast<double>(static_cast<float*>(data)[row]));
      break;
    case DUCKDB_TYPE_DOUBLE:
      value = SqlValue::Double(static_cast<double*>(data)[row]);
      break;
    case DUCKDB_TYPE_VARCHAR: {
      // Borrow into the live chunk: valid until the next Next() (see header).
      auto* s = &static_cast<duckdb_string_t*>(data)[row];
      value = SqlValue::String(duckdb_string_t_data(s));
      break;
    }
    case DUCKDB_TYPE_BLOB: {
      auto* s = &static_cast<duckdb_string_t*>(data)[row];
      value = SqlValue::Bytes(duckdb_string_t_data(s),
                              duckdb_string_t_length(*s));
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
  return result_.statement_count_with_output;
}

std::string DuckDbIteratorImpl::LastStatementSql() {
  return result_.last_statement_sql;
}

}  // namespace perfetto::trace_processor::duckdb_integration
