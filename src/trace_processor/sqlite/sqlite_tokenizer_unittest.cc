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
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/sqlite/sql_source.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using Token = SqliteTokenizer::Token;
using Type = SqliteTokenType;

class SqliteTokenizerTest : public ::testing::Test {
 protected:
  std::vector<SqliteTokenizer::Token> Tokenize(const char* ptr) {
    tokenizer_.Reset(SqlSource::FromTraceProcessorImplementation(ptr));
    std::vector<SqliteTokenizer::Token> tokens;
    for (auto t = tokenizer_.Next(); !t.str.empty(); t = tokenizer_.Next()) {
      tokens.push_back(t);
    }
    return tokens;
  }

  SqliteTokenizer tokenizer_{SqlSource::FromTraceProcessorImplementation("")};
};

TEST_F(SqliteTokenizerTest, EmptyString) {
  ASSERT_THAT(Tokenize(""), testing::IsEmpty());
}

TEST_F(SqliteTokenizerTest, OnlySpace) {
  ASSERT_THAT(Tokenize(" "), testing::ElementsAre(Token{" ", Type::TK_SPACE}));
}

TEST_F(SqliteTokenizerTest, SpaceColon) {
  ASSERT_THAT(Tokenize(" ;"), testing::ElementsAre(Token{" ", Type::TK_SPACE},
                                                   Token{";", Type::TK_SEMI}));
}

TEST_F(SqliteTokenizerTest, Select) {
  ASSERT_THAT(
      Tokenize("SELECT * FROM slice;"),
      testing::ElementsAre(
          Token{"SELECT", Type::TK_GENERIC_KEYWORD}, Token{" ", Type::TK_SPACE},
          Token{"*", Type::TK_STAR}, Token{" ", Type::TK_SPACE},
          Token{"FROM", Type::TK_GENERIC_KEYWORD}, Token{" ", Type::TK_SPACE},
          Token{"slice", Type::TK_ID}, Token{";", Type::TK_SEMI}));
}

TEST_F(SqliteTokenizerTest, PastEndErrorToken) {
  tokenizer_.Reset(SqlSource::FromTraceProcessorImplementation("S"));
  ASSERT_EQ(tokenizer_.Next(), (Token{"S", Type::TK_ID}));

  auto end_token = tokenizer_.Next();
  ASSERT_EQ(end_token, (Token{"", Type::TK_ILLEGAL}));
  ASSERT_EQ(tokenizer_.AsTraceback(end_token),
            "  Trace Processor Internal line 1 col 2\n"
            "    S\n"
            "     ^\n");
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
