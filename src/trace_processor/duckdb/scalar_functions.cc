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

#include "src/trace_processor/duckdb/scalar_functions.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/regex.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"

namespace perfetto::trace_processor::duckdb_integration {
namespace {

// --- Small vector-access helpers (mirror sched_table_function.cc). -----------

// Reads a VARCHAR cell as a string_view borrowing into the input chunk. The
// returned view is valid for the duration of the trampoline call.
std::string_view ReadVarchar(duckdb_vector vec, idx_t row) {
  auto* data = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec));
  duckdb_string_t* s = &data[row];
  return std::string_view(duckdb_string_t_data(s), duckdb_string_t_length(*s));
}

// True iff `row` of `vec` is NULL (handles the all-valid NULL validity mask).
bool IsRowNull(duckdb_vector vec, idx_t row) {
  uint64_t* validity = duckdb_vector_get_validity(vec);
  return validity && !duckdb_validity_row_is_valid(validity, row);
}

// The reusable registration primitive: builds a DuckDB scalar function from a
// name, parameter logical-type ids, a return logical-type id, and a vectorized
// trampoline, then registers it on `conn`. This is the mechanism the broader
// UDF tier reuses. Records the (lowercased) name in `out_registered` on success.
//
// `extra_info`/`destroy` are optional (nullptr for pure, context-free
// functions). All logical types are created+destroyed here (C API owns them).
base::Status RegisterScalarFunction(
    duckdb_connection conn,
    const char* name,
    const std::vector<duckdb_type>& param_types,
    duckdb_type return_type,
    duckdb_scalar_function_t trampoline,
    std::unordered_set<std::string>* out_registered,
    void* extra_info = nullptr,
    duckdb_delete_callback_t destroy = nullptr) {
  duckdb_scalar_function f = duckdb_create_scalar_function();
  duckdb_scalar_function_set_name(f, name);
  for (duckdb_type t : param_types) {
    duckdb_logical_type lt = duckdb_create_logical_type(t);
    duckdb_scalar_function_add_parameter(f, lt);
    duckdb_destroy_logical_type(&lt);
  }
  duckdb_logical_type ret = duckdb_create_logical_type(return_type);
  duckdb_scalar_function_set_return_type(f, ret);
  duckdb_destroy_logical_type(&ret);
  // These functions can return NULL for non-NULL input (e.g. ln of a negative),
  // so they must not be treated as NULL-propagating-only. They are deterministic
  // (default), which is correct.
  if (extra_info) {
    duckdb_scalar_function_set_extra_info(f, extra_info, destroy);
  }
  duckdb_scalar_function_set_function(f, trampoline);
  duckdb_state st = duckdb_register_scalar_function(conn, f);
  duckdb_destroy_scalar_function(&f);
  if (st == DuckDBError) {
    return base::ErrStatus(
        "RegisterScalarFunctions: failed to register '%s'", name);
  }
  out_registered->insert(base::ToLower(name));
  return base::OkStatus();
}

// --- Trampolines. ------------------------------------------------------------
//
// Each trampoline processes a whole input chunk. Output validity is made
// writable up-front so per-row NULLs can be set.

// double -> double, applying `fn` per row. NULL in -> NULL out. The functor is
// passed as a plain function pointer so the trampoline can stay a C-compatible
// `duckdb_scalar_function_t` while sharing the loop.
template <double (*Fn)(double, bool*)>
void MathTrampoline(duckdb_function_info, duckdb_data_chunk in,
                    duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector a0 = duckdb_data_chunk_get_vector(in, 0);
  auto* in_data = static_cast<double*>(duckdb_vector_get_data(a0));
  auto* out_data = static_cast<double*>(duckdb_vector_get_data(out));
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_validity = duckdb_vector_get_validity(out);
  for (idx_t i = 0; i < n; ++i) {
    if (IsRowNull(a0, i)) {
      duckdb_validity_set_row_invalid(out_validity, i);
      continue;
    }
    bool is_null = false;
    double r = Fn(in_data[i], &is_null);
    if (is_null) {
      duckdb_validity_set_row_invalid(out_validity, i);
    } else {
      out_data[i] = r;
    }
  }
}

// The VARCHAR overload of the math functions. Perfetto's intrinsics return NULL
// for a text argument (the SQLite path tests `NumericType` and falls through to
// NULL). DuckDB would otherwise implicitly cast the string to DOUBLE and error,
// so we register an explicit (VARCHAR)->DOUBLE overload that always yields NULL.
void MathVarcharNull(duckdb_function_info, duckdb_data_chunk in,
                     duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_validity = duckdb_vector_get_validity(out);
  for (idx_t i = 0; i < n; ++i) {
    duckdb_validity_set_row_invalid(out_validity, i);
  }
}

// Pure math bodies (shared semantics with math_functions/math.cc). `*is_null` is
// set when the result is undefined (matching the SQLite intrinsic's NULL).
double LnImpl(double v, bool* is_null) {
  if (v > 0.0) {
    return std::log(v);
  }
  *is_null = true;  // ln of <= 0 is NULL (matches __intrinsic_ln).
  return 0.0;
}
double ExpImpl(double v, bool*) {
  return std::exp(v);
}
double SqrtImpl(double v, bool*) {
  return std::sqrt(v);
}

// regexp_extract(input VARCHAR, pattern VARCHAR) -> VARCHAR. Mirrors
// __intrinsic_regexp_extract: returns group 1 if the (single) capture group
// matched, else the full match; NULL on no match. Errors if the pattern has
// more than one group, or if the pattern is invalid.
void RegexpExtractTrampoline(duckdb_function_info info, duckdb_data_chunk in,
                             duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector a0 = duckdb_data_chunk_get_vector(in, 0);
  duckdb_vector a1 = duckdb_data_chunk_get_vector(in, 1);
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_validity = duckdb_vector_get_validity(out);
  for (idx_t i = 0; i < n; ++i) {
    if (IsRowNull(a0, i) || IsRowNull(a1, i)) {
      duckdb_validity_set_row_invalid(out_validity, i);
      continue;
    }
    std::string_view text = ReadVarchar(a0, i);
    std::string pattern(ReadVarchar(a1, i));
    base::StatusOr<base::Regex> re = base::Regex::Create(pattern);
    if (!re.ok()) {
      duckdb_scalar_function_set_error(info, re.status().c_message());
      return;
    }
    std::vector<std::string_view> matches;
    re->PartialMatchWithGroups(text, matches);
    if (matches.empty()) {
      duckdb_validity_set_row_invalid(out_validity, i);
      continue;
    }
    // groups[0] = full match, groups[1] = first subexpression.
    if (matches.size() > 2) {
      duckdb_scalar_function_set_error(
          info, "REGEXP_EXTRACT: pattern has more than one group.");
      return;
    }
    std::string_view result =
        (matches.size() == 2 && !matches[1].empty()) ? matches[1] : matches[0];
    duckdb_vector_assign_string_element_len(out, i, result.data(),
                                            result.size());
  }
}

// unhex(input VARCHAR) -> BIGINT. Mirrors __intrinsic_unhex: trims whitespace,
// strips an optional 0x/0X prefix, parses base-16 into an int64. NULL in ->
// NULL out; errors on empty / invalid / out-of-range.
void UnhexTrampoline(duckdb_function_info info, duckdb_data_chunk in,
                     duckdb_vector out) {
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector a0 = duckdb_data_chunk_get_vector(in, 0);
  auto* out_data = static_cast<int64_t*>(duckdb_vector_get_data(out));
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_validity = duckdb_vector_get_validity(out);
  for (idx_t i = 0; i < n; ++i) {
    if (IsRowNull(a0, i)) {
      duckdb_validity_set_row_invalid(out_validity, i);
      continue;
    }
    std::string_view hex_str = ReadVarchar(a0, i);
    size_t first = hex_str.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string_view::npos) {
      duckdb_scalar_function_set_error(
          info, "UNHEX: input is empty or only whitespace");
      return;
    }
    size_t last = hex_str.find_last_not_of(" \t\n\r\f\v");
    hex_str = hex_str.substr(first, (last - first + 1));
    if (hex_str.length() >= 2 && hex_str[0] == '0' &&
        (hex_str[1] == 'x' || hex_str[1] == 'X')) {
      hex_str.remove_prefix(2);
    }
    if (hex_str.empty()) {
      duckdb_scalar_function_set_error(info,
                                       "UNHEX: hex string is empty after prefix");
      return;
    }
    std::optional<int64_t> result =
        base::StringViewToInt64(base::StringView(hex_str), 16);
    if (!result.has_value()) {
      duckdb_scalar_function_set_error(
          info, "UNHEX: invalid or out of range hex string");
      return;
    }
    out_data[i] = *result;
  }
}

// __intrinsic_hex_text(input VARCHAR) -> VARCHAR. Uppercase hex of the input's
// UTF-8 bytes. SQLite's `hex(X)` operates on X's TEXT/BLOB rendering (so
// `hex(123)` hexes the string "123" -> "313233", NOT the integer value), which
// DuckDB's native `hex(INTEGER)` does NOT do (it hexes the value). The surface
// `hex` is therefore a MACRO that casts its argument to VARCHAR first and calls
// this, reproducing SQLite's "hex of the text" semantics for every input type.
void HexTextTrampoline(duckdb_function_info, duckdb_data_chunk in,
                       duckdb_vector out) {
  static const char kHex[] = "0123456789ABCDEF";
  idx_t n = duckdb_data_chunk_get_size(in);
  duckdb_vector a0 = duckdb_data_chunk_get_vector(in, 0);
  duckdb_vector_ensure_validity_writable(out);
  uint64_t* out_validity = duckdb_vector_get_validity(out);
  std::string buf;
  for (idx_t i = 0; i < n; ++i) {
    if (IsRowNull(a0, i)) {
      duckdb_validity_set_row_invalid(out_validity, i);
      continue;
    }
    std::string_view s = ReadVarchar(a0, i);
    buf.clear();
    buf.reserve(s.size() * 2);
    for (char ch : s) {
      auto c = static_cast<unsigned char>(ch);
      buf.push_back(kHex[c >> 4]);
      buf.push_back(kHex[c & 0x0f]);
    }
    duckdb_vector_assign_string_element_len(out, i, buf.data(), buf.size());
  }
}

// Registers a math function under one name: the (DOUBLE)->DOUBLE numeric
// overload plus the (VARCHAR)->DOUBLE "text -> NULL" overload, matching the
// Perfetto intrinsic's behaviour on a text argument.
base::Status RegisterMath(duckdb_connection conn,
                          const char* name,
                          duckdb_scalar_function_t numeric,
                          std::unordered_set<std::string>* out) {
  base::Status s = RegisterScalarFunction(conn, name, {DUCKDB_TYPE_DOUBLE},
                                          DUCKDB_TYPE_DOUBLE, numeric, out);
  if (!s.ok()) {
    return s;
  }
  return RegisterScalarFunction(conn, name, {DUCKDB_TYPE_VARCHAR},
                                DUCKDB_TYPE_DOUBLE, MathVarcharNull, out);
}

}  // namespace

base::Status RegisterScalarFunctions(
    duckdb_connection conn,
    std::unordered_set<std::string>* out_registered) {
  // Math: register under BOTH the public surface name (what a user query carries
  // into DuckDB) and the underlying __intrinsic_* name (proving the mechanism
  // generalizes to genuinely intrinsic-only functions).
  struct MathReg {
    const char* surface;
    const char* intrinsic;
    duckdb_scalar_function_t fn;
  };
  const MathReg kMath[] = {
      {"ln", "__intrinsic_ln", &MathTrampoline<&LnImpl>},
      {"exp", "__intrinsic_exp", &MathTrampoline<&ExpImpl>},
      {"sqrt", "__intrinsic_sqrt", &MathTrampoline<&SqrtImpl>},
  };
  for (const MathReg& m : kMath) {
    RETURN_IF_ERROR(RegisterMath(conn, m.surface, m.fn, out_registered));
    RETURN_IF_ERROR(RegisterMath(conn, m.intrinsic, m.fn, out_registered));
  }

  // regexp_extract(VARCHAR, VARCHAR) -> VARCHAR.
  RETURN_IF_ERROR(RegisterScalarFunction(
      conn, "regexp_extract", {DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_VARCHAR},
      DUCKDB_TYPE_VARCHAR, RegexpExtractTrampoline, out_registered));
  RETURN_IF_ERROR(RegisterScalarFunction(
      conn, "__intrinsic_regexp_extract",
      {DUCKDB_TYPE_VARCHAR, DUCKDB_TYPE_VARCHAR}, DUCKDB_TYPE_VARCHAR,
      RegexpExtractTrampoline, out_registered));

  // unhex(VARCHAR) -> BIGINT. Unlike ln/exp/sqrt/regexp_extract (whose return
  // type matches DuckDB's native overload, so ALTER_ON_CONFLICT cleanly
  // OVERWRITES the same-signature builtin), DuckDB's native `unhex(VARCHAR)`
  // returns BLOB. ScalarFunction::Equal compares the return type, so a
  // BIGINT-returning overload is ADDED beside the BLOB one rather than replacing
  // it, leaving `unhex(VARCHAR)` ambiguous. To match Perfetto's semantics we
  // register the real implementation under the conflict-free `__intrinsic_unhex`
  // name and shadow the surface `unhex` with a DuckDB MACRO that delegates to
  // it (a user macro in the main catalog binds before the system builtin) -
  // mirroring PerfettoSQL's own `unhex DELEGATES TO __intrinsic_unhex`.
  RETURN_IF_ERROR(RegisterScalarFunction(
      conn, "__intrinsic_unhex", {DUCKDB_TYPE_VARCHAR}, DUCKDB_TYPE_BIGINT,
      UnhexTrampoline, out_registered));
  {
    duckdb_result res;
    duckdb_state st = duckdb_query(
        conn,
        "CREATE OR REPLACE MACRO unhex(s) AS __intrinsic_unhex(s);", &res);
    std::string err = st == DuckDBError ? duckdb_result_error(&res) : "";
    duckdb_destroy_result(&res);
    if (st == DuckDBError) {
      return base::ErrStatus(
          "RegisterScalarFunctions: failed to create unhex macro: %s",
          err.c_str());
    }
    out_registered->insert("unhex");
  }

  // iif(cond, a, b): SQLite's ternary. DuckDB has no `iif` builtin (it spells it
  // `CASE WHEN`), so define a MACRO with the exact CASE expansion. This binds
  // with identical semantics to SQLite (including NULL-condition -> else branch).
  {
    duckdb_result res;
    duckdb_state st = duckdb_query(
        conn,
        "CREATE OR REPLACE MACRO iif(c, a, b) AS CASE WHEN c THEN a ELSE b END;",
        &res);
    std::string err = st == DuckDBError ? duckdb_result_error(&res) : "";
    duckdb_destroy_result(&res);
    if (st == DuckDBError) {
      return base::ErrStatus(
          "RegisterScalarFunctions: failed to create iif macro: %s",
          err.c_str());
    }
    out_registered->insert("iif");
  }

  // hex(x): SQLite hexes the TEXT rendering of x (uppercase). Register the byte
  // implementation under a conflict-free name and shadow the surface `hex` with
  // a MACRO that casts to VARCHAR first (so `hex(123)` -> hex('123') ->
  // "313233", matching SQLite), mirroring the unhex pattern.
  RETURN_IF_ERROR(RegisterScalarFunction(
      conn, "__intrinsic_hex_text", {DUCKDB_TYPE_VARCHAR}, DUCKDB_TYPE_VARCHAR,
      HexTextTrampoline, out_registered));
  {
    duckdb_result res;
    duckdb_state st = duckdb_query(
        conn,
        "CREATE OR REPLACE MACRO hex(x) AS "
        "__intrinsic_hex_text(CAST(x AS VARCHAR));",
        &res);
    std::string err = st == DuckDBError ? duckdb_result_error(&res) : "";
    duckdb_destroy_result(&res);
    if (st == DuckDBError) {
      return base::ErrStatus(
          "RegisterScalarFunctions: failed to create hex macro: %s",
          err.c_str());
    }
    out_registered->insert("hex");
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::duckdb_integration
