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

#include "src/trace_processor/sqlite/sqlite_tokenizer.h"

#include <ctype.h>
#include <sqlite3.h>
#include <cstdint>
#include <optional>
#include <string_view>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {

// The contents of this file are ~copied from SQLite with some modifications to
// minimize the amount copied: i.e. if we can call a libc function/public SQLite
// API instead of a private one.
//
// The changes are as follows:
// 1. Remove all ifdefs to only keep branches we actually use
// 2. Change handling of |CC_KYWD0| to remove distinction between different
//    SQLite kewords, reducing how many things we need to copy over.
// 3. Constants are changed from be macro defines to be values in
//    |SqliteTokenType|.

namespace {

const unsigned char sqlite3CtypeMap[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 00..07    ........ */
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, /* 08..0f    ........ */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 10..17    ........ */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 18..1f    ........ */
    0x01, 0x00, 0x80, 0x00, 0x40, 0x00, 0x00, 0x80, /* 20..27     !"#$%&' */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 28..2f    ()*+,-./ */
    0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, /* 30..37    01234567 */
    0x0c, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 38..3f    89:;<=>? */

    0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x02, /* 40..47    @ABCDEFG */
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, /* 48..4f    HIJKLMNO */
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, /* 50..57    PQRSTUVW */
    0x02, 0x02, 0x02, 0x80, 0x00, 0x00, 0x00, 0x40, /* 58..5f    XYZ[\]^_ */
    0x80, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x22, /* 60..67    `abcdefg */
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, /* 68..6f    hijklmno */
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, /* 70..77    pqrstuvw */
    0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, /* 78..7f    xyz{|}~. */

    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 80..87    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 88..8f    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 90..97    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 98..9f    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* a0..a7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* a8..af    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* b0..b7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* b8..bf    ........ */

    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* c0..c7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* c8..cf    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* d0..d7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* d8..df    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* e0..e7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* e8..ef    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* f0..f7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40  /* f8..ff    ........ */
};

#define CC_X 0        /* The letter 'x', or start of BLOB literal */
#define CC_KYWD0 1    /* First letter of a keyword */
#define CC_KYWD 2     /* Alphabetics or '_'.  Usable in a keyword */
#define CC_DIGIT 3    /* Digits */
#define CC_DOLLAR 4   /* '$' */
#define CC_VARALPHA 5 /* '@', '#', ':'.  Alphabetic SQL variables */
#define CC_VARNUM 6   /* '?'.  Numeric SQL variables */
#define CC_SPACE 7    /* Space characters */
#define CC_QUOTE 8    /* '"', '\'', or '`'.  String literals, quoted ids */
#define CC_QUOTE2 9   /* '['.   [...] style quoted ids */
#define CC_PIPE 10    /* '|'.   Bitwise OR or concatenate */
#define CC_MINUS 11   /* '-'.  Minus or SQL-style comment */
#define CC_LT 12      /* '<'.  Part of < or <= or <> */
#define CC_GT 13      /* '>'.  Part of > or >= */
#define CC_EQ 14      /* '='.  Part of = or == */
#define CC_BANG 15    /* '!'.  Part of != */
#define CC_SLASH 16   /* '/'.  / or c-style comment */
#define CC_LP 17      /* '(' */
#define CC_RP 18      /* ')' */
#define CC_SEMI 19    /* ';' */
#define CC_PLUS 20    /* '+' */
#define CC_STAR 21    /* '*' */
#define CC_PERCENT 22 /* '%' */
#define CC_COMMA 23   /* ',' */
#define CC_AND 24     /* '&' */
#define CC_TILDA 25   /* '~' */
#define CC_DOT 26     /* '.' */
#define CC_ID 27      /* unicode characters usable in IDs */
#define CC_NUL 29     /* 0x00 */
#define CC_BOM 30     /* First byte of UTF8 BOM:  0xEF 0xBB 0xBF */

// clang-format off
static const unsigned char aiClass[] = {
/*         x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xa  xb  xc  xd  xe  xf */
/* 0x */   29, 28, 28, 28, 28, 28, 28, 28, 28,  7,  7, 28,  7,  7, 28, 28,
/* 1x */   28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 2x */    7, 15,  8,  5,  4, 22, 24,  8, 17, 18, 21, 20, 23, 11, 26, 16,
/* 3x */    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  5, 19, 12, 14, 13,  6,
/* 4x */    5,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
/* 5x */    1,  1,  1,  1,  1,  1,  1,  1,  0,  2,  2,  9, 28, 28, 28,  2,
/* 6x */    8,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
/* 7x */    1,  1,  1,  1,  1,  1,  1,  1,  0,  2,  2, 28, 10, 28, 25, 28,
/* 8x */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* 9x */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Ax */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Bx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Cx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Dx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Ex */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 30,
/* Fx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27
};
// clang-format on

#define IdChar(C) ((sqlite3CtypeMap[static_cast<unsigned char>(C)] & 0x46) != 0)

// Copy of |sqlite3GetToken| for use by the PerfettoSql transpiler.
//
// We copy this function because |sqlite3GetToken| is static to sqlite3.c
// in most distributions of SQLite so we cannot call it from our code.
//
// While we could redefine SQLITE_PRIVATE, pragmatically that will not fly in
// all the places we build trace processor so we need to resort to making a
// copy.
int GetSqliteToken(const unsigned char* z, SqliteTokenType* tokenType) {
  int i, c;
  switch (aiClass[*z]) { /* Switch on the character-class of the first byte
                         ** of the token. See the comment on the CC_ defines
                         ** above. */
    case CC_SPACE: {
      for (i = 1; isspace(z[i]); i++) {
      }
      *tokenType = SqliteTokenType::TK_SPACE;
      return i;
    }
    case CC_MINUS: {
      if (z[1] == '-') {
        for (i = 2; (c = z[i]) != 0 && c != '\n'; i++) {
        }
        *tokenType = SqliteTokenType::TK_SPACE; /* IMP: R-22934-25134 */
        return i;
      } else if (z[1] == '>') {
        *tokenType = SqliteTokenType::TK_PTR;
        return 2 + (z[2] == '>');
      }
      *tokenType = SqliteTokenType::TK_MINUS;
      return 1;
    }
    case CC_LP: {
      *tokenType = SqliteTokenType::TK_LP;
      return 1;
    }
    case CC_RP: {
      *tokenType = SqliteTokenType::TK_RP;
      return 1;
    }
    case CC_SEMI: {
      *tokenType = SqliteTokenType::TK_SEMI;
      return 1;
    }
    case CC_PLUS: {
      *tokenType = SqliteTokenType::TK_PLUS;
      return 1;
    }
    case CC_STAR: {
      *tokenType = SqliteTokenType::TK_STAR;
      return 1;
    }
    case CC_SLASH: {
      if (z[1] != '*' || z[2] == 0) {
        *tokenType = SqliteTokenType::TK_SLASH;
        return 1;
      }
      for (i = 3, c = z[2]; (c != '*' || z[i] != '/') && (c = z[i]) != 0; i++) {
      }
      if (c)
        i++;
      *tokenType = SqliteTokenType::TK_SPACE; /* IMP: R-22934-25134 */
      return i;
    }
    case CC_PERCENT: {
      *tokenType = SqliteTokenType::TK_REM;
      return 1;
    }
    case CC_EQ: {
      *tokenType = SqliteTokenType::TK_EQ;
      return 1 + (z[1] == '=');
    }
    case CC_LT: {
      if ((c = z[1]) == '=') {
        *tokenType = SqliteTokenType::TK_LE;
        return 2;
      } else if (c == '>') {
        *tokenType = SqliteTokenType::TK_NE;
        return 2;
      } else if (c == '<') {
        *tokenType = SqliteTokenType::TK_LSHIFT;
        return 2;
      } else {
        *tokenType = SqliteTokenType::TK_LT;
        return 1;
      }
    }
    case CC_GT: {
      if ((c = z[1]) == '=') {
        *tokenType = SqliteTokenType::TK_GE;
        return 2;
      } else if (c == '>') {
        *tokenType = SqliteTokenType::TK_RSHIFT;
        return 2;
      } else {
        *tokenType = SqliteTokenType::TK_GT;
        return 1;
      }
    }
    case CC_BANG: {
      if (z[1] != '=') {
        *tokenType = SqliteTokenType::TK_ILLEGAL;
        return 1;
      } else {
        *tokenType = SqliteTokenType::TK_NE;
        return 2;
      }
    }
    case CC_PIPE: {
      if (z[1] != '|') {
        *tokenType = SqliteTokenType::TK_BITOR;
        return 1;
      } else {
        *tokenType = SqliteTokenType::TK_CONCAT;
        return 2;
      }
    }
    case CC_COMMA: {
      *tokenType = SqliteTokenType::TK_COMMA;
      return 1;
    }
    case CC_AND: {
      *tokenType = SqliteTokenType::TK_BITAND;
      return 1;
    }
    case CC_TILDA: {
      *tokenType = SqliteTokenType::TK_BITNOT;
      return 1;
    }
    case CC_QUOTE: {
      int delim = z[0];
      for (i = 1; (c = z[i]) != 0; i++) {
        if (c == delim) {
          if (z[i + 1] == delim) {
            i++;
          } else {
            break;
          }
        }
      }
      if (c == '\'') {
        *tokenType = SqliteTokenType::TK_STRING;
        return i + 1;
      } else if (c != 0) {
        *tokenType = SqliteTokenType::TK_ID;
        return i + 1;
      } else {
        *tokenType = SqliteTokenType::TK_ILLEGAL;
        return i;
      }
    }
    case CC_DOT: {
      if (!isdigit(z[1])) {
        *tokenType = SqliteTokenType::TK_DOT;
        return 1;
      }
      [[fallthrough]];
    }
    case CC_DIGIT: {
      *tokenType = SqliteTokenType::TK_INTEGER;
      if (z[0] == '0' && (z[1] == 'x' || z[1] == 'X') && isxdigit(z[2])) {
        for (i = 3; isxdigit(z[i]); i++) {
        }
        return i;
      }
      for (i = 0; isxdigit(z[i]); i++) {
      }
      if (z[i] == '.') {
        i++;
        while (isxdigit(z[i])) {
          i++;
        }
        *tokenType = SqliteTokenType::TK_FLOAT;
      }
      if ((z[i] == 'e' || z[i] == 'E') &&
          (isdigit(z[i + 1]) ||
           ((z[i + 1] == '+' || z[i + 1] == '-') && isdigit(z[i + 2])))) {
        i += 2;
        while (isdigit(z[i])) {
          i++;
        }
        *tokenType = SqliteTokenType::TK_FLOAT;
      }
      while (IdChar(z[i])) {
        *tokenType = SqliteTokenType::TK_ILLEGAL;
        i++;
      }
      return i;
    }
    case CC_QUOTE2: {
      for (i = 1, c = z[0]; c != ']' && (c = z[i]) != 0; i++) {
      }
      *tokenType =
          c == ']' ? SqliteTokenType::TK_ID : SqliteTokenType::TK_ILLEGAL;
      return i;
    }
    case CC_VARNUM: {
      *tokenType = SqliteTokenType::TK_VARIABLE;
      for (i = 1; isdigit(z[i]); i++) {
      }
      return i;
    }
    case CC_DOLLAR:
    case CC_VARALPHA: {
      int n = 0;
      *tokenType = SqliteTokenType::TK_VARIABLE;
      for (i = 1; (c = z[i]) != 0; i++) {
        if (IdChar(c)) {
          n++;
        } else if (c == '(' && n > 0) {
          do {
            i++;
          } while ((c = z[i]) != 0 && !isspace(c) && c != ')');
          if (c == ')') {
            i++;
          } else {
            *tokenType = SqliteTokenType::TK_ILLEGAL;
          }
          break;
        } else if (c == ':' && z[i + 1] == ':') {
          i++;
        } else {
          break;
        }
      }
      if (n == 0)
        *tokenType = SqliteTokenType::TK_ILLEGAL;
      return i;
    }
    case CC_KYWD0: {
      for (i = 1; aiClass[z[i]] <= CC_KYWD; i++) {
      }
      if (IdChar(z[i])) {
        /* This token started out using characters that can appear in keywords,
        ** but z[i] is a character not allowed within keywords, so this must
        ** be an identifier instead */
        i++;
        break;
      }
      if (sqlite3_keyword_check(reinterpret_cast<const char*>(z), i)) {
        *tokenType = SqliteTokenType::TK_GENERIC_KEYWORD;
      } else {
        *tokenType = SqliteTokenType::TK_ID;
      }
      return i;
    }
    case CC_X: {
      if (z[1] == '\'') {
        *tokenType = SqliteTokenType::TK_BLOB;
        for (i = 2; isdigit(z[i]); i++) {
        }
        if (z[i] != '\'' || i % 2) {
          *tokenType = SqliteTokenType::TK_ILLEGAL;
          while (z[i] && z[i] != '\'') {
            i++;
          }
        }
        if (z[i])
          i++;
        return i;
      }
      [[fallthrough]];
    }
    case CC_KYWD:
    case CC_ID: {
      i = 1;
      break;
    }
    case CC_BOM: {
      if (z[1] == 0xbb && z[2] == 0xbf) {
        *tokenType = SqliteTokenType::TK_SPACE;
        return 3;
      }
      i = 1;
      break;
    }
    case CC_NUL: {
      *tokenType = SqliteTokenType::TK_ILLEGAL;
      return 0;
    }
    default: {
      *tokenType = SqliteTokenType::TK_ILLEGAL;
      return 1;
    }
  }
  while (IdChar(z[i])) {
    i++;
  }
  *tokenType = SqliteTokenType::TK_ID;
  return i;
}

}  // namespace

SqliteTokenizer::SqliteTokenizer(SqlSource sql) : source_(std::move(sql)) {}

SqliteTokenizer::Token SqliteTokenizer::Next() {
  Token token;
  const char* start = source_.sql().data() + offset_;
  int n = GetSqliteToken(reinterpret_cast<const unsigned char*>(start),
                         &token.token_type);
  offset_ += static_cast<uint32_t>(n);
  token.str = std::string_view(start, static_cast<uint32_t>(n));
  return token;
}

SqliteTokenizer::Token SqliteTokenizer::NextNonWhitespace() {
  Token t;
  for (t = Next(); t.token_type == SqliteTokenType::TK_SPACE; t = Next()) {
  }
  return t;
}

SqliteTokenizer::Token SqliteTokenizer::NextTerminal() {
  Token tok = Next();
  while (!tok.IsTerminal()) {
    tok = Next();
  }
  return tok;
}

SqlSource SqliteTokenizer::Substr(const Token& start, const Token& end) const {
  uint32_t offset =
      static_cast<uint32_t>(start.str.data() - source_.sql().c_str());
  uint32_t len = static_cast<uint32_t>(end.str.data() - start.str.data());
  return source_.Substr(offset, len);
}

SqlSource SqliteTokenizer::SubstrToken(const Token& token) const {
  uint32_t offset =
      static_cast<uint32_t>(token.str.data() - source_.sql().c_str());
  uint32_t len = static_cast<uint32_t>(token.str.size());
  return source_.Substr(offset, len);
}

std::string SqliteTokenizer::AsTraceback(const Token& token) const {
  PERFETTO_CHECK(source_.sql().c_str() <= token.str.data());
  PERFETTO_CHECK(token.str.data() <=
                 source_.sql().c_str() + source_.sql().size());
  uint32_t offset =
      static_cast<uint32_t>(token.str.data() - source_.sql().c_str());
  return source_.AsTraceback(offset);
}

void SqliteTokenizer::Rewrite(SqlSource::Rewriter& rewriter,
                              const Token& start,
                              const Token& end,
                              SqlSource rewrite,
                              EndToken end_token) const {
  uint32_t s_off =
      static_cast<uint32_t>(start.str.data() - source_.sql().c_str());
  uint32_t e_off =
      static_cast<uint32_t>(end.str.data() - source_.sql().c_str());
  uint32_t e_diff = end_token == EndToken::kInclusive
                        ? static_cast<uint32_t>(end.str.size())
                        : 0;
  rewriter.Rewrite(s_off, e_off + e_diff, std::move(rewrite));
}

void SqliteTokenizer::RewriteToken(SqlSource::Rewriter& rewriter,
                                   const Token& token,
                                   SqlSource rewrite) const {
  uint32_t s_off =
      static_cast<uint32_t>(token.str.data() - source_.sql().c_str());
  uint32_t e_off = static_cast<uint32_t>(token.str.data() + token.str.size() -
                                         source_.sql().c_str());
  rewriter.Rewrite(s_off, e_off, std::move(rewrite));
}

}  // namespace trace_processor
}  // namespace perfetto
