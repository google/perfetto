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

#ifndef SRC_PERFETTO_SQL_SYNTAQLITE_PIPE_TOKEN_H_
#define SRC_PERFETTO_SQL_SYNTAQLITE_PIPE_TOKEN_H_

// The tokenizer the Perfetto dialect uses: SQLite's, from the generated
// parser, with `|>` as one token, PIPE. SQLite's tokenizer alone yields `|`
// then `>`, and since an expression can end in `|`, the grammar could not
// tell a pipe after `WHERE a` from a bitwise-or. The generated tokenizer is
// not open to a dialect adding a token, so both places which call it go
// through here: the trace processor's parser through inline_dispatch.h, and
// the formatter's dialect library through the template pipe_dialect.c
// exports.
//
// The generated tokenizer is declared here because the generated header does
// not declare it.

#include <stdint.h>

#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

int64_t SynqPerfettoGetToken(const SyntaqliteDialect* env,
                             const unsigned char* z,
                             int* token_type);

static inline int64_t SynqPerfettoGetTokenWithPipe(const SyntaqliteDialect* env,
                                                   const unsigned char* z,
                                                   int* token_type) {
  if (z[0] == '|' && z[1] == '>') {
    *token_type = SYNTAQLITE_TK_PIPE;
    return 2;
  }
  return SynqPerfettoGetToken(env, z, token_type);
}

#endif  // SRC_PERFETTO_SQL_SYNTAQLITE_PIPE_TOKEN_H_
