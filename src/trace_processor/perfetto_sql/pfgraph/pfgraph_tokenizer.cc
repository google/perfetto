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

#include "src/trace_processor/perfetto_sql/pfgraph/pfgraph_tokenizer.h"

#include <cctype>

namespace perfetto::trace_processor::pfgraph {

PfGraphTokenizer::PfGraphTokenizer(std::string_view input) : input_(input) {}

Token PfGraphTokenizer::MakeToken(TokenType type, size_t len) {
  Token tok;
  tok.type = type;
  tok.text = input_.substr(pos_, len);
  tok.line = line_;
  tok.col = col_;
  for (size_t i = 0; i < len; ++i) {
    if (input_[pos_ + i] == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
  }
  pos_ += len;
  return tok;
}

void PfGraphTokenizer::SkipWhitespaceAndComments() {
  while (pos_ < input_.size()) {
    char c = input_[pos_];
    // Skip whitespace (spaces, tabs, newlines).
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (c == '\n') {
        ++line_;
        col_ = 1;
      } else {
        ++col_;
      }
      ++pos_;
      continue;
    }
    // Skip comments (# to end of line, or -- to end of line).
    if (c == '#' || (c == '-' && pos_ + 1 < input_.size() &&
                     input_[pos_ + 1] == '-')) {
      while (pos_ < input_.size() && input_[pos_] != '\n') {
        ++pos_;
        ++col_;
      }
      continue;
    }
    break;
  }
}

Token PfGraphTokenizer::ScanString() {
  uint32_t start_line = line_;
  uint32_t start_col = col_;
  size_t start = pos_;

  // Check for triple-quoted string.
  bool triple = (pos_ + 2 < input_.size() && input_[pos_ + 1] == '\'' &&
                 input_[pos_ + 2] == '\'');
  if (triple) {
    pos_ += 3;
    col_ += 3;
    while (pos_ + 2 < input_.size()) {
      if (input_[pos_] == '\'' && input_[pos_ + 1] == '\'' &&
          input_[pos_ + 2] == '\'') {
        pos_ += 3;
        col_ += 3;
        Token tok;
        tok.type = TokenType::kString;
        tok.text = input_.substr(start, pos_ - start);
        tok.line = start_line;
        tok.col = start_col;
        return tok;
      }
      if (input_[pos_] == '\n') {
        ++line_;
        col_ = 1;
      } else {
        ++col_;
      }
      ++pos_;
    }
    // Unterminated triple-quoted string.
    Token tok;
    tok.type = TokenType::kError;
    tok.text = input_.substr(start, pos_ - start);
    tok.line = start_line;
    tok.col = start_col;
    return tok;
  }

  // Single-quoted string.
  ++pos_;
  ++col_;
  while (pos_ < input_.size()) {
    char c = input_[pos_];
    if (c == '\'') {
      // Check for escaped quote ('').
      if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '\'') {
        pos_ += 2;
        col_ += 2;
        continue;
      }
      ++pos_;
      ++col_;
      Token tok;
      tok.type = TokenType::kString;
      tok.text = input_.substr(start, pos_ - start);
      tok.line = start_line;
      tok.col = start_col;
      return tok;
    }
    if (c == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    ++pos_;
  }
  // Unterminated string.
  Token tok;
  tok.type = TokenType::kError;
  tok.text = input_.substr(start, pos_ - start);
  tok.line = start_line;
  tok.col = start_col;
  return tok;
}

Token PfGraphTokenizer::ScanNumber() {
  size_t start = pos_;
  uint32_t start_line = line_;
  uint32_t start_col = col_;
  bool is_float = false;

  // Optional leading minus is handled by the caller as a separate token.
  while (pos_ < input_.size() && std::isdigit(input_[pos_])) {
    ++pos_;
    ++col_;
  }
  if (pos_ < input_.size() && input_[pos_] == '.') {
    // Check it's not a method call (e.g., "10.filter" should be "10" + ".").
    if (pos_ + 1 < input_.size() && std::isdigit(input_[pos_ + 1])) {
      is_float = true;
      ++pos_;
      ++col_;
      while (pos_ < input_.size() && std::isdigit(input_[pos_])) {
        ++pos_;
        ++col_;
      }
    }
  }
  // Scientific notation: 1e6, 1.5e-3, etc.
  if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
    is_float = true;
    ++pos_;
    ++col_;
    if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
      ++pos_;
      ++col_;
    }
    while (pos_ < input_.size() && std::isdigit(input_[pos_])) {
      ++pos_;
      ++col_;
    }
  }

  Token tok;
  tok.type = is_float ? TokenType::kFloat : TokenType::kInt;
  tok.text = input_.substr(start, pos_ - start);
  tok.line = start_line;
  tok.col = start_col;
  return tok;
}

Token PfGraphTokenizer::ScanIdent() {
  size_t start = pos_;
  uint32_t start_line = line_;
  uint32_t start_col = col_;

  // Allow $ as leading character for template parameter references.
  if (pos_ < input_.size() && input_[pos_] == '$') {
    ++pos_;
    ++col_;
  }
  while (pos_ < input_.size() &&
         (std::isalnum(input_[pos_]) || input_[pos_] == '_')) {
    ++pos_;
    ++col_;
  }

  Token tok;
  tok.type = TokenType::kIdent;
  tok.text = input_.substr(start, pos_ - start);
  tok.line = start_line;
  tok.col = start_col;
  return tok;
}

Token PfGraphTokenizer::Next() {
  if (has_peeked_) {
    has_peeked_ = false;
    return peeked_;
  }

  SkipWhitespaceAndComments();

  if (pos_ >= input_.size()) {
    Token tok;
    tok.type = TokenType::kEof;
    tok.line = line_;
    tok.col = col_;
    return tok;
  }

  char c = input_[pos_];

  // String literal.
  if (c == '\'') {
    return ScanString();
  }

  // Number.
  if (std::isdigit(c)) {
    return ScanNumber();
  }

  // Identifier or keyword (including $param references in templates).
  if (std::isalpha(c) || c == '_' ||
      (c == '$' && pos_ + 1 < input_.size() &&
       (std::isalpha(input_[pos_ + 1]) || input_[pos_ + 1] == '_'))) {
    return ScanIdent();
  }

  // Two-character operators.
  if (pos_ + 1 < input_.size()) {
    char c2 = input_[pos_ + 1];
    if (c == '!' && c2 == '=')
      return MakeToken(TokenType::kNotEquals, 2);
    if (c == '<' && c2 == '=')
      return MakeToken(TokenType::kLessEq, 2);
    if (c == '>' && c2 == '=')
      return MakeToken(TokenType::kGreaterEq, 2);
    if (c == '|' && c2 == '|')
      return MakeToken(TokenType::kPipe, 2);
  }

  // Single-character tokens.
  switch (c) {
    case '.':
      return MakeToken(TokenType::kDot, 1);
    case ':':
      return MakeToken(TokenType::kColon, 1);
    case ',':
      return MakeToken(TokenType::kComma, 1);
    case '(':
      return MakeToken(TokenType::kLParen, 1);
    case ')':
      return MakeToken(TokenType::kRParen, 1);
    case '[':
      return MakeToken(TokenType::kLBracket, 1);
    case ']':
      return MakeToken(TokenType::kRBracket, 1);
    case '{':
      return MakeToken(TokenType::kLBrace, 1);
    case '}':
      return MakeToken(TokenType::kRBrace, 1);
    case '@':
      return MakeToken(TokenType::kAt, 1);
    case '=':
      return MakeToken(TokenType::kEquals, 1);
    case '<':
      return MakeToken(TokenType::kLess, 1);
    case '>':
      return MakeToken(TokenType::kGreater, 1);
    case '*':
      return MakeToken(TokenType::kStar, 1);
    case '+':
      return MakeToken(TokenType::kPlus, 1);
    case '-':
      return MakeToken(TokenType::kMinus, 1);
    case '/':
      return MakeToken(TokenType::kSlash, 1);
    case '%':
      return MakeToken(TokenType::kPercent, 1);
    default:
      break;
  }

  // Unknown character.
  return MakeToken(TokenType::kError, 1);
}

Token PfGraphTokenizer::Peek() {
  if (!has_peeked_) {
    peeked_ = Next();
    has_peeked_ = true;
  }
  return peeked_;
}

std::string PfGraphTokenizer::ScanUntilMatchingBrace() {
  // Invalidate any peeked token since we're doing raw scanning.
  has_peeked_ = false;

  SkipWhitespaceAndComments();

  size_t start = pos_;
  int depth = 1;
  while (pos_ < input_.size() && depth > 0) {
    char c = input_[pos_];
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0)
        break;
    } else if (c == '\'') {
      // Skip string literal.
      ++pos_;
      ++col_;
      while (pos_ < input_.size() && input_[pos_] != '\'') {
        if (input_[pos_] == '\n') {
          ++line_;
          col_ = 1;
        } else {
          ++col_;
        }
        ++pos_;
      }
      // pos_ now points to closing quote, will be advanced below.
    } else if (c == '-' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '-') {
      // Skip SQL -- comments.
      while (pos_ < input_.size() && input_[pos_] != '\n') {
        ++pos_;
        ++col_;
      }
      continue;
    }

    if (c == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    ++pos_;
  }

  // Trim trailing whitespace from the captured SQL.
  size_t end = pos_;
  while (end > start && (input_[end - 1] == ' ' || input_[end - 1] == '\n' ||
                         input_[end - 1] == '\r' || input_[end - 1] == '\t')) {
    --end;
  }

  // Skip past the closing brace.
  if (pos_ < input_.size() && input_[pos_] == '}') {
    ++pos_;
    ++col_;
  }

  return std::string(input_.substr(start, end - start));
}

}  // namespace perfetto::trace_processor::pfgraph
