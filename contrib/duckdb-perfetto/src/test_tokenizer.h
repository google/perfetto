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

#ifndef CONTRIB_DUCKDB_PERFETTO_SRC_TEST_TOKENIZER_H_
#define CONTRIB_DUCKDB_PERFETTO_SRC_TEST_TOKENIZER_H_

#include <cctype>
#include <string>
#include <vector>

#include "contrib/duckdb-perfetto/src/block_rewriter.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

namespace perfetto::duckdb_ext {

// The extension tokenizes the outer query with DuckDB's own tokenizer, which
// tests can't link against. For queries without DuckDB-only lexical forms,
// syntaqlite's tokenizer produces the same tokens.
inline std::vector<OuterToken> TestTokenize(const std::string& s) {
  std::vector<OuterToken> toks;
  SyntaqliteTokenizer* tok = syntaqlite_tokenizer_create_perfetto(nullptr);
  syntaqlite_tokenizer_reset(tok, s.data(),
                             static_cast<SyntaqliteLength>(s.size()));
  SyntaqliteToken t;
  while (syntaqlite_tokenizer_next(tok, &t)) {
    if (t.type == SYNTAQLITE_TK_SPACE || t.type == SYNTAQLITE_TK_COMMENT)
      continue;
    OuterToken::Kind kind = OuterToken::Kind::kOperator;
    char c = t.text[0];
    if (t.type == SYNTAQLITE_TK_STRING ||
        isdigit(static_cast<unsigned char>(c)))
      kind = OuterToken::Kind::kLiteral;
    else if (isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '"')
      kind = OuterToken::Kind::kWord;
    toks.push_back({static_cast<size_t>(t.text - s.data()), kind});
  }
  syntaqlite_tokenizer_destroy(tok);
  return toks;
}

}  // namespace perfetto::duckdb_ext

#endif  // CONTRIB_DUCKDB_PERFETTO_SRC_TEST_TOKENIZER_H_
