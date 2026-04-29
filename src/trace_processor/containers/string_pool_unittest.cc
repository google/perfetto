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

#include "src/trace_processor/containers/string_pool.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {

class StringPoolTest : public testing::Test {
 protected:
  static constexpr size_t kNumBlockOffsetBits = StringPool::kNumBlockOffsetBits;
  static constexpr size_t kBlockIndexBitMask = StringPool::kBlockIndexBitMask;
  static constexpr size_t kBlockSizeBytes = StringPool::kBlockSizeBytes;
  static constexpr size_t kMinLargeStringSizeBytes =
      StringPool::kMinLargeStringSizeBytes;

  StringPool pool_;
};

namespace {

TEST_F(StringPoolTest, EmptyPool) {
  ASSERT_EQ(pool_.Get(StringPool::Id::Null()).c_str(), nullptr);

  auto it = pool_.CreateSmallStringIterator();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.StringView().c_str(), nullptr);
  ASSERT_FALSE(++it);
}

TEST_F(StringPoolTest, InternAndRetrieve) {
  static char kString[] = "Test String";
  auto id = pool_.InternString(kString);
  ASSERT_STREQ(pool_.Get(id).c_str(), kString);
  ASSERT_EQ(pool_.Get(id), kString);
  ASSERT_EQ(id, pool_.InternString(kString));
}

TEST_F(StringPoolTest, NullPointerHandling) {
  auto id = pool_.InternString(NullTermStringView());
  ASSERT_TRUE(id.is_null());
  ASSERT_EQ(pool_.Get(id).c_str(), nullptr);
}

TEST_F(StringPoolTest, Iterator) {
  auto it = pool_.CreateSmallStringIterator();
  ASSERT_TRUE(it);
  ASSERT_EQ(it.StringView().c_str(), nullptr);
  ASSERT_FALSE(++it);

  static char kString[] = "Test String";
  pool_.InternString(kString);

  it = pool_.CreateSmallStringIterator();
  ASSERT_TRUE(++it);
  ASSERT_STREQ(it.StringView().c_str(), kString);
  ASSERT_FALSE(++it);
}

TEST_F(StringPoolTest, ConstIterator) {
  static char kString[] = "Test String";
  pool_.InternString(kString);

  const StringPool& const_pool = pool_;

  auto it = const_pool.CreateSmallStringIterator();
  ASSERT_TRUE(it);
  ASSERT_TRUE(++it);
  ASSERT_STREQ(it.StringView().c_str(), kString);
  ASSERT_FALSE(++it);
}

TEST_F(StringPoolTest, StressTest) {
  // First create a buffer with 33MB of random characters, so that we insert
  // into at least two chunks.
  constexpr size_t kBufferSize = 33 * 1024 * 1024;
  std::minstd_rand0 rnd_engine(0);
  std::unique_ptr<char[]> buffer(new char[kBufferSize]);
  for (size_t i = 0; i < kBufferSize; i++)
    buffer.get()[i] = 'A' + (rnd_engine() % 26);

  // Next create strings of length 0 to 16k in length from this buffer and
  // intern them, storing their ids.
  std::multimap<StringPool::Id, base::StringView> string_map;
  constexpr uint16_t kMaxStrSize = 16u * 1024u - 1;
  for (size_t i = 0;;) {
    size_t length = static_cast<uint64_t>(rnd_engine()) % (kMaxStrSize + 1);
    if (i + length > kBufferSize)
      break;

    auto str = base::StringView(&buffer.get()[i], length);
    string_map.emplace(pool_.InternString(str), str);
    i += length;
  }

  // Finally, iterate through each string in the string pool, check that all ids
  // that match in the multimap are equal, and finish by checking we've removed
  // every item in the multimap.
  for (auto it = pool_.CreateSmallStringIterator(); it; ++it) {
    ASSERT_EQ(it.StringView(), pool_.Get(it.StringId()));

    auto it_pair = string_map.equal_range(it.StringId());
    for (auto in_it = it_pair.first; in_it != it_pair.second; ++in_it) {
      ASSERT_EQ(it.StringView(), in_it->second)
          << it.StringId().raw_id() << ": " << it.StringView().Hash() << " vs "
          << in_it->second.Hash();
    }
    string_map.erase(it_pair.first, it_pair.second);
  }
  ASSERT_EQ(string_map.size(), 0u);
}

TEST_F(StringPoolTest, LargeString) {
  // Would not fit into a block at all, so has to go into |large_strings_|.
  constexpr size_t kEnormousStringSize = 33 * 1024 * 1024;

  constexpr std::array<size_t, 1> kStringSizes = {
      kEnormousStringSize,  // large strings
  };

  std::array<std::unique_ptr<char[]>, kStringSizes.size()> big_strings;
  for (size_t i = 0; i < big_strings.size(); i++) {
    big_strings[i].reset(new char[kStringSizes[i] + 1]);
    for (size_t j = 0; j < kStringSizes[i]; j++) {
      big_strings[i].get()[j] = 'A' + static_cast<char>((j + i) % 26);
    }
    big_strings[i].get()[kStringSizes[i]] = '\0';
  }

  std::array<StringPool::Id, kStringSizes.size()> string_ids;
  for (size_t i = 0; i < big_strings.size(); i++) {
    string_ids[i] = pool_.InternString(
        base::StringView(big_strings[i].get(), kStringSizes[i]));
    // Interning it a second time should return the original id.
    ASSERT_EQ(string_ids[i], pool_.InternString(base::StringView(
                                 big_strings[i].get(), kStringSizes[i])));
  }

  ASSERT_TRUE(string_ids[0].is_large_string());
  for (size_t i = 0; i < big_strings.size(); i++) {
    ASSERT_EQ(big_strings[i].get(), pool_.Get(string_ids[i]));
  }
}

TEST_F(StringPoolTest, MaxSmallStringIdOnBlockBoundary) {
  // Null string should be at (0, 0).
  pool_.InternString(base::StringView());

  static constexpr uint32_t kMetadataSize = 5;
  static constexpr uint32_t kNullStringSize = 5;
  pool_.InternString(base::StringView(
      std::string(1048576 - kMetadataSize - kNullStringSize, 'a')));
  pool_.InternString(
      base::StringView(std::string(1048576 - kMetadataSize, 'b')));
  pool_.InternString(
      base::StringView(std::string(1048576 - kMetadataSize, 'c')));
  pool_.InternString(
      base::StringView(std::string(1048576 - kMetadataSize, 'd')));

  // The max id should point to the *next* block.
  StringPool::Id max_id = pool_.MaxSmallStringId();
  ASSERT_EQ(max_id.block_index(), 1u);
  ASSERT_EQ(max_id.block_offset(), 0u);
}

// Stress test for the `EnableThreadSafetyForMultiConnection` lock toggle.
// Spawns several threads each interning a mix of distinct and shared strings
// concurrently and asserts (a) no crashes, (b) every shared string round-trips
// to the same `StringId` regardless of which thread interned it first, and
// (c) every distinct string is recoverable via `Get(id)`.
TEST_F(StringPoolTest, ConcurrentInternIsThreadSafe) {
  pool_.EnableThreadSafetyForMultiConnection();

  constexpr int kThreads = 8;
  constexpr int kItersPerThread = 2000;
  constexpr int kSharedStrings = 32;

  std::vector<std::string> shared;
  shared.reserve(kSharedStrings);
  for (int i = 0; i < kSharedStrings; ++i) {
    shared.push_back("shared_" + std::to_string(i));
  }

  std::vector<std::vector<StringPool::Id>> shared_ids(
      static_cast<size_t>(kThreads));
  std::vector<std::vector<std::pair<std::string, StringPool::Id>>>
      distinct_pairs(static_cast<size_t>(kThreads));

  auto worker = [&](size_t tag) {
    auto& sids = shared_ids[tag];
    auto& dp = distinct_pairs[tag];
    sids.reserve(static_cast<size_t>(kItersPerThread));
    dp.reserve(static_cast<size_t>(kItersPerThread));
    for (size_t i = 0; i < static_cast<size_t>(kItersPerThread); ++i) {
      // Mix of dedup-hits (shared bucket) and unique strings (writer path).
      const std::string& s = shared[i % shared.size()];
      sids.push_back(pool_.InternString(base::StringView(s)));

      std::string unique =
          "t" + std::to_string(tag) + "_i" + std::to_string(i);
      dp.emplace_back(unique,
                      pool_.InternString(base::StringView(unique)));
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(kThreads));
  for (size_t i = 0; i < static_cast<size_t>(kThreads); ++i) {
    threads.emplace_back(worker, i);
  }
  for (auto& t : threads) {
    t.join();
  }

  // All threads must have agreed on the canonical Id for each shared string.
  for (size_t s = 0; s < static_cast<size_t>(kSharedStrings); ++s) {
    StringPool::Id canonical = shared_ids[0][s];
    for (size_t t = 0; t < static_cast<size_t>(kThreads); ++t) {
      for (size_t i = s; i < shared_ids[t].size();
           i += static_cast<size_t>(kSharedStrings)) {
        ASSERT_EQ(shared_ids[t][i], canonical)
            << "shared string '" << shared[s]
            << "' got different ids across threads";
      }
    }
    ASSERT_STREQ(pool_.Get(canonical).c_str(), shared[s].c_str());
  }

  // Every distinct string must round-trip via Get(Id).
  std::unordered_set<uint32_t> seen_ids;
  for (size_t t = 0; t < static_cast<size_t>(kThreads); ++t) {
    for (auto& [str, id] : distinct_pairs[t]) {
      ASSERT_STREQ(pool_.Get(id).c_str(), str.c_str());
      // Unique strings should map to unique ids.
      ASSERT_TRUE(seen_ids.insert(id.raw_id()).second)
          << "duplicate id for distinct string '" << str << "'";
    }
  }
}

}  // namespace
}  // namespace perfetto::trace_processor
