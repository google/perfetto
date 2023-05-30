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

#include "src/trace_processor/sqlite/perfetto_sql_parser.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/sqlite/sqlite_tokenizer.h"

namespace perfetto {
namespace trace_processor {
namespace {

using Token = SqliteTokenizer::Token;
using Statement = PerfettoSqlParser::Statement;

bool TokenIsTerminal(Token t) {
  return t.token_type == SqliteTokenType::TK_SEMI || t.str.empty();
}

}  // namespace

PerfettoSqlParser::PerfettoSqlParser(const char* sql)
    : tokenizer_(sql), start_(sql) {}

bool PerfettoSqlParser::Next() {
  PERFETTO_DCHECK(status_.ok());

  const char* non_space_ptr = nullptr;
  for (Token token = tokenizer_.Next();; token = tokenizer_.Next()) {
    // Space should always be completely ignored by any logic below as it will
    // never change the current state in the state machine.
    if (token.token_type == SqliteTokenType::TK_SPACE) {
      continue;
    }

    if (TokenIsTerminal(token)) {
      // If we have a non-space character we've seen, just return all the stuff
      // we've seen between that and the current token.
      if (non_space_ptr) {
        uint32_t offset_of_non_space =
            static_cast<uint32_t>(non_space_ptr - start_);
        uint32_t chars_since_non_space =
            static_cast<uint32_t>(tokenizer_.ptr() - non_space_ptr);
        statement_ = Statement(
            SqliteSql{std::string_view(non_space_ptr, chars_since_non_space),
                      offset_of_non_space});
        return true;
      }
      // This means we've seen a semi-colon without any non-space content. Just
      // try and find the next statement as this "statement" is a noop.
      if (token.token_type == SqliteTokenType::TK_SEMI) {
        continue;
      }
      // This means we've reached the end of the SQL.
      PERFETTO_DCHECK(token.str.empty());
      return false;
    }

    // If we've not seen a space character, keep track of the current position.
    if (!non_space_ptr) {
      non_space_ptr = token.str.data();
    }
  }
}

}  // namespace trace_processor
}  // namespace perfetto
