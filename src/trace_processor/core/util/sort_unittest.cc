/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/core/util/sort.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

struct TestEntry {
  uint32_t key;
  uint32_t value;
};

struct TestEntry64 {
  uint64_t key;
  uint32_t value;
};

struct TestEntryString {
  static constexpr size_t kMaxKeySize = 32;
  char key[kMaxKeySize];
  uint32_t value;
};

TEST(RadixSort, SmokeTest) {
  std::vector<uint32_t> data = {3, 1, 4, 1, 5, 9, 2, 6};
  std::vector<uint32_t> scratch(data.size());
  std::vector<uint32_t> counts(RadixSortCountsSize(32, data.size()));

  uint32_t* result =
      RadixSort(data.data(), data.data() + data.size(), scratch.data(),
                counts.data(), 32, [](uint32_t x) { return uint64_t{x}; });

  std::vector<uint32_t> sorted_data(result, result + data.size());
  ASSERT_THAT(sorted_data, testing::ElementsAre(1, 1, 2, 3, 4, 5, 6, 9));
}

// Checks every key width against std::stable_sort, with keys which vary in
// all of their bits, in only some digits, or have constant bits above them.
TEST(RadixSort, MatchesStableSort) {
  std::minstd_rand0 rnd(0);
  for (uint32_t key_bits = 1; key_bits <= 64; ++key_bits) {
    uint64_t mask =
        key_bits == 64 ? ~uint64_t{0} : (uint64_t{1} << key_bits) - 1;
    for (uint64_t varying : {mask, mask & 0xFF, mask & ~uint64_t{0xFFFF}}) {
      uint64_t constant = ~mask & 0xA5A5A5A5A5A5A5A5;
      std::vector<TestEntry64> data(3000);
      for (uint32_t i = 0; i < data.size(); ++i) {
        uint64_t r = (uint64_t{rnd()} << 32) ^ rnd();
        data[i] = {constant | (r & varying), i};
      }
      std::vector<TestEntry64> scratch(data.size());
      std::vector<uint32_t> counts(RadixSortCountsSize(key_bits, data.size()));
      TestEntry64* result = RadixSort(
          data.data(), data.data() + data.size(), scratch.data(), counts.data(),
          key_bits, [](const TestEntry64& x) { return x.key; });

      std::vector<TestEntry64> expected = data;
      std::stable_sort(expected.begin(), expected.end(),
                       [](const TestEntry64& a, const TestEntry64& b) {
                         return a.key < b.key;
                       });
      for (size_t i = 0; i < data.size(); ++i) {
        ASSERT_EQ(result[i].key, expected[i].key) << key_bits;
        ASSERT_EQ(result[i].value, expected[i].value) << key_bits;
      }
    }
  }
}

TEST(RadixSort, Stability) {
  std::vector<TestEntry> data = {{3, 0}, {1, 1}, {4, 2}, {1, 3},
                                 {5, 4}, {9, 5}, {2, 6}, {6, 7}};
  std::vector<TestEntry> scratch(data.size());
  std::vector<uint32_t> counts(RadixSortCountsSize(32, data.size()));

  TestEntry* result =
      RadixSort(data.data(), data.data() + data.size(), scratch.data(),
                counts.data(), 32, [](const TestEntry& x) { return x.key; });

  std::vector<TestEntry> sorted_data(result, result + data.size());

  // The two entries with key 1 should maintain their original order.
  ASSERT_EQ(sorted_data[0].key, 1u);
  ASSERT_EQ(sorted_data[0].value, 1u);
  ASSERT_EQ(sorted_data[1].key, 1u);
  ASSERT_EQ(sorted_data[1].value, 3u);
}

// Sizes either side of where radix sorting becomes cheaper, with few and many
// distinct keys, against std::stable_sort.
TEST(StableSortByKey, MatchesStableSort) {
  std::minstd_rand0 rnd(0);
  for (uint32_t size : {0u, 1u, 2u, 100u, 5000u, 100000u}) {
    for (uint32_t key_bits : {3u, 40u}) {
      std::vector<TestEntry64> data(size);
      for (uint32_t i = 0; i < size; ++i) {
        uint64_t r = (uint64_t{rnd()} << 32) ^ rnd();
        data[i] = {r & ((uint64_t{1} << key_bits) - 1), i};
      }
      std::vector<TestEntry64> scratch(size);
      TestEntry64* result = StableSortByKey(
          data.data(), data.data() + size, scratch.data(), key_bits,
          [](const TestEntry64& x) { return x.key; },
          [](const TestEntry64& x) { return x.value; });

      std::vector<TestEntry64> expected = data;
      std::stable_sort(expected.begin(), expected.end(),
                       [](const TestEntry64& a, const TestEntry64& b) {
                         return a.key < b.key;
                       });
      for (uint32_t i = 0; i < size; ++i) {
        ASSERT_EQ(result[i].key, expected[i].key) << size << " " << key_bits;
        ASSERT_EQ(result[i].value, expected[i].value)
            << size << " " << key_bits;
      }
    }
  }
}

TEST(MsdRadixSort, SmokeTest) {
  std::vector<TestEntryString> data;
  data.push_back(TestEntryString{"", 0});
  base::StringCopy(data.back().key, "apple", TestEntryString::kMaxKeySize);
  data.push_back(TestEntryString{"", 1});
  base::StringCopy(data.back().key, "banana", TestEntryString::kMaxKeySize);
  data.push_back(TestEntryString{"", 2});
  base::StringCopy(data.back().key, "apricot", TestEntryString::kMaxKeySize);
  data.push_back(TestEntryString{"", 3});
  base::StringCopy(data.back().key, "ban", TestEntryString::kMaxKeySize);

  std::vector<TestEntryString> scratch(data.size());

  MsdRadixSort(
      data.data(), data.data() + data.size(), scratch.data(),
      [](const TestEntryString& x) { return std::string_view(x.key); });

  ASSERT_STREQ(data[0].key, "apple");
  ASSERT_STREQ(data[1].key, "apricot");
  ASSERT_STREQ(data[2].key, "ban");
  ASSERT_STREQ(data[3].key, "banana");
}

TEST(MsdRadixSort, LargeRandomStringTest) {
  std::vector<TestEntryString> data;
  std::minstd_rand0 rnd(42);
  for (uint32_t i = 0; i < 1000; ++i) {
    uint32_t len = 5 + (rnd() % (TestEntryString::kMaxKeySize - 6));
    std::string key;
    for (uint32_t j = 0; j < len; ++j) {
      key += static_cast<char>('a' + (rnd() % 26));
    }
    data.push_back(TestEntryString{"", i});
    base::StringCopy(data.back().key, key.c_str(),
                     TestEntryString::kMaxKeySize);
  }

  std::vector<TestEntryString> scratch(data.size());
  MsdRadixSort(
      data.data(), data.data() + data.size(), scratch.data(),
      [](const TestEntryString& x) { return std::string_view(x.key); });

  std::vector<TestEntryString> std_sorted = data;
  std::sort(std_sorted.begin(), std_sorted.end(),
            [](const TestEntryString& a, const TestEntryString& b) {
              return strcmp(a.key, b.key) < 0;
            });

  for (size_t i = 0; i < data.size(); ++i) {
    ASSERT_STREQ(data[i].key, std_sorted[i].key);
  }
}

TEST(MsdRadixSort, SingleElementBuckets) {
  std::vector<TestEntryString> data;
  data.push_back(TestEntryString{"", 0});
  base::StringCopy(data.back().key, "a", TestEntryString::kMaxKeySize);
  data.push_back(TestEntryString{"", 1});
  base::StringCopy(data.back().key, "b", TestEntryString::kMaxKeySize);
  data.push_back(TestEntryString{"", 2});
  base::StringCopy(data.back().key, "c", TestEntryString::kMaxKeySize);

  std::vector<TestEntryString> scratch(data.size());

  MsdRadixSort(
      data.data(), data.data() + data.size(), scratch.data(),
      [](const TestEntryString& x) { return std::string_view(x.key); });

  ASSERT_STREQ(data[0].key, "a");
  ASSERT_STREQ(data[1].key, "b");
  ASSERT_STREQ(data[2].key, "c");
}

}  // namespace
}  // namespace perfetto::trace_processor::core
