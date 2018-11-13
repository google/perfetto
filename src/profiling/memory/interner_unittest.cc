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

#include "src/profiling/memory/interner.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace profiling {
namespace {

TEST(InternerStringTest, Basic) {
  Interner<std::string> interner;
  {
    Interner<std::string>::Interned interned_str = interner.Intern("foo");
    ASSERT_EQ(interned_str.data(), "foo");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(InternerStringTest, TwoStrings) {
  Interner<std::string> interner;
  {
    Interner<std::string>::Interned interned_str = interner.Intern("foo");
    Interner<std::string>::Interned other_interned_str = interner.Intern("bar");
    ASSERT_EQ(interned_str.data(), "foo");
    ASSERT_EQ(other_interned_str.data(), "bar");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(InternerStringTest, TwoReferences) {
  Interner<std::string> interner;
  {
    Interner<std::string>::Interned interned_str = interner.Intern("foo");
    ASSERT_EQ(interned_str.data(), "foo");
    Interner<std::string>::Interned interned_str2 = interner.Intern("foo");
    ASSERT_EQ(interner.entry_count_for_testing(), 1);
    ASSERT_EQ(interned_str2.data(), "foo");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(InternerStringTest, Move) {
  Interner<std::string> interner;
  {
    Interner<std::string>::Interned interned_str = interner.Intern("foo");
    {
      Interner<std::string>::Interned interned_str2(std::move(interned_str));
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.data(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 0);
  }
}

TEST(InternerStringTest, Copy) {
  Interner<std::string> interner;
  {
    Interner<std::string>::Interned interned_str = interner.Intern("foo");
    {
      Interner<std::string>::Interned interned_str2(interned_str);
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.data(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 1);
    ASSERT_EQ(interned_str.data(), "foo");
  }
}

TEST(InternerStringTest, MoveAssign) {
  Interner<std::string> interner;
  {
    Interner<std::string>::Interned interned_str = interner.Intern("foo");
    {
      Interner<std::string>::Interned interned_str2 = std::move(interned_str);
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.data(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 0);
  }
}

TEST(InternerStringTest, CopyAssign) {
  Interner<std::string> interner;
  {
    Interner<std::string>::Interned interned_str = interner.Intern("foo");
    {
      Interner<std::string>::Interned interned_str2 = interned_str;
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.data(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 1);
    ASSERT_EQ(interned_str.data(), "foo");
  }
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
