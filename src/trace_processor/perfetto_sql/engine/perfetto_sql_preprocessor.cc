/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_preprocessor.h"
#include <unordered_set>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "src/trace_processor/sqlite/sqlite_tokenizer.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

PerfettoSqlPreprocessor::PerfettoSqlPreprocessor(SqlSource source)
    : global_tokenizer_(std::move(source)) {}

bool PerfettoSqlPreprocessor::NextStatement() {
  PERFETTO_CHECK(status_.ok());

  // Skip through any number of semi-colons (representing empty statements).
  SqliteTokenizer::Token tok = global_tokenizer_.NextNonWhitespace();
  while (tok.token_type == SqliteTokenType::TK_SEMI) {
    tok = global_tokenizer_.NextNonWhitespace();
  }

  // If we still see a terminal token at this point, we must have hit EOF.
  if (tok.IsTerminal()) {
    PERFETTO_DCHECK(tok.token_type != SqliteTokenType::TK_SEMI);
    return false;
  }

  SqlSource stmt =
      global_tokenizer_.Substr(tok, global_tokenizer_.NextTerminal());
  statement_ = std::move(stmt);
  return true;
}

base::Status PerfettoSqlPreprocessor::ErrorAtToken(
    const SqliteTokenizer& tokenizer,
    const SqliteTokenizer::Token& token,
    const char* error) {
  std::string traceback = tokenizer.AsTraceback(token);
  return base::ErrStatus("%s%s", traceback.c_str(), error);
}

}  // namespace trace_processor
}  // namespace perfetto
