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

#include "src/trace_processor/duckdb/clock_function.h"

#include <cstdint>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/importers/common/clock_converter.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// Reads the BIGINT input column and writes the int64 result of `convert` per
// row, NULLing rows where the input is NULL, the converter is absent, or the
// conversion fails.
template <base::StatusOr<int64_t> (ClockConverter::*Convert)(int64_t)>
void Int64Trampoline(duckdb_function_info info,
                     duckdb_data_chunk in,
                     duckdb_vector out) {
  auto* conv =
      static_cast<ClockConverter*>(duckdb_scalar_function_get_extra_info(info));
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector v0 = duckdb_data_chunk_get_vector(in, 0);
  auto* in_data = static_cast<int64_t*>(duckdb_vector_get_data(v0));
  uint64_t* in_valid = duckdb_vector_get_validity(v0);
  auto* out_data = static_cast<int64_t*>(duckdb_vector_get_data(out));
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_valid = duckdb_vector_get_validity(out);
  for (idx_t row = 0; row < n; ++row) {
    if (!conv || (in_valid && !duckdb_validity_row_is_valid(in_valid, row))) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    base::StatusOr<int64_t> r = (conv->*Convert)(in_data[row]);
    if (!r.ok()) {
      duckdb_validity_set_row_invalid(out_valid, row);
    } else {
      out_data[row] = *r;
      duckdb_validity_set_row_valid(out_valid, row);
    }
  }
}

void AbsTimeStrTrampoline(duckdb_function_info info,
                          duckdb_data_chunk in,
                          duckdb_vector out) {
  auto* conv =
      static_cast<ClockConverter*>(duckdb_scalar_function_get_extra_info(info));
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector v0 = duckdb_data_chunk_get_vector(in, 0);
  auto* in_data = static_cast<int64_t*>(duckdb_vector_get_data(v0));
  uint64_t* in_valid = duckdb_vector_get_validity(v0);
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_valid = duckdb_vector_get_validity(out);
  for (idx_t row = 0; row < n; ++row) {
    if (!conv || (in_valid && !duckdb_validity_row_is_valid(in_valid, row))) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    base::StatusOr<std::string> r = conv->ToAbsTime(in_data[row]);
    if (!r.ok()) {
      duckdb_validity_set_row_invalid(out_valid, row);
    } else {
      duckdb_vector_assign_string_element_len(out, row, r->data(), r->size());
      duckdb_validity_set_row_valid(out_valid, row);
    }
  }
}

base::Status RegisterOne(duckdb_connection conn,
                         const char* name,
                         duckdb_type return_type,
                         duckdb_scalar_function_t fn,
                         ClockConverter* converter) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, name);
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_scalar_function_add_parameter(f, bigint);
  duckdb_logical_type ret = duckdb_create_logical_type(return_type);
  duckdb_scalar_function_set_return_type(f, ret);
  duckdb_scalar_function_set_extra_info(f, converter, /*destroy=*/nullptr);
  duckdb_scalar_function_set_function(f, fn);
  duckdb_state st = duckdb_register_scalar_function(conn, f);
  duckdb_destroy_logical_type(&ret);
  duckdb_destroy_logical_type(&bigint);
  duckdb_destroy_scalar_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus("RegisterClockFunctions: '%s' failed", name);
  }
  return base::OkStatus();
}

}  // namespace

base::Status RegisterClockFunctions(duckdb_connection conn,
                                    ClockConverter* converter) {
  RETURN_IF_ERROR(RegisterOne(conn, "to_monotonic", DUCKDB_TYPE_BIGINT,
                              Int64Trampoline<&ClockConverter::ToMonotonic>,
                              converter));
  RETURN_IF_ERROR(RegisterOne(conn, "to_realtime", DUCKDB_TYPE_BIGINT,
                              Int64Trampoline<&ClockConverter::ToRealtime>,
                              converter));
  RETURN_IF_ERROR(RegisterOne(conn, "abs_time_str", DUCKDB_TYPE_VARCHAR,
                              AbsTimeStrTrampoline, converter));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
