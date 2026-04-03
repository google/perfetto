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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_TOKENIZER_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace perfetto::trace_processor::pfgraph {

enum class TokenType {
  kIdent,       // Identifier or keyword.
  kString,      // String literal ('...' or '''...''').
  kInt,         // Integer literal.
  kFloat,       // Floating point literal.
  kDot,         // .
  kColon,       // :
  kComma,       // ,
  kLParen,      // (
  kRParen,      // )
  kLBracket,    // [
  kRBracket,    // ]
  kLBrace,      // {
  kRBrace,      // }
  kAt,          // @
  kEquals,      // =
  kNotEquals,   // !=
  kLess,        // <
  kGreater,     // >
  kLessEq,      // <=
  kGreaterEq,   // >=
  kStar,        // *
  kPlus,        // +
  kMinus,       // -
  kSlash,       // /
  kPercent,     // %
  kPipe,        // ||
  kError,       // Tokenization error.
  kEof,         // End of input.
};

struct Token {
  TokenType type = TokenType::kEof;
  std::string_view text;
  uint32_t line = 0;
  uint32_t col = 0;
};

// Tokenizer for the PfGraph DSL. Handles comments (#), string literals,
// numbers, identifiers, and punctuation.
class PfGraphTokenizer {
 public:
  explicit PfGraphTokenizer(std::string_view input);

  // Returns the next token, advancing the position.
  Token Next();

  // Returns the next token without advancing.
  Token Peek();

  // Scans raw text until a matching closing brace, handling string literals
  // and nested braces. Returns the text between the braces (excluding them).
  // The opening brace must have already been consumed.
  std::string ScanUntilMatchingBrace();

  // Returns the current line number (1-based).
  uint32_t line() const { return line_; }

  // Returns the current column number (1-based).
  uint32_t col() const { return col_; }

 private:
  void SkipWhitespaceAndComments();
  Token ScanString();
  Token ScanNumber();
  Token ScanIdent();
  Token MakeToken(TokenType type, size_t len);

  std::string_view input_;
  size_t pos_ = 0;
  uint32_t line_ = 1;
  uint32_t col_ = 1;
  bool has_peeked_ = false;
  Token peeked_;
};

}  // namespace perfetto::trace_processor::pfgraph

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_PFGRAPH_TOKENIZER_H_
