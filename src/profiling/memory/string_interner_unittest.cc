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

#include "src/profiling/memory/string_interner.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace {

TEST(StringInternerTest, Basic) {
  StringInterner interner;
  {
    StringInterner::InternedString interned_str = interner.Intern("foo");
    ASSERT_EQ(interned_str.str(), "foo");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(StringInternerTest, TwoStrings) {
  StringInterner interner;
  {
    StringInterner::InternedString interned_str = interner.Intern("foo");
    StringInterner::InternedString other_interned_str = interner.Intern("bar");
    ASSERT_EQ(interned_str.str(), "foo");
    ASSERT_EQ(other_interned_str.str(), "bar");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(StringInternerTest, TwoReferences) {
  StringInterner interner;
  {
    StringInterner::InternedString interned_str = interner.Intern("foo");
    ASSERT_EQ(interned_str.str(), "foo");
    StringInterner::InternedString interned_str2 = interner.Intern("foo");
    ASSERT_EQ(interner.entry_count_for_testing(), 1);
    ASSERT_EQ(interned_str2.str(), "foo");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(StringInternerTest, Move) {
  StringInterner interner;
  {
    StringInterner::InternedString interned_str = interner.Intern("foo");
    {
      StringInterner::InternedString interned_str2(std::move(interned_str));
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.str(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 0);
  }
}

TEST(StringInternerTest, Copy) {
  StringInterner interner;
  {
    StringInterner::InternedString interned_str = interner.Intern("foo");
    {
      StringInterner::InternedString interned_str2(interned_str);
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.str(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 1);
    ASSERT_EQ(interned_str.str(), "foo");
  }
}

TEST(StringInternerTest, MoveAssign) {
  StringInterner interner;
  {
    StringInterner::InternedString interned_str = interner.Intern("foo");
    {
      StringInterner::InternedString interned_str2 = std::move(interned_str);
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.str(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 0);
  }
}

TEST(StringInternerTest, CopyAssign) {
  StringInterner interner;
  {
    StringInterner::InternedString interned_str = interner.Intern("foo");
    {
      StringInterner::InternedString interned_str2 = interned_str;
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.str(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 1);
    ASSERT_EQ(interned_str.str(), "foo");
  }
}

}  // namespace
}  // namespace perfetto
