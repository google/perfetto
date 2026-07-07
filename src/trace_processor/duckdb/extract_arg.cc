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

#include "src/trace_processor/duckdb/extract_arg.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// The UNION member ordinals of the extract_arg return type, in registration
// order: tag value `i` -> integer, `i+1` -> double, etc. The physical layout of
// a DuckDB UNION vector is a STRUCT whose child 0 is the (UTINYINT) tag and
// child `member+1` is that member's vector.
enum Member : uint8_t { kIntMember = 0, kRealMember = 1, kStringMember = 2 };

std::string_view ReadVarchar(duckdb_vector vec, idx_t row) {
  auto* data = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec));
  duckdb_string_t* s = &data[row];
  return std::string_view(duckdb_string_t_data(s), duckdb_string_t_length(*s));
}

bool IsRowNull(duckdb_vector vec, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vec);
  return validity && !duckdb_validity_row_is_valid(validity, row);
}

// The vectorized trampoline: for each (arg_set_id, key) input row, looks up the
// prebuilt index and writes the arg's value into the UNION output vector with
// the member/tag matching the arg's natural type (or sets the row NULL on a
// miss). The index MUST already be built (EnsureExtractArgIndexBuilt) - the
// trampoline never issues a query (that would re-enter execution).
void ExtractArgTrampoline(duckdb_function_info info,
                          duckdb_data_chunk in,
                          duckdb_vector out) {
  auto* st = static_cast<ExtractArgState*>(
      duckdb_scalar_function_get_extra_info(info));
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector a0 = duckdb_data_chunk_get_vector(in, 0);  // arg_set_id BIGINT
  duckdb_vector a1 = duckdb_data_chunk_get_vector(in, 1);  // key VARCHAR
  auto* asi_data = static_cast<int64_t*>(duckdb_vector_get_data(a0));

  duckdb_vector tag = duckdb_struct_vector_get_child(out, 0);
  duckdb_vector m_i = duckdb_struct_vector_get_child(out, kIntMember + 1);
  duckdb_vector m_r = duckdb_struct_vector_get_child(out, kRealMember + 1);
  duckdb_vector m_s = duckdb_struct_vector_get_child(out, kStringMember + 1);
  auto* tag_data = static_cast<uint8_t*>(duckdb_vector_get_data(tag));
  auto* i_data = static_cast<int64_t*>(duckdb_vector_get_data(m_i));
  auto* r_data = static_cast<double*>(duckdb_vector_get_data(m_r));

  duckdb_vector_ensure_validity_writable(out);
  duckdb_vector_ensure_validity_writable(m_i);
  duckdb_vector_ensure_validity_writable(m_r);
  duckdb_vector_ensure_validity_writable(m_s);
  uint64_t* out_valid = duckdb_vector_get_validity(out);
  uint64_t* vi = duckdb_vector_get_validity(m_i);
  uint64_t* vr = duckdb_vector_get_validity(m_r);
  uint64_t* vs = duckdb_vector_get_validity(m_s);

  for (idx_t row = 0; row < n; ++row) {
    // Default: the row's non-active members are NULL (so a downstream flatten
    // of an inactive member never reads garbage, especially the VARCHAR
    // member).
    duckdb_validity_set_row_invalid(vi, row);
    duckdb_validity_set_row_invalid(vr, row);
    duckdb_validity_set_row_invalid(vs, row);
    tag_data[row] = kIntMember;

    if (IsRowNull(a0, row) || IsRowNull(a1, row)) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    auto it1 = st->index.find(asi_data[row]);
    if (it1 == st->index.end()) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    auto it2 = it1->second.find(std::string(ReadVarchar(a1, row)));
    if (it2 == it1->second.end()) {
      duckdb_validity_set_row_invalid(out_valid, row);
      continue;
    }
    const ExtractArgState::Value& v = it2->second;
    switch (v.kind) {
      case ExtractArgState::Value::Kind::kInt:
        tag_data[row] = kIntMember;
        i_data[row] = v.i;
        duckdb_validity_set_row_valid(vi, row);
        break;
      case ExtractArgState::Value::Kind::kReal:
        tag_data[row] = kRealMember;
        r_data[row] = v.r;
        duckdb_validity_set_row_valid(vr, row);
        break;
      case ExtractArgState::Value::Kind::kString:
        tag_data[row] = kStringMember;
        duckdb_vector_assign_string_element_len(m_s, row, v.s.data(),
                                                v.s.size());
        duckdb_validity_set_row_valid(vs, row);
        break;
    }
  }
}

base::Status RegisterOne(duckdb_connection conn,
                         const char* name,
                         duckdb_logical_type union_type,
                         ExtractArgState* state) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, name);
  duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
  duckdb_logical_type varchar = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
  duckdb_scalar_function_add_parameter(f, bigint);
  duckdb_scalar_function_add_parameter(f, varchar);
  duckdb_destroy_logical_type(&bigint);
  duckdb_destroy_logical_type(&varchar);
  duckdb_scalar_function_set_return_type(f, union_type);
  duckdb_scalar_function_set_extra_info(f, state, /*destroy=*/nullptr);
  duckdb_scalar_function_set_function(f, ExtractArgTrampoline);
  duckdb_state st = duckdb_register_scalar_function(conn, f);
  duckdb_destroy_scalar_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus("RegisterExtractArg: failed to register '%s'", name);
  }
  return base::OkStatus();
}

}  // namespace

base::StatusOr<std::unique_ptr<ExtractArgState>> RegisterExtractArg(
    duckdb_connection conn,
    std::unordered_set<std::string>* out_registered) {
  auto state = std::make_unique<ExtractArgState>();

  // Build the UNION(i BIGINT, r DOUBLE, s VARCHAR) return type. extract_arg
  // surfaces an arg in its NATURAL type per row (matching SQLite); the iterator
  // unwraps the active member back to the right SqlValue type.
  duckdb_logical_type members[3] = {
      duckdb_create_logical_type(DUCKDB_TYPE_BIGINT),
      duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE),
      duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR),
  };
  const char* member_names[3] = {"i", "r", "s"};
  duckdb_logical_type union_type =
      duckdb_create_union_type(members, member_names, 3);
  for (duckdb_logical_type& m : members) {
    duckdb_destroy_logical_type(&m);
  }

  base::Status s1 = RegisterOne(conn, "extract_arg", union_type, state.get());
  base::Status s2 =
      RegisterOne(conn, "__intrinsic_extract_arg", union_type, state.get());
  duckdb_destroy_logical_type(&union_type);
  if (!s1.ok()) {
    return s1;
  }
  if (!s2.ok()) {
    return s2;
  }
  out_registered->insert("extract_arg");
  out_registered->insert("__intrinsic_extract_arg");
  return state;
}

base::Status EnsureExtractArgIndexBuilt(duckdb_connection conn,
                                        ExtractArgState* state) {
  if (state->built) {
    return base::OkStatus();
  }
  // Mark built up-front: even if the args table is empty or unreadable, we do
  // not want to retry on every query (a miss simply yields NULL, matching
  // SQLite's extract_arg over an absent arg).
  state->built = true;

  duckdb_result res;
  const char* kSql =
      "SELECT arg_set_id, key, value_type, int_value, real_value, "
      "string_value FROM __intrinsic_args";
  if (duckdb_query(conn, kSql, &res) == DuckDBError) {
    std::string err = duckdb_result_error(&res);
    duckdb_destroy_result(&res);
    return base::ErrStatus("EnsureExtractArgIndexBuilt: %s", err.c_str());
  }

  for (;;) {
    duckdb_data_chunk chunk = duckdb_fetch_chunk(res);
    if (!chunk) {
      break;
    }
    idx_t rows = duckdb_data_chunk_get_size(chunk);
    duckdb_vector c_asi = duckdb_data_chunk_get_vector(chunk, 0);
    duckdb_vector c_key = duckdb_data_chunk_get_vector(chunk, 1);
    duckdb_vector c_vt = duckdb_data_chunk_get_vector(chunk, 2);
    duckdb_vector c_int = duckdb_data_chunk_get_vector(chunk, 3);
    duckdb_vector c_real = duckdb_data_chunk_get_vector(chunk, 4);
    duckdb_vector c_str = duckdb_data_chunk_get_vector(chunk, 5);
    auto* asi_data = static_cast<int64_t*>(duckdb_vector_get_data(c_asi));
    auto* int_data = static_cast<int64_t*>(duckdb_vector_get_data(c_int));
    auto* real_data = static_cast<double*>(duckdb_vector_get_data(c_real));
    for (idx_t row = 0; row < rows; ++row) {
      if (IsRowNull(c_asi, row) || IsRowNull(c_key, row) ||
          IsRowNull(c_vt, row)) {
        continue;
      }
      std::string_view vt = ReadVarchar(c_vt, row);
      ExtractArgState::Value v;
      if (vt == "string" || vt == "json") {
        if (IsRowNull(c_str, row)) {
          continue;
        }
        v.kind = ExtractArgState::Value::Kind::kString;
        v.s = std::string(ReadVarchar(c_str, row));
      } else if (vt == "real") {
        if (IsRowNull(c_real, row)) {
          continue;
        }
        v.kind = ExtractArgState::Value::Kind::kReal;
        v.r = real_data[row];
      } else if (vt == "int" || vt == "uint" || vt == "bool" ||
                 vt == "pointer") {
        if (IsRowNull(c_int, row)) {
          continue;
        }
        v.kind = ExtractArgState::Value::Kind::kInt;
        v.i = int_data[row];
      } else {
        // 'null' or any unknown type: a miss yields NULL, so do not index it.
        continue;
      }
      state->index[asi_data[row]][std::string(ReadVarchar(c_key, row))] =
          std::move(v);
    }
    duckdb_destroy_data_chunk(&chunk);
  }
  duckdb_destroy_result(&res);
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
