/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "perfetto/ext/base/murmur_hash.h"

#include <string>
#include <string_view>

#include "perfetto/ext/base/string_view.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {

TEST(MurmurHashTest, CombiningOneValueIsItsHash) {
  EXPECT_EQ(MurmurHashCombine(int64_t{42}), MurmurHashValue(int64_t{42}));
  EXPECT_EQ(MurmurHashCombine(3.5), MurmurHashValue(3.5));
  EXPECT_NE(MurmurHashCombine(int64_t{1}, int64_t{2}),
            MurmurHashCombine(int64_t{2}, int64_t{1}));
}

TEST(MurmurHashTest, StringView) {
  base::StringView a = "abc";
  base::StringView b = "def";
  EXPECT_NE(murmur_internal::MurmurHashBytes(a.data(), a.size()),
            murmur_internal::MurmurHashBytes(b.data(), b.size()));
}

TEST(MurmurHashTest, HeterogeneousStringHash) {
  MurmurHash<std::string> hasher;
  const char* pointer = "abc";
  const auto expected = hasher(std::string("abc"));
  EXPECT_EQ(hasher(pointer), expected);
  EXPECT_EQ(hasher(std::string_view("abc")), expected);
  EXPECT_EQ(hasher("abc"), expected);
  // Select the heterogeneous overload explicitly: the owning-key overload
  // could otherwise hide missing array support via a string conversion.
  EXPECT_EQ(hasher.operator()<char[4]>("abc"), expected);
}

}  // namespace
}  // namespace perfetto::base
