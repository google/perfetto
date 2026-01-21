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

#include "perfetto/ext/base/string_view.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {

TEST(MurmurHashTest, StringView) {
  base::StringView a = "abc";
  base::StringView b = "def";
  EXPECT_NE(murmur_internal::MurmurHashBytes(a.data(), a.size()),
            murmur_internal::MurmurHashBytes(b.data(), b.size()));
}

TEST(MurmurHashTest, Combine) {
  EXPECT_NE(MurmurHashCombine(1, 2), MurmurHashCombine(2, 1));
  EXPECT_NE(MurmurHashCombine(1, 2), MurmurHashCombine(1));
  EXPECT_EQ(MurmurHashCombine(1, 2, 3), MurmurHashCombine(1, std::tuple(2, 3)));
}

TEST(MurmurHashTest, Combiner) {
  MurmurHashCombiner combiner;
  combiner.Combine(1u);
  combiner.Combine(2u);
  uint64_t hash1 = combiner.digest();

  EXPECT_EQ(hash1, MurmurHashCombine(1u, 2u));
}

struct CustomType {
  int a;
  int b;
  template <typename H>
  friend H PerfettoHashValue(H h, const CustomType& v) {
    return H::Combine(std::move(h), std::tie(v.a, v.b));
  }
};

TEST(MurmurHashTest, CustomType) {
  CustomType v1{1, 2};
  CustomType v2{2, 1};
  EXPECT_NE(MurmurHashValue(v1), MurmurHashValue(v2));
  EXPECT_EQ(MurmurHashValue(v1), MurmurHashCombine(1, 2));
}

}  // namespace
}  // namespace perfetto::base
