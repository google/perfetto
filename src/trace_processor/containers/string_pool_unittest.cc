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
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/util/concurrency_stress_test_util.h"
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

// Deterministic strings of varied lengths. Every 500th is large enough for the
// |large_strings_| path.
std::vector<std::string> MakeVariedStrings(size_t count, size_t large_size) {
  std::minstd_rand0 rnd_engine(0);
  std::vector<std::string> strings;
  strings.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    size_t length;
    if (i % 500 == 499) {
      length = large_size + (rnd_engine() % 4096);
    } else {
      length = rnd_engine() % 256;
    }
    std::string s;
    s.reserve(length);
    for (size_t j = 0; j < length; ++j)
      s.push_back(static_cast<char>('A' + (rnd_engine() % 26)));
    // The index prefix keeps even tiny strings unique.
    strings.push_back(std::to_string(i) + ":" + s);
  }
  return strings;
}

// Reads of an otherwise immutable pool must be safe from many threads.
TEST_F(StringPoolTest, ConcurrentReadsNoInterns) {
  pool_.set_locking(true);

  constexpr size_t kNumStrings = 4000;
  std::vector<std::string> strings =
      MakeVariedStrings(kNumStrings, kMinLargeStringSizeBytes);
  std::vector<StringPool::Id> ids;
  ids.reserve(kNumStrings);
  for (const auto& s : strings)
    ids.push_back(pool_.InternString(base::StringView(s)));

  ConcurrentlyRun(8, 16, [&](uint32_t thread_idx) {
    // Each thread starts at a different offset so the reads spread across
    // blocks and large strings at once.
    for (size_t i = 0; i < kNumStrings; ++i) {
      size_t k = (i + thread_idx * 257) % kNumStrings;
      const std::string& expected = strings[k];
      NullTermStringView got = pool_.Get(ids[k]);
      PERFETTO_CHECK(got == base::StringView(expected));

      std::optional<StringPool::Id> looked_up =
          pool_.GetId(base::StringView(expected));
      PERFETTO_CHECK(looked_up.has_value());
      PERFETTO_CHECK(*looked_up == ids[k]);
    }
  });
}

// Get() of a published id is lock-free. Concurrent interns, which allocate new
// blocks and append large strings under the lock, must not corrupt it.
TEST_F(StringPoolTest, ConcurrentReadsWhileLockedInterns) {
  pool_.set_locking(true);

  constexpr size_t kNumBaseStrings = 4000;
  std::vector<std::string> base_strings =
      MakeVariedStrings(kNumBaseStrings, kMinLargeStringSizeBytes);
  std::vector<StringPool::Id> base_ids;
  base_ids.reserve(kNumBaseStrings);
  for (const auto& s : base_strings)
    base_ids.push_back(pool_.InternString(base::StringView(s)));

  constexpr size_t kReadWindow = 128;
  constexpr uint32_t kThreadCount = 8;
  constexpr uint32_t kIterations = 2000;
  ConcurrentlyRun(kThreadCount, kIterations, [&](uint32_t thread_idx) {
    // Half the threads intern new strings, the other half read existing ids.
    if (thread_idx % 2 == 0) {
      static std::atomic<uint64_t> counter{0};
      uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
      // 4 writers x kIterations x ~2KB is ~16MB, so several new 4MB blocks get
      // allocated while the readers run. Every 1000th string is a large one.
      std::string s = "writer-" + std::to_string(thread_idx) + "-" +
                      std::to_string(n) + ":";
      s.append(n % 1000 == 999 ? kMinLargeStringSizeBytes + 1 : 2048, 'x');
      pool_.InternString(base::StringView(s));
    } else {
      uint64_t base = static_cast<uint64_t>(thread_idx) * 257;
      for (size_t i = 0; i < kReadWindow; ++i) {
        size_t k = (base + i) % kNumBaseStrings;
        NullTermStringView got = pool_.Get(base_ids[k]);
        PERFETTO_CHECK(got == base::StringView(base_strings[k]));
      }
    }
  });

  // Every base id must still resolve after the churn.
  for (size_t i = 0; i < kNumBaseStrings; ++i)
    ASSERT_EQ(pool_.Get(base_ids[i]), base::StringView(base_strings[i]));

  // The writers must actually have forced block growth for this to mean much.
  ASSERT_GT(pool_.MaxSmallStringId().block_index(), 0u);
}

}  // namespace
}  // namespace perfetto::trace_processor
