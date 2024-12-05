/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "perfetto/ext/base/string_view_splitter.h"

#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using testing::ElementsAreArray;

TEST(StringViewSplitterTest, StdString) {
  {
    StringViewSplitter ss("", 'x');
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    EXPECT_FALSE(ss.Next());
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
  }
  {
    StringViewSplitter ss("", 'x');
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    EXPECT_FALSE(ss.Next());
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
  }
  {
    StringViewSplitter ss("a", 'x');
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("a", ss.cur_token().ToStdString().c_str());
    EXPECT_FALSE(ss.Next());
  }
  {
    StringViewSplitter ss("abc", 'x');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("abc", ss.cur_token().ToStdString().c_str());
    EXPECT_FALSE(ss.Next());
  }
  {
    StringViewSplitter ss("ab,", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("ab", ss.cur_token().ToStdString().c_str());
    EXPECT_FALSE(ss.Next());
  }
  {
    StringViewSplitter ss(",ab,", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("ab", ss.cur_token().ToStdString().c_str());
    EXPECT_FALSE(ss.Next());
  }
  {
    StringViewSplitter ss("a,b,c", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("a", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("b", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("c", ss.cur_token().ToStdString().c_str());

    EXPECT_FALSE(ss.Next());
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
  }
  {
    StringViewSplitter ss("a,b,c,", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("a", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("b", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("c", ss.cur_token().ToStdString().c_str());

    EXPECT_FALSE(ss.Next());
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
  }
  {
    StringViewSplitter ss(",,a,,b,,,,c,,,", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("a", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("b", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("c", ss.cur_token().ToStdString().c_str());

    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());
    }
  }
  {
    StringViewSplitter ss(",,", ',');
    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());
      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
  {
    StringViewSplitter ss(",,foo", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("foo", ss.cur_token().ToStdString().c_str());

    EXPECT_FALSE(ss.Next());
  }
}

TEST(StringViewSplitterTest, CString) {
  {
    StringViewSplitter ss("foo\nbar\n\nbaz\n", '\n');
    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("foo", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("bar", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("baz", ss.cur_token().ToStdString().c_str());

    EXPECT_FALSE(ss.Next());
  }
  {
    StringViewSplitter ss("", ',');
    EXPECT_FALSE(ss.Next());
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
  }
  {
    StringViewSplitter ss(",,foo,bar\0,baz", ',');

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("foo", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("bar", ss.cur_token().ToStdString().c_str());

    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());
    }
  }
  {
    StringViewSplitter ss(",,a\0,b,", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("a", ss.cur_token().ToStdString().c_str());

    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());
      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
  {
    StringViewSplitter ss(",a,\0b", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("a", ss.cur_token().ToStdString().c_str());

    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());

      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
  {
    StringViewSplitter ss(",a\0\0,x\0\0b", ',');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("a", ss.cur_token().ToStdString().c_str());

    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());

      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
}

TEST(StringViewSplitterTest, SplitOnNUL) {
  {
    StringViewSplitter ss("", '\0');
    EXPECT_FALSE(ss.Next());
    EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
  }
  {
    std::string str;
    str.resize(48);
    memcpy(&str[0], "foo\0", 4);
    memcpy(&str[4], "bar\0", 4);
    memcpy(&str[20], "baz", 3);
    StringViewSplitter ss(base::StringView(std::move(str)), '\0');
    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("foo", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("bar", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());
    EXPECT_STREQ("baz", ss.cur_token().ToStdString().c_str());

    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());

      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
  {
    char buf[] = "foo\0bar\0baz\0";
    StringViewSplitter ss(base::StringView("foo\0bar\0baz\0", sizeof(buf)),
                          '\0');
    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("foo", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("bar", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("baz", ss.cur_token().ToStdString().c_str());

    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());

      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
  {
    char buf[] = "\0\0foo\0\0\0\0bar\0baz\0\0";
    StringViewSplitter ss(base::StringView(buf, sizeof(buf)), '\0');
    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("foo", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("bar", ss.cur_token().ToStdString().c_str());

    EXPECT_TRUE(ss.Next());

    EXPECT_STREQ("baz", ss.cur_token().ToStdString().c_str());

    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());

      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
  {
    StringViewSplitter ss("", '\0');
    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());

      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
  {
    StringViewSplitter ss("\0", '\0');
    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());

      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
  {
    StringViewSplitter ss("\0\0", '\0');
    for (int i = 0; i < 3; i++) {
      EXPECT_FALSE(ss.Next());

      EXPECT_STREQ("", ss.cur_token().ToStdString().c_str());
    }
  }
}

TEST(StringViewSplitterTest, NestedUsage) {
  char text[] = R"(
l1w1 l1w2 l1w3

,l,2,w,1   l,2,,w,,2,,
)";
  std::vector<base::StringView> all_lines;
  std::vector<base::StringView> all_words;
  std::vector<base::StringView> all_tokens;
  for (StringViewSplitter lines(base::StringView(text), '\n'); lines.Next();) {
    all_lines.push_back(lines.cur_token());
    for (StringViewSplitter words(&lines, ' '); words.Next();) {
      all_words.push_back(words.cur_token());
      for (StringViewSplitter tokens(&words, ','); tokens.Next();) {
        all_tokens.push_back(tokens.cur_token());
      }
    }
  }
  EXPECT_THAT(all_lines,
              ElementsAreArray({"l1w1 l1w2 l1w3", ",l,2,w,1   l,2,,w,,2,,"}));
  EXPECT_THAT(all_words, ElementsAreArray({"l1w1", "l1w2", "l1w3", ",l,2,w,1",
                                           "l,2,,w,,2,,"}));
  EXPECT_THAT(all_tokens, ElementsAreArray({"l1w1", "l1w2", "l1w3", "l", "2",
                                            "w", "1", "l", "2", "w", "2"}));
}  // namespace

TEST(StringViewSplitterTest, EmptyTokens) {
  std::vector<std::string> tokens;
  for (StringViewSplitter lines(
           "a,,b", ',', StringViewSplitter::EmptyTokenMode::ALLOW_EMPTY_TOKENS);
       lines.Next();) {
    tokens.push_back(lines.cur_token().ToStdString());
  }
  EXPECT_THAT(tokens, testing::ElementsAre("a", "", "b"));
}  // namespace

}  // namespace
}  // namespace base
}  // namespace perfetto
