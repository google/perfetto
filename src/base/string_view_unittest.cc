/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "perfetto/base/string_view.h"

#include <forward_list>
#include <unordered_map>
#include <unordered_set>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace base {
namespace {

TEST(StringViewTest, BasicCases) {
  EXPECT_EQ(StringView(""), StringView(""));
  EXPECT_EQ(StringView(""), StringView("", 0));
  EXPECT_EQ(StringView("ab"), StringView("ab", 2));
  EXPECT_EQ(StringView("ax", 1), StringView("ay", 1));
  EXPECT_EQ(StringView("ax", 1), StringView("a"));
  EXPECT_EQ(StringView("ax", 1), "a");
  EXPECT_EQ(StringView("foo|", 3).ToStdString(), std::string("foo"));
  EXPECT_TRUE(StringView("x") != StringView(""));
  EXPECT_TRUE(StringView("") != StringView("y"));
  EXPECT_TRUE(StringView("a") != StringView("b"));
  EXPECT_EQ(StringView("").size(), 0);
  EXPECT_NE(StringView("").data(), nullptr);
  EXPECT_TRUE(StringView("").empty());
  EXPECT_FALSE(StringView("x").empty());

  {
    StringView x("abc");
    EXPECT_EQ(x.size(), 3u);
    EXPECT_EQ(x.data()[0], 'a');
    EXPECT_EQ(x.data()[2], 'c');
    EXPECT_TRUE(x == "abc");
    EXPECT_TRUE(x == StringView("abc"));
    EXPECT_TRUE(x != StringView("abcd"));
  }
}

TEST(StringViewTest, HashCollisions) {
  std::unordered_map<uint64_t, StringView> hashes;
  std::unordered_set<StringView> sv_set;
  auto insert_sv = [&hashes, &sv_set](StringView sv) {
    hashes.emplace(sv.Hash(), sv);
    size_t prev_set_size = sv_set.size();
    sv_set.insert(sv);
    ASSERT_EQ(sv_set.size(), prev_set_size + 1);
  };

  insert_sv("");
  EXPECT_EQ(hashes.size(), 1u);
  size_t last_size = 1;
  std::forward_list<std::string> strings;
  for (uint8_t c = 0; c < 0x80; c++) {
    char buf[500];
    memset(buf, static_cast<char>(c), sizeof(buf));
    for (size_t i = 1; i <= sizeof(buf); i++) {
      strings.emplace_front(buf, i);
      StringView sv(strings.front());
      auto other = hashes.find(sv.Hash());
      if (other == hashes.end()) {
        insert_sv(sv);
        ++last_size;
        ASSERT_EQ(hashes.size(), last_size);
        continue;
      }
      EXPECT_TRUE(false) << "H(" << sv.ToStdString() << ") = "
                         << "H(" << other->second.ToStdString() << ")";
    }
  }
}

}  // namespace
}  // namespace base
}  // namespace perfetto
