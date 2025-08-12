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

#include "src/trace_processor/dataframe/impl/sort.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "perfetto/ext/base/endian.h"
#include "perfetto/ext/base/string_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {

struct TestEntry {
  uint32_t key;
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
  std::vector<uint32_t> counts(1 << 16);

  uint32_t* result = RadixSort(
      data.data(), data.data() + data.size(), scratch.data(), counts.data(),
      sizeof(uint32_t),
      [](const uint32_t& x) { return reinterpret_cast<const uint8_t*>(&x); });

  std::vector<uint32_t> sorted_data(result, result + data.size());
  ASSERT_THAT(sorted_data, testing::ElementsAre(1, 1, 2, 3, 4, 5, 6, 9));
}

TEST(RadixSort, LargeRandomTest) {
  std::vector<uint64_t> data;
  std::vector<uint64_t> bswap_data;
  std::minstd_rand0 rnd(0);
  for (uint32_t i = 0; i < 10000; ++i) {
    data.push_back(static_cast<uint64_t>(rnd()));
    bswap_data.push_back(base::HostToBE64(data.back()));
  }

  std::vector<uint64_t> scratch(data.size());
  std::vector<uint32_t> counts(1 << 16);
  uint64_t* result = RadixSort(
      bswap_data.data(), bswap_data.data() + data.size(), scratch.data(),
      counts.data(), sizeof(uint64_t),
      [](const uint64_t& x) { return reinterpret_cast<const uint8_t*>(&x); });

  std::vector<uint64_t> sorted_data(result, result + data.size());
  for (auto& item : sorted_data) {
    item = base::BE64ToHost(item);
  }
  std::vector<uint64_t> std_sorted = data;
  std::sort(std_sorted.begin(), std_sorted.end());

  ASSERT_EQ(sorted_data, std_sorted);
}

TEST(RadixSort, StructSort) {
  std::vector<TestEntry> data = {{3, 0}, {1, 1}, {4, 2}, {1, 3},
                                 {5, 4}, {9, 5}, {2, 6}, {6, 7}};
  std::vector<TestEntry> scratch(data.size());
  std::vector<uint32_t> counts(1 << 16);

  TestEntry* result =
      RadixSort(data.data(), data.data() + data.size(), scratch.data(),
                counts.data(), sizeof(uint32_t), [](const TestEntry& x) {
                  return reinterpret_cast<const uint8_t*>(&x.key);
                });

  std::vector<TestEntry> sorted_data(result, result + data.size());
  ASSERT_THAT(sorted_data,
              testing::ElementsAre(testing::Field(&TestEntry::key, 1),
                                   testing::Field(&TestEntry::key, 1),
                                   testing::Field(&TestEntry::key, 2),
                                   testing::Field(&TestEntry::key, 3),
                                   testing::Field(&TestEntry::key, 4),
                                   testing::Field(&TestEntry::key, 5),
                                   testing::Field(&TestEntry::key, 6),
                                   testing::Field(&TestEntry::key, 9)));
}

TEST(RadixSort, OddKeyWidth) {
  struct Key5Byte {
    uint8_t key[5];
  };
  std::vector<Key5Byte> data(100);
  std::minstd_rand0 rnd(0);
  for (auto& item : data) {
    for (size_t i = 0; i < 5; ++i) {
      item.key[i] = static_cast<uint8_t>(rnd());
    }
  }

  std::vector<Key5Byte> scratch(data.size());
  std::vector<uint32_t> counts(1 << 16);
  Key5Byte* result =
      RadixSort(data.data(), data.data() + data.size(), scratch.data(),
                counts.data(), 5, [](const Key5Byte& x) { return x.key; });

  std::vector<Key5Byte> sorted_data(result, result + data.size());
  std::vector<Key5Byte> std_sorted = data;
  std::sort(std_sorted.begin(), std_sorted.end(),
            [](const Key5Byte& a, const Key5Byte& b) {
              return memcmp(a.key, b.key, 5) < 0;
            });

  for (size_t i = 0; i < data.size(); ++i) {
    ASSERT_EQ(memcmp(sorted_data[i].key, std_sorted[i].key, 5), 0);
  }
}

TEST(RadixSort, Stability) {
  std::vector<TestEntry> data = {{3, 0}, {1, 1}, {4, 2}, {1, 3},
                                 {5, 4}, {9, 5}, {2, 6}, {6, 7}};
  std::vector<TestEntry> scratch(data.size());
  std::vector<uint32_t> counts(1 << 16);

  TestEntry* result =
      RadixSort(data.data(), data.data() + data.size(), scratch.data(),
                counts.data(), sizeof(uint32_t), [](const TestEntry& x) {
                  return reinterpret_cast<const uint8_t*>(&x.key);
                });

  std::vector<TestEntry> sorted_data(result, result + data.size());

  // The two entries with key 1 should maintain their original order.
  ASSERT_EQ(sorted_data[0].key, 1u);
  ASSERT_EQ(sorted_data[0].value, 1u);
  ASSERT_EQ(sorted_data[1].key, 1u);
  ASSERT_EQ(sorted_data[1].value, 3u);
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
}  // namespace perfetto::base
