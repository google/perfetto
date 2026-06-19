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

// EXPERIMENTAL (M2 size gate only). This file exists solely to force the DuckDB
// amalgamation to be linked into the trace_processor Wasm bundle so we can
// measure the binary-size delta. It is NOT a real integration and must be
// removed before any production use.
//
// It opens an in-memory DuckDB, runs a trivial query, and returns the result so
// the optimizer/linker cannot dead-code-strip DuckDB out of the bundle. The
// symbol is referenced from the Wasm bridge (see wasm_bridge.cc) under the same
// flag.

#include "src/trace_processor/duckdb_wasm_force.h"

#include "duckdb.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

namespace perfetto {
namespace trace_processor {

int64_t DuckDbForceLinkSmoke() {
  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);
  auto result = con.Query("SELECT 42");
  if (result->HasError()) {
    return -1;
  }
  return result->GetValue(0, 0).GetValue<int64_t>();
}

}  // namespace trace_processor
}  // namespace perfetto

#if defined(__EMSCRIPTEN__)
// Exported (KEEPALIVE) so the linker treats DuckDbForceLinkSmoke (and therefore
// the whole DuckDB amalgamation) as reachable and does not dead-code-strip it.
// This is the M2 size-gate hook only.
extern "C" int64_t EMSCRIPTEN_KEEPALIVE duckdb_force_link_smoke() {
  return perfetto::trace_processor::DuckDbForceLinkSmoke();
}
#endif
