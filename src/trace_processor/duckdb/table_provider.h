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

#include <functional>
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
// The provider also registers a DuckDB replacement scan so that a BARE
// PerfettoSQL table name (e.g. `SELECT * FROM sched`, no `__perfetto_df(...)`,
// no parens) resolves to the generic function: when DuckDB fails to find a name
// in its catalog it fires the scan, which - on a registry hit - rewrites the
// reference to `__perfetto_df('<name>')`. On a miss the scan leaves the function
// name unset so DuckDB raises its normal catalog error (the miss path is what a
// future routing/fallback layer keys on); it never crashes.
//
// READ-THROUGH CACHE (D1 section 5.1): the registry is a cache in front of an
// optional resolver callback. When a name is not already in the local cache, the
// provider calls the resolver (if one was supplied) to obtain the live
// `dataframe::Dataframe*`, snapshots it lazily (`CopyFinalized()`), inserts it,
// and proceeds. This lets static AND runtime `CREATE PERFETTO TABLE` tables
// resolve by name with no per-table DuckDB registration - the engine's
// `GetDataframeOrNull` is the eventual resolver (NOT wired into
// `PerfettoSqlConnection` in this subtask; validated here with a test-provided
// resolver).
class DuckDbTableProvider {
 public:
  // Resolves a PerfettoSQL table name to a live finalized dataframe, or nullptr
  // if no such table exists. Used as the read-through backing store on a local
  // cache miss. The returned pointer need only be valid for the duration of the
  // call (the provider snapshots immediately via `CopyFinalized()`).
  using Resolver =
      std::function<const dataframe::Dataframe*(const std::string&)>;

  // `string_pool` must outlive the provider and every scan; it is used to
  // resolve `String` cells to text. It is the StringPool backing the registered
  // dataframes. `resolver` is optional (may be empty); if set it is consulted on
  // a local-cache miss to lazily snapshot a live dataframe by name.
  explicit DuckDbTableProvider(StringPool* string_pool,
                               Resolver resolver = {});
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

  // Registers the replacement scan that rewrites a bare `FROM <name>` into
  // `__perfetto_df('<name>')` when `<name>` resolves in this provider. The
  // provider must outlive `db`. Must be called exactly once. Note: replacement
  // scans are registered on the DATABASE, not the connection.
  base::Status RegisterReplacementScan(duckdb_database db);

  // Implementation detail, exposed only so the C-API callbacks (which are free
  // functions) can reach the registry. Resolves `name` to its entry, or nullptr.
  struct Entry {
    explicit Entry(dataframe::Dataframe d)
        : df(std::move(d)), spec(df.CreateSpec()) {}

    dataframe::Dataframe df;            // CopyFinalized() snapshot.
    dataframe::DataframeSpec spec;      // Cached CreateSpec().
    uint32_t id_col_idx = 0;            // Resolved id-column index.

    // Staleness key for resolver-backed (read-through) entries. Records the
    // identity (address) and mutation count of the LIVE dataframe this snapshot
    // was taken from. Every mutating DDL (CREATE OR REPLACE / DROP+CREATE /
    // CREATE INDEX) swaps the live dataframe to a NEW object (verified), so a
    // changed address means the snapshot is stale and must be retaken; the
    // mutation count is a defensive secondary check. Unset (nullptr) for eager
    // `Register`ed entries, which are never re-resolved.
    const dataframe::Dataframe* source = nullptr;
    uint64_t source_mutations = 0;
  };

  // Resolves `name`: returns the cached entry if present; otherwise consults the
  // read-through resolver (if any), lazily snapshotting + caching a hit. Returns
  // nullptr if the name is unknown to both the cache and the resolver. This is
  // the single resolution path used by BOTH the replacement scan and the table
  // function's bind, so they always agree.
  const Entry* Resolve(const std::string& name);

  // Like `Resolve` but read-only: only checks the local cache, never the
  // resolver. Retained for callers that must not mutate the cache.
  const Entry* Find(const std::string& name) const;

  StringPool* string_pool() const { return string_pool_; }

 private:
  // Inserts (or replaces) a snapshot of `df` under `name`, recording `df` as the
  // staleness source. `df` must be finalized and have an id column. Returns
  // nullptr on failure (no id column).
  const Entry* InsertSnapshot(const std::string& name,
                              const dataframe::Dataframe& df);

  StringPool* string_pool_;
  Resolver resolver_;
  std::map<std::string, std::unique_ptr<Entry>> entries_;
};

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_TABLE_PROVIDER_H_
