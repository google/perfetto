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

#include "src/trace_processor/dataframe/impl/bit_vector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe::impl {
namespace {

// Test that the default constructor creates an empty bit vector
TEST(BitVectorTest, DefaultConstructor) {
  BitVector bits;
  EXPECT_EQ(bits.size(), 0u);
}

TEST(BitVectorTest, CreateWithSize) {
  {
    auto bits = BitVector::CreateWithSize(31);
    EXPECT_EQ(bits.size(), 31u);
    for (size_t i = 0; i < 31; ++i) {
      EXPECT_FALSE(bits.is_set(i)) << "Bit " << i << " should be unset";
    }
  }
  {
    auto bits = BitVector::CreateWithSize(65, true);
    EXPECT_EQ(bits.size(), 65u);
    for (size_t i = 0; i < 65; ++i) {
      EXPECT_TRUE(bits.is_set(i)) << "Bit " << i << " should be set";
    }
  }
}

// Test push_back functionality
TEST(BitVectorTest, PushBack) {
  BitVector bits;

  // Size should start at 0
  EXPECT_EQ(bits.size(), 0u);

  // Add some bits
  bits.push_back(true);   // bit 0
  bits.push_back(false);  // bit 1
  bits.push_back(true);   // bit 2

  EXPECT_EQ(bits.size(), 3u);

  EXPECT_TRUE(bits.is_set(0));
  EXPECT_FALSE(bits.is_set(1));
  EXPECT_TRUE(bits.is_set(2));

  // Add more bits to cross word boundaries (assuming 64-bit words)
  for (size_t i = 3; i < 64; ++i) {
    bits.push_back(false);
  }

  // Add bits that go into a second word
  bits.push_back(true);   // bit 64
  bits.push_back(false);  // bit 65
  bits.push_back(true);   // bit 66

  EXPECT_EQ(bits.size(), 67u);

  EXPECT_TRUE(bits.is_set(64));
  EXPECT_FALSE(bits.is_set(65));
  EXPECT_TRUE(bits.is_set(66));
}

// Test set, clear, and is_set methods
TEST(BitVectorTest, SetClearAndIsSet) {
  BitVector bits;

  // First initialize bits (since CreateWithCapacity only sets capacity)
  for (size_t i = 0; i < 200; ++i) {
    bits.push_back(false);
  }

  // Initially all bits should be unset (we pushed all false values)
  for (size_t i = 0; i < 200; ++i) {
    EXPECT_FALSE(bits.is_set(i));
  }

  // Set specific bits including across word boundaries
  bits.set(5);
  bits.set(63);   // Last bit of first word
  bits.set(64);   // First bit of second word
  bits.set(127);  // Last bit of second word
  bits.set(128);  // First bit of third word
  bits.set(198);

  // Check set bits are set
  EXPECT_TRUE(bits.is_set(5));
  EXPECT_TRUE(bits.is_set(63));
  EXPECT_TRUE(bits.is_set(64));
  EXPECT_TRUE(bits.is_set(127));
  EXPECT_TRUE(bits.is_set(128));
  EXPECT_TRUE(bits.is_set(198));

  // Check some unset bits remain unset
  EXPECT_FALSE(bits.is_set(0));
  EXPECT_FALSE(bits.is_set(6));
  EXPECT_FALSE(bits.is_set(65));
  EXPECT_FALSE(bits.is_set(199));

  // Clear some of the set bits
  bits.clear(5);
  bits.clear(64);
  bits.clear(198);

  // Verify cleared bits are now unset
  EXPECT_FALSE(bits.is_set(5));
  EXPECT_TRUE(bits.is_set(63));
  EXPECT_FALSE(bits.is_set(64));
  EXPECT_TRUE(bits.is_set(127));
  EXPECT_TRUE(bits.is_set(128));
  EXPECT_FALSE(bits.is_set(198));
}

// Test the change method
TEST(BitVectorTest, ChangeMethod) {
  BitVector bits;

  // First initialize bits (since CreateWithCapacity only sets capacity)
  for (size_t i = 0; i < 100; ++i) {
    bits.push_back(false);
  }

  // Set some bits first
  bits.set(10);
  bits.set(20);
  bits.set(30);

  // Change them with different transitions
  bits.change(10, false);  // true -> false
  bits.change(20, true);   // true -> true (no change)
  bits.change(30, false);  // true -> false
  bits.change(40, true);   // false -> true

  // Verify changes took effect
  EXPECT_FALSE(bits.is_set(10));
  EXPECT_TRUE(bits.is_set(20));
  EXPECT_FALSE(bits.is_set(30));
  EXPECT_TRUE(bits.is_set(40));
}

// Test the change_assume_unset method
TEST(BitVectorTest, ChangeAssumeUnsetMethod) {
  BitVector bits;

  // First initialize bits (since CreateWithCapacity only sets capacity)
  for (size_t i = 0; i < 100; ++i) {
    bits.push_back(false);
  }

  // Use change_assume_unset on unset bits
  bits.change_assume_unset(15, true);
  bits.change_assume_unset(25, false);  // No change since value is false
  bits.change_assume_unset(35, true);

  // Verify changes
  EXPECT_TRUE(bits.is_set(15));
  EXPECT_FALSE(bits.is_set(25));
  EXPECT_TRUE(bits.is_set(35));
}

// Test the set_bits_until_in_word method
TEST(BitVectorTest, SetBitsUntilInWord) {
  BitVector bits;

  // First initialize bits (since CreateWithCapacity only sets capacity)
  for (size_t i = 0; i < 130; ++i) {  // Only need 130 for the test
    bits.push_back(false);
  }

  // Set bits in first word
  bits.set(1);
  bits.set(3);
  bits.set(7);
  bits.set(20);

  // Check counts in first word
  EXPECT_EQ(bits.count_set_bits_until_in_word(0),
            0u);  // No bits before position 0
  EXPECT_EQ(bits.count_set_bits_until_in_word(1),
            0u);  // The bit at position 1 itself isn't counted
  EXPECT_EQ(bits.count_set_bits_until_in_word(2), 1u);  // Only bit 1 is counted
  EXPECT_EQ(bits.count_set_bits_until_in_word(4), 2u);  // Bits 1 and 3 counted
  EXPECT_EQ(bits.count_set_bits_until_in_word(10),
            3u);  // Bits 1, 3, and 7 counted
  EXPECT_EQ(bits.count_set_bits_until_in_word(21),
            4u);  // Bits 1, 3, 7, and 20 counted

  // Set bits in second word
  bits.set(64);  // First bit of second word
  bits.set(70);  // Another bit in second word

  // Check counts in second word - this should start from 0 again
  EXPECT_EQ(bits.count_set_bits_until_in_word(64),
            0u);  // No bits before position 64 in this word
  EXPECT_EQ(bits.count_set_bits_until_in_word(65),
            1u);  // Just bit 64 counted in this word
  EXPECT_EQ(bits.count_set_bits_until_in_word(71),
            2u);  // Bits 64 and 70 counted in this word
}

// Test the PackLeft method
TEST(BitVectorTest, PackLeftMethod) {
  BitVector bits;

  // First initialize bits (since CreateWithCapacity only sets capacity)
  for (size_t i = 0; i < 20; ++i) {
    bits.push_back(false);
  }

  // Set every other bit
  for (size_t i = 0; i < 20; i += 2) {
    bits.set(i);
  }

  // Create source array with indices 0-19
  uint32_t source[20];
  for (uint32_t i = 0; i < 20; ++i) {
    source[i] = i;
  }

  // Create target array
  uint32_t target[20] = {0};

  // Filter with default behavior (keep set bits)
  uint32_t* end = bits.PackLeft(source, source + 20, target);

  // Should keep 10 elements (those with even indices where bits are set)
  EXPECT_EQ(end - target, 10);

  // Check the filtered values
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(target[i], i * 2);
  }

  // Reset target
  std::fill_n(target, 20, 0);

  // Filter with inverted behavior (keep unset bits)
  end = bits.PackLeft<true>(source, source + 20, target);

  // Should keep 10 elements (those with odd indices where bits are not set)
  EXPECT_EQ(end - target, 10);

  // Check the filtered values
  for (size_t i = 0; i < 10; ++i) {
    EXPECT_EQ(target[i], i * 2 + 1);
  }
}

// Test the PrefixPopcount method
TEST(BitVectorTest, PrefixPopcountMethod) {
  BitVector bits;

  // First initialize bits (since CreateWithCapacity only sets capacity)
  for (size_t i = 0; i < 200; ++i) {
    bits.push_back(false);
  }

  // First word (0-63): set 3 bits
  bits.set(5);
  bits.set(20);
  bits.set(40);

  // Second word (64-127): set 2 bits
  bits.set(70);
  bits.set(100);

  // Third word (128-191): set 4 bits
  bits.set(130);
  bits.set(140);
  bits.set(150);
  bits.set(160);

  // Get the prefix popcounts
  auto prefixes = bits.PrefixPopcount();

  // Check size - should be one entry per word
  EXPECT_EQ(prefixes.size(), (bits.size() + 63) / 64);

  // Check values
  EXPECT_EQ(prefixes[0], 0u);  // No words before the first word
  EXPECT_EQ(prefixes[1], 3u);  // First word had 3 set bits
  EXPECT_EQ(prefixes[2], 5u);  // First+Second word had 3+2=5 set bits
  EXPECT_EQ(prefixes[3], 9u);  // First+Second+Third had 3+2+4=9 set bits
}

// Test across word boundaries
TEST(BitVectorTest, WordBoundaries) {
  BitVector bits;

  // First initialize bits (since CreateWithCapacity only sets capacity)
  for (size_t i = 0; i < 256; ++i) {
    bits.push_back(false);
  }

  // Set bits at word boundaries
  bits.set(0);    // First bit of first word
  bits.set(63);   // Last bit of first word
  bits.set(64);   // First bit of second word
  bits.set(127);  // Last bit of second word
  bits.set(128);  // First bit of third word
  bits.set(191);  // Last bit of third word
  bits.set(192);  // First bit of fourth word
  bits.set(255);  // Last bit of fourth word

  // Check all bits are correctly set
  EXPECT_TRUE(bits.is_set(0));
  EXPECT_TRUE(bits.is_set(63));
  EXPECT_TRUE(bits.is_set(64));
  EXPECT_TRUE(bits.is_set(127));
  EXPECT_TRUE(bits.is_set(128));
  EXPECT_TRUE(bits.is_set(191));
  EXPECT_TRUE(bits.is_set(192));
  EXPECT_TRUE(bits.is_set(255));

  // Clear some boundary bits
  bits.clear(0);
  bits.clear(64);
  bits.clear(128);
  bits.clear(192);

  // Check cleared bits are unset and others remained set
  EXPECT_FALSE(bits.is_set(0));
  EXPECT_TRUE(bits.is_set(63));
  EXPECT_FALSE(bits.is_set(64));
  EXPECT_TRUE(bits.is_set(127));
  EXPECT_FALSE(bits.is_set(128));
  EXPECT_TRUE(bits.is_set(191));
  EXPECT_FALSE(bits.is_set(192));
  EXPECT_TRUE(bits.is_set(255));
}

// Test with many bits
TEST(BitVectorTest, LargeVector) {
  // Create a bit vector with capacity for 8192 bits (power of two bits)
  BitVector bits;
  EXPECT_EQ(bits.size(), 0u);  // Size starts at 0

  // Initialize bits
  for (size_t i = 0; i < 8192; ++i) {
    bits.push_back(false);
  }

  EXPECT_EQ(bits.size(), 8192u);

  // Set every 1000th bit
  for (size_t i = 0; i < 8192; i += 1000) {
    bits.set(i);
  }

  // Verify the correct bits are set
  for (size_t i = 0; i < 8192; ++i) {
    if (i % 1000 == 0) {
      EXPECT_TRUE(bits.is_set(i)) << "Bit " << i << " should be set";
    } else {
      EXPECT_FALSE(bits.is_set(i)) << "Bit " << i << " should be unset";
    }
  }
}

// Test a mixed sequence of operations
TEST(BitVectorTest, MixedOperations) {
  BitVector bits;

  EXPECT_EQ(bits.size(), 0u);  // Initial size should be 0

  // First initialize 100 bits (since CreateWithCapacity only sets capacity)
  for (size_t i = 0; i < 100; ++i) {
    bits.push_back(false);
  }

  // Add some more bits
  bits.push_back(true);
  bits.push_back(false);
  bits.push_back(true);

  // Current size should be 103
  EXPECT_EQ(bits.size(), 103u);

  // Set, clear, and modify bits
  bits.set(5);
  bits.set(50);
  bits.clear(50);
  bits.change(5, false);
  bits.change(10, true);
  bits.change_assume_unset(20, true);

  // Check final state
  EXPECT_FALSE(bits.is_set(5));
  EXPECT_FALSE(bits.is_set(50));
  EXPECT_TRUE(bits.is_set(10));
  EXPECT_TRUE(bits.is_set(20));
  EXPECT_TRUE(bits.is_set(100));
  EXPECT_FALSE(bits.is_set(101));
  EXPECT_TRUE(bits.is_set(102));
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe::impl
