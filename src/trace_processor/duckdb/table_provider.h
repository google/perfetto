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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_TABLE_PROVIDER_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_TABLE_PROVIDER_H_

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "duckdb.h"

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"

namespace perfetto::trace_processor::duckdb_integration {

// D1 (experimental DuckDB query engine): the data-driven generalization of the
// one-off `sched_df` table function. Registers ONE generic DuckDB C-API table
// function `__perfetto_df(VARCHAR)` that, given a table name, scans the
// corresponding `dataframe::Dataframe` and returns its rows. The schema of each
// table is derived at bind time from `Dataframe::CreateSpec()` rather than
// hard-coded, so the same function serves every storage / runtime dataframe.
//
// Usage from SQL once a table is registered (e.g. as "sched"):
//   SELECT count(*), sum(dur) FROM __perfetto_df('sched') WHERE ucpu = 3;
//
// CRUCIAL DIFFERENCE vs `sched_df`: cells are read through the dataframe
// `Cursor` (storage-order, plan-driven), NOT `Dataframe::GetCell` random
// access. `GetCell` FATALs on plain `SparseNull` columns and reads
// out-of-bounds on post-finalize `SparseNullWithPopcountUntilFinalization`
// columns (see `dataframe.h:357-359`); the cursor handles all nullability kinds
// correctly. This is what makes the generic provider safe for the whole table
// set (and especially runtime tables, which use plain `SparseNull`).
//
// Type mapping (D1 section 2.1, the parity decision): ALL integer storage kinds
// (Id, Uint32, Int32, Int64) map to DUCKDB_TYPE_BIGINT (matching SQLite's
// uniform 64-bit numeric output); Double -> DOUBLE; String -> VARCHAR. Nulls are
// expressed via DuckDB validity bitmaps.
//
// Lifetime: each registered table stores a `CopyFinalized()` snapshot (a shallow
// shared-ptr copy of the column buffers) so the dataframe DuckDB scans is stable
// for the scan's duration and outlives the query. The whole provider (and its
// snapshots) is owned by the table function's extra-info and freed at DB
// teardown.
//
// Threading: the scan pins `max_threads = 1` because the StringPool and the
// single cursor are not thread-safe.
//
// IMPORTANT: driven exclusively through DuckDB's C API so no C++ exception can
// unwind across the boundary into Perfetto's `-fno-exceptions` code.
//
// OUT OF SCOPE for this subtask (later work): the DuckDB replacement scan (so
// bare `FROM sched` syntax), runtime-table read-through, and any routing.
class DuckDbTableProvider {
 public:
  // `string_pool` must outlive the provider and every scan; it is used to
  // resolve `String` cells to text. It is the StringPool backing the registered
  // dataframes.
  explicit DuckDbTableProvider(StringPool* string_pool);
  ~DuckDbTableProvider();

  DuckDbTableProvider(const DuckDbTableProvider&) = delete;
  DuckDbTableProvider& operator=(const DuckDbTableProvider&) = delete;

  // Registers `df` under `name`, taking a `CopyFinalized()` snapshot internally.
  // `df` must be finalized. Returns an error if `name` is already registered or
  // the dataframe has no id column.
  base::Status Register(const std::string& name, const dataframe::Dataframe& df);

  // Registers the generic `__perfetto_df(VARCHAR)` table function on
  // `connection`, backed by this provider. The provider must outlive the
  // connection's database. Must be called exactly once.
  base::Status RegisterTableFunction(duckdb_connection connection);

  // Implementation detail, exposed only so the C-API callbacks (which are free
  // functions) can reach the registry. Resolves `name` to its entry, or nullptr.
  struct Entry {
    explicit Entry(dataframe::Dataframe d)
        : df(std::move(d)), spec(df.CreateSpec()) {}

    dataframe::Dataframe df;            // CopyFinalized() snapshot.
    dataframe::DataframeSpec spec;      // Cached CreateSpec().
    uint32_t id_col_idx = 0;            // Resolved id-column index.
  };
  const Entry* Find(const std::string& name) const;
  StringPool* string_pool() const { return string_pool_; }

 private:
  StringPool* string_pool_;
  std::map<std::string, std::unique_ptr<Entry>> entries_;
};

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_TABLE_PROVIDER_H_
