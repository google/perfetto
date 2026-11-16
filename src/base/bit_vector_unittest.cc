/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "perfetto/ext/base/bit_vector.h"

#include <limits>
#include <random>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(BaseBitVectorTest, TestAndSetReturnsThePreviousValue) {
  BitVector bits;
  EXPECT_FALSE(bits.TestAndSet(5));
  EXPECT_TRUE(bits.TestAndSet(5));
  EXPECT_TRUE(bits.IsSet(5));
  EXPECT_FALSE(bits.IsSet(4));
  EXPECT_FALSE(bits.IsSet(6));
}

TEST(BaseBitVectorTest, WordBoundaries) {
  BitVector bits;
  for (size_t i : {0u, 63u, 64u, 127u, 128u}) {
    EXPECT_FALSE(bits.IsSet(i));
    EXPECT_FALSE(bits.TestAndSet(i));
    EXPECT_TRUE(bits.IsSet(i));
  }
  EXPECT_FALSE(bits.IsSet(1));
  EXPECT_FALSE(bits.IsSet(62));
  EXPECT_FALSE(bits.IsSet(65));
  EXPECT_FALSE(bits.IsSet(126));
  EXPECT_FALSE(bits.IsSet(129));
}

TEST(BaseBitVectorTest, IsSetPastTheEndDoesNotGrow) {
  BitVector bits;
  EXPECT_EQ(bits.capacity(), 0u);
  EXPECT_FALSE(bits.IsSet(1000));
  EXPECT_FALSE(bits.IsSet(std::numeric_limits<size_t>::max()));
  EXPECT_EQ(bits.capacity(), 0u);
}

TEST(BaseBitVectorTest, GrowthKeepsTheSetBits) {
  BitVector bits;
  EXPECT_FALSE(bits.TestAndSet(3));
  size_t capacity = bits.capacity();
  EXPECT_FALSE(bits.TestAndSet(capacity + 10));
  EXPECT_GT(bits.capacity(), capacity);
  EXPECT_TRUE(bits.IsSet(3));
  EXPECT_TRUE(bits.IsSet(capacity + 10));
  EXPECT_FALSE(bits.IsSet(capacity));
}

TEST(BaseBitVectorTest, ClearAllKeepsTheCapacity) {
  BitVector bits;
  EXPECT_FALSE(bits.TestAndSet(100));
  size_t capacity = bits.capacity();
  bits.ClearAll();
  EXPECT_EQ(bits.capacity(), capacity);
  EXPECT_FALSE(bits.IsSet(100));
  EXPECT_FALSE(bits.TestAndSet(100));
}

TEST(BaseBitVectorTest, ClearAllOnEmpty) {
  BitVector bits;
  bits.ClearAll();
  EXPECT_EQ(bits.capacity(), 0u);
}

TEST(BaseBitVectorTest, GrowthPreservesOldWordsAndZeroesNewWords) {
  BitVector bits;
  bits.TestAndSet(0);
  for (size_t round = 0; round < 8; ++round) {
    bits.ClearAll();
    const size_t old_capacity = bits.capacity();
    for (size_t i = 0; i < old_capacity; i += 3)
      ASSERT_FALSE(bits.TestAndSet(i));

    const size_t new_bit = old_capacity + 193;
    ASSERT_FALSE(bits.TestAndSet(new_bit));
    ASSERT_GT(bits.capacity(), new_bit);
    for (size_t i = 0; i < bits.capacity(); ++i) {
      const bool expected = (i < old_capacity && i % 3 == 0) || i == new_bit;
      ASSERT_EQ(bits.IsSet(i), expected) << "bit " << i << ", round " << round;
    }
  }
}

TEST(BaseBitVectorTest, ClearAllClearsWholeAllocationBeforeRegrowth) {
  BitVector bits;
  bits.TestAndSet(1023);
  const size_t capacity = bits.capacity();
  for (size_t i = 0; i < capacity; ++i)
    bits.TestAndSet(i);
  bits.ClearAll();
  ASSERT_EQ(bits.capacity(), capacity);
  ASSERT_FALSE(bits.TestAndSet(capacity));
  for (size_t i = 0; i < bits.capacity(); ++i)
    ASSERT_EQ(bits.IsSet(i), i == capacity) << "bit " << i;
}

TEST(BaseBitVectorTest, MatchesReferenceAcrossInsertClearAndGrow) {
  BitVector bits;
  std::vector<bool> expected(4096);
  std::minstd_rand rng(42);
  for (size_t step = 0; step < 32768; ++step) {
    const size_t i = static_cast<size_t>(rng()) % (step / 8 + 1);
    ASSERT_EQ(bits.IsSet(i), expected[i]);
    ASSERT_EQ(bits.TestAndSet(i), expected[i]);
    expected[i] = true;
    ASSERT_TRUE(bits.IsSet(i));
    ASSERT_TRUE(bits.TestAndSet(i));
    if (step % 257 == 0) {
      bits.ClearAll();
      expected.assign(expected.size(), false);
    }
  }
  for (size_t i = 0; i < expected.size(); ++i)
    ASSERT_EQ(bits.IsSet(i), expected[i]) << "bit " << i;
}

TEST(BaseBitVectorTest, ClearPreservesOtherBitsAndCapacity) {
  BitVector bits;
  bits.Clear(std::numeric_limits<size_t>::max());
  EXPECT_EQ(bits.capacity(), 0u);
  for (size_t i : {0u, 63u, 64u, 127u, 128u})
    bits.TestAndSet(i);
  const size_t capacity = bits.capacity();
  bits.Clear(64);
  bits.Clear(64);
  bits.Clear(capacity);
  bits.Clear(std::numeric_limits<size_t>::max());
  EXPECT_EQ(bits.capacity(), capacity);
  EXPECT_FALSE(bits.IsSet(64));
  for (size_t i : {0u, 63u, 127u, 128u})
    EXPECT_TRUE(bits.IsSet(i));
  EXPECT_FALSE(bits.TestAndSet(64));
}

TEST(BaseBitVectorTest, MixedOperationsMatchReference) {
  BitVector bits;
  std::vector<bool> expected(8192);
  std::minstd_rand rng(123);
  for (size_t step = 0; step < 65536; ++step) {
    const size_t i = static_cast<size_t>(rng()) % (step / 8 + 1);
    if (rng() % 3 == 0) {
      bits.Clear(i);
      expected[i] = false;
    } else {
      ASSERT_EQ(bits.TestAndSet(i), expected[i]);
      expected[i] = true;
    }
    ASSERT_EQ(bits.IsSet(i), expected[i]);
    if (step % 997 == 0) {
      bits.ClearAll();
      std::fill(expected.begin(), expected.end(), false);
    }
  }
  for (size_t i = 0; i < expected.size(); ++i)
    ASSERT_EQ(bits.IsSet(i), expected[i]) << i;
}

}  // namespace
}  // namespace base
}  // namespace perfetto
