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

#ifndef SRC_TRACE_PROCESSOR_SQLITE_SQLITE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_SQLITE_SQLITE_TOKENIZER_H_

#include <optional>
#include <string_view>

namespace perfetto {
namespace trace_processor {

// List of token types returnable by |SqliteTokenizer|
// 1:1 matches the defintions in SQLite.
enum class SqliteTokenType : uint32_t {
  TK_SEMI = 1,
  TK_LP = 22,
  TK_RP = 23,
  TK_COMMA = 25,
  TK_NE = 52,
  TK_EQ = 53,
  TK_GT = 54,
  TK_LE = 55,
  TK_LT = 56,
  TK_GE = 57,
  TK_ID = 59,
  TK_BITAND = 102,
  TK_BITOR = 103,
  TK_LSHIFT = 104,
  TK_RSHIFT = 105,
  TK_PLUS = 106,
  TK_MINUS = 107,
  TK_STAR = 108,
  TK_SLASH = 109,
  TK_REM = 110,
  TK_CONCAT = 111,
  TK_PTR = 112,
  TK_BITNOT = 114,
  TK_STRING = 117,
  TK_DOT = 141,
  TK_FLOAT = 153,
  TK_BLOB = 154,
  TK_INTEGER = 155,
  TK_VARIABLE = 156,
  TK_SPACE = 183,
  TK_ILLEGAL = 184,

  // Generic constant which replaces all the keywords in SQLite as we do not
  // care about the distinguishing between the vast majority of them.
  TK_GENERIC_KEYWORD = 1000,
};

// Tokenizes SQL statements according to SQLite SQL language specification:
// https://www2.sqlite.org/hlr40000.html
//
// Usage of this class:
// SqliteTokenizer tzr;
// tzr.Reset(my_sql_string.c_str());
// for (auto t = tzr.Next(); t.token_type != TK_SEMI; t = tzr.Next()) {
//   // Handle t here
// }
class SqliteTokenizer {
 public:
  // A single SQL token according to the SQLite standard.
  struct Token {
    // The string contents of the token.
    std::string_view str;

    // The type of the token.
    SqliteTokenType token_type;

    bool operator==(const Token& o) const {
      return str == o.str && token_type == o.token_type;
    }
  };

  explicit SqliteTokenizer(const char* sql);

  // Returns the next SQL token.
  Token Next();

  // Returns the next SQL token which is not of type TK_SPACE.
  Token NextNonWhitespace();

  // Returns the pointer to the start of the next token which will be returned.
  const char* ptr() const { return ptr_; }

 private:
  const unsigned char* unsigned_ptr() const {
    return reinterpret_cast<const unsigned char*>(ptr_);
  }

  const char* ptr_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SQLITE_SQLITE_TOKENIZER_H_
