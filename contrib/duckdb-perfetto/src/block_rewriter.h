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

#ifndef CONTRIB_DUCKDB_PERFETTO_SRC_BLOCK_REWRITER_H_
#define CONTRIB_DUCKDB_PERFETTO_SRC_BLOCK_REWRITER_H_

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct SyntaqliteParser;

namespace perfetto::duckdb_ext {

// A token of the outer (DuckDB) query. Comments are not tokens.
struct OuterToken {
  enum class Kind { kWord, kOperator, kLiteral };
  size_t start;  // Byte offset into the tokenized string.
  Kind kind;     // kWord: identifiers and keywords (including quoted ones).
};

// Tokenizes DuckDB SQL. In the extension this is DuckDB's own tokenizer, so
// that DuckDB-only lexical forms ($$ strings, nested comments, E'' strings)
// are handled exactly as DuckDB handles them.
using OuterTokenizer =
    std::function<std::vector<OuterToken>(const std::string&)>;

struct RewriteResult {
  enum Status {
    kNoBlocks,  // Nothing to do: let DuckDB parse the query as-is.
    kRewritten,
    kError,
  };
  Status status = kNoBlocks;
  std::string query;  // Valid iff kRewritten.
  std::string error;  // Valid iff kError.
  size_t error_offset = 0;
};

// Finds embedded PerfettoSQL blocks in a DuckDB query and rewrites them into
// calls to the perfetto_query() table function:
//
//   PERFETTO(alias) ( <PerfettoSQL> )   ->  perfetto_query('alias', '<sql>')
//   PERFETTO ( <PerfettoSQL> )          ->  perfetto_query('', '<sql>')
//
// When a block appears where a statement or subquery is expected (rather than
// after FROM / JOIN / ','), the call is prefixed with FROM so that it forms a
// complete query, e.g. `PERFETTO(t) (INCLUDE PERFETTO MODULE foo)` on its own
// becomes `FROM perfetto_query('t', 'INCLUDE PERFETTO MODULE foo')`.
//
// Each dialect is lexed by its own engine: the outer query by |tokenize|
// (DuckDB), and the block body by syntaqlite's PerfettoSQL tokenizer, the
// same front end TraceProcessor uses. The body is then parsed with
// syntaqlite's PerfettoSQL parser, so syntax errors are reported against the
// original query text before anything runs.
class BlockRewriter {
 public:
  BlockRewriter();
  ~BlockRewriter();

  BlockRewriter(const BlockRewriter&) = delete;
  BlockRewriter& operator=(const BlockRewriter&) = delete;

  // Thread-safe.
  RewriteResult Rewrite(const std::string& query,
                        const OuterTokenizer& tokenize);

 private:
  SyntaqliteParser* AcquireParser();
  void ReleaseParser(SyntaqliteParser*);

  // Parsers not currently in use by a Rewrite() call. The pool grows to the
  // number of concurrent rewrites.
  std::mutex mu_;
  std::vector<SyntaqliteParser*> parsers_;
};

}  // namespace perfetto::duckdb_ext

#endif  // CONTRIB_DUCKDB_PERFETTO_SRC_BLOCK_REWRITER_H_
