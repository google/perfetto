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

#include "src/trace_processor/duckdb/arg_set_json_function.h"

#include <cstdint>
#include <optional>
#include <string>

#include "duckdb.h"

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

void Int64ToVarcharTrampoline(duckdb_function_info info,
                              duckdb_data_chunk in,
                              duckdb_vector out) {
  const auto* converter = static_cast<const Int64ToVarcharConverter*>(
      duckdb_scalar_function_get_extra_info(info));
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector v0 = duckdb_data_chunk_get_vector(in, 0);
  auto* in_data = static_cast<int64_t*>(duckdb_vector_get_data(v0));
  uint64_t* in_valid = duckdb_vector_get_validity(v0);
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_valid = duckdb_vector_get_validity(out);
  for (idx_t row = 0; row < n; ++row) {
    if (!converter || !*converter ||
        (in_valid && !duckdb_validity_row_is_valid(in_valid, row))) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    std::optional<std::string> s = (*converter)(in_data[row]);
    if (!s) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    duckdb_vector_assign_string_element_len(out, row, s->data(), s->size());
    duckdb_validity_set_row_valid(out_valid, row);
  }
}

}  // namespace

base::Status RegisterInt64ToVarcharFunction(
    duckdb_connection conn,
    const char* name,
    const Int64ToVarcharConverter* converter) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, name);
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
  duckdb_scalar_function_add_parameter(f, bigint);
  duckdb_scalar_function_set_return_type(f, varchar);
  duckdb_scalar_function_set_extra_info(
      f, const_cast<Int64ToVarcharConverter*>(converter), /*destroy=*/nullptr);
  duckdb_scalar_function_set_function(f, Int64ToVarcharTrampoline);
  duckdb_state st = duckdb_register_scalar_function(conn, f);
  duckdb_destroy_scalar_function(&f);
  duckdb_destroy_logical_type(&varchar);
  duckdb_destroy_logical_type(&bigint);
  if (st == DuckDBError) {
    return base::ErrStatus(
        "RegisterInt64ToVarcharFunction: failed to register '%s'", name);
  }
  return base::OkStatus();
}

base::Status RegisterArgSetJsonFunction(duckdb_connection conn,
                                        const ArgSetJsonConverter* converter) {
  return RegisterInt64ToVarcharFunction(conn, "__intrinsic_arg_set_to_json",
                                        converter);
}

}  // namespace perfetto::trace_processor::duckdb_integration
