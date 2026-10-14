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

#ifndef CONTRIB_DUCKDB_PERFETTO_SRC_CHAIN_REWRITER_H_
#define CONTRIB_DUCKDB_PERFETTO_SRC_CHAIN_REWRITER_H_

#include <string>

#include "contrib/duckdb-perfetto/src/block_rewriter.h"

namespace perfetto::duckdb_ext {

// Rewrites `|>` pipelines in a DuckDB query into nested DuckDB subqueries:
//
//   FROM <source> [AS a] |> WHERE p |> SELECT x
//   ->  SELECT x FROM (SELECT * FROM <source> AS a WHERE p) AS a
//
// <source> is a table or view (possibly a table function call, which is
// what PERFETTO blocks become) or a parenthesized subquery. The pipeline
// must be at the start of a query: a statement, a parenthesized subquery,
// after AS (CTE bodies, CREATE TABLE ... AS) or after EXPLAIN [ANALYZE].
// Stages are WHERE, SELECT, EXTEND, SET, DROP, RENAME, AGGREGATE [GROUP BY],
// [LEFT | ...] JOIN, ORDER BY, LIMIT and AS; their bodies are DuckDB SQL and
// are copied verbatim.
//
// |query| must not contain PERFETTO blocks: see RewriteQuery().
RewriteResult RewriteChains(const std::string& query,
                            const OuterTokenizer& tokenize);

// Rewrites PERFETTO blocks with |blocks|, then pipelines with
// RewriteChains(). Error offsets point into |query|.
RewriteResult RewriteQuery(BlockRewriter* blocks,
                           const std::string& query,
                           const OuterTokenizer& tokenize);

}  // namespace perfetto::duckdb_ext

#endif  // CONTRIB_DUCKDB_PERFETTO_SRC_CHAIN_REWRITER_H_
