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
    Interned<std::string> interned_str = interner.Intern("foo");
    ASSERT_EQ(interned_str.data(), "foo");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(InternerStringTest, TwoStrings) {
  Interner<std::string> interner;
  {
    Interned<std::string> interned_str = interner.Intern("foo");
    Interned<std::string> other_interned_str = interner.Intern("bar");
    ASSERT_EQ(interned_str.data(), "foo");
    ASSERT_EQ(other_interned_str.data(), "bar");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(InternerStringTest, TwoReferences) {
  Interner<std::string> interner;
  {
    Interned<std::string> interned_str = interner.Intern("foo");
    ASSERT_EQ(interned_str.data(), "foo");
    Interned<std::string> interned_str2 = interner.Intern("foo");
    ASSERT_EQ(interner.entry_count_for_testing(), 1);
    ASSERT_EQ(interned_str2.data(), "foo");
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

TEST(InternerStringTest, Move) {
  Interner<std::string> interner;
  {
    Interned<std::string> interned_str = interner.Intern("foo");
    {
      Interned<std::string> interned_str2(std::move(interned_str));
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.data(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 0);
  }
}

TEST(InternerStringTest, Copy) {
  Interner<std::string> interner;
  {
    Interned<std::string> interned_str = interner.Intern("foo");
    {
      Interned<std::string> interned_str2(interned_str);
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
    Interned<std::string> interned_str = interner.Intern("foo");
    {
      Interned<std::string> interned_str2 = std::move(interned_str);
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.data(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 0);
  }
}

TEST(InternerStringTest, CopyAssign) {
  Interner<std::string> interner;
  {
    Interned<std::string> interned_str = interner.Intern("foo");
    {
      Interned<std::string> interned_str2 = interned_str;
      ASSERT_EQ(interner.entry_count_for_testing(), 1);
      ASSERT_EQ(interned_str2.data(), "foo");
    }
    ASSERT_EQ(interner.entry_count_for_testing(), 1);
    ASSERT_EQ(interned_str.data(), "foo");
  }
}

TEST(InternerStringTest, IDsUnique) {
  Interner<std::string> interner;
  Interned<std::string> interned_str = interner.Intern("foo");
  Interned<std::string> same_interned_str = interner.Intern("foo");
  Interned<std::string> other_interned_str = interner.Intern("bar");
  EXPECT_EQ(interned_str.id(), same_interned_str.id());
  EXPECT_NE(interned_str.id(), other_interned_str.id());
}

TEST(InternerStringTest, IdsConsecutive) {
  Interner<std::string> interner;
  {
    Interned<std::string> interned_str = interner.Intern("foo");
    interner.Intern("foo");
    Interned<std::string> other_interned_str = interner.Intern("bar");
    ASSERT_EQ(interned_str.id() + 1, other_interned_str.id());
  }
  ASSERT_EQ(interner.entry_count_for_testing(), 0);
}

class NoCopyOrMove {
 public:
  NoCopyOrMove(const NoCopyOrMove&) = delete;
  NoCopyOrMove& operator=(const NoCopyOrMove&) = delete;
  NoCopyOrMove(const NoCopyOrMove&&) = delete;
  NoCopyOrMove& operator=(const NoCopyOrMove&&) = delete;
  NoCopyOrMove(int d) : data(d) {}
  ~NoCopyOrMove() {}
  bool operator<(const NoCopyOrMove& other) const { return data < other.data; }

 private:
  int data;
};

TEST(InternerStringTest, NoCopyOrMove) {
  Interner<NoCopyOrMove> interner;
  Interned<NoCopyOrMove> interned_str = interner.Intern(1);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
