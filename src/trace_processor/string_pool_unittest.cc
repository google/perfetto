/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/string_pool.h"

#include <random>

#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(StringPoolTest, EmptyPool) {
  StringPool pool;

  ASSERT_EQ(pool.Get(0).c_str(), nullptr);

  auto it = pool.CreateIterator();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.StringView().c_str(), nullptr);
  ASSERT_FALSE(++it);
}

TEST(StringPoolTest, InternAndRetrieve) {
  StringPool pool;

  static char kString[] = "Test String";
  auto id = pool.InternString(kString);
  ASSERT_STREQ(pool.Get(id).c_str(), kString);
  ASSERT_EQ(pool.Get(id), kString);
  ASSERT_EQ(id, pool.InternString(kString));
}

TEST(StringPoolTest, NullPointerHandling) {
  StringPool pool;

  auto id = pool.InternString(NullTermStringView());
  ASSERT_EQ(id, 0);
  ASSERT_EQ(pool.Get(id).c_str(), nullptr);
}

TEST(StringPoolTest, Iterator) {
  StringPool pool;

  auto it = pool.CreateIterator();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.StringView().c_str(), nullptr);
  ASSERT_FALSE(++it);

  static char kString[] = "Test String";
  pool.InternString(kString);

  it = pool.CreateIterator();
  ASSERT_TRUE(++it);
  ASSERT_STREQ(it.StringView().c_str(), kString);
  ASSERT_FALSE(++it);
}

TEST(StringPoolTest, StressTest) {
  // First create a buffer with 128MB of random characters.
  constexpr size_t kBufferSize = 128 * 1024 * 1024;
  std::minstd_rand0 rnd_engine(0);
  std::unique_ptr<char[]> buffer(new char[kBufferSize]);
  for (size_t i = 0; i < kBufferSize; i++)
    buffer.get()[i] = 'A' + (rnd_engine() % 26);

  // Next create strings of length 0 to 16k in length from this buffer and
  // intern them, storing their ids.
  StringPool pool;
  std::multimap<StringPool::Id, base::StringView> string_map;
  constexpr uint16_t kMaxStrSize = 16u * 1024u - 1;
  for (size_t i = 0;;) {
    size_t length = static_cast<uint64_t>(rnd_engine()) % (kMaxStrSize + 1);
    if (i + length > kBufferSize)
      break;

    auto str = base::StringView(&buffer.get()[i], length);
    string_map.emplace(pool.InternString(str), str);
    i += length;
  }

  // Finally, iterate through each string in the string pool, check that all ids
  // that match in the multimap are equal, and finish by checking we've removed
  // every item in the multimap.
  for (auto it = pool.CreateIterator(); it; ++it) {
    ASSERT_EQ(it.StringView(), pool.Get(it.StringId()));

    auto it_pair = string_map.equal_range(it.StringId());
    for (auto in_it = it_pair.first; in_it != it_pair.second; ++in_it) {
      ASSERT_EQ(it.StringView(), in_it->second);
    }
    string_map.erase(it_pair.first, it_pair.second);
  }
  ASSERT_EQ(string_map.size(), 0);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
