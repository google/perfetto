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

#include "src/trace_processor/containers/bit_vector.h"

#include <bitset>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace_processor/serialization.pbzero.h"

namespace perfetto::trace_processor {
namespace {
using testing::ElementsAre;
using testing::IsEmpty;
using testing::UnorderedElementsAre;

TEST(BitVectorUnittest, CreateAllTrue) {
  BitVector bv(2049, true);

  // Ensure that a selection of interesting bits are set.
  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_TRUE(bv.IsSet(1));
  ASSERT_TRUE(bv.IsSet(511));
  ASSERT_TRUE(bv.IsSet(512));
  ASSERT_TRUE(bv.IsSet(2047));
  ASSERT_TRUE(bv.IsSet(2048));
}

TEST(BitVectorUnittest, CreateAllFalse) {
  BitVector bv(2049, false);

  // Ensure that a selection of interesting bits are cleared.
  ASSERT_FALSE(bv.IsSet(0));
  ASSERT_FALSE(bv.IsSet(1));
  ASSERT_FALSE(bv.IsSet(511));
  ASSERT_FALSE(bv.IsSet(512));
  ASSERT_FALSE(bv.IsSet(2047));
  ASSERT_FALSE(bv.IsSet(2048));
}

TEST(BitVectorUnittest, Set) {
  BitVector bv(2049, false);
  bv.Set(0);
  bv.Set(1);
  bv.Set(511);
  bv.Set(512);
  bv.Set(2047);

  // Ensure the bits we touched are set.
  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_TRUE(bv.IsSet(1));
  ASSERT_TRUE(bv.IsSet(511));
  ASSERT_TRUE(bv.IsSet(512));
  ASSERT_TRUE(bv.IsSet(2047));

  // Ensure that a selection of other interestinng bits are
  // still cleared.
  ASSERT_FALSE(bv.IsSet(2));
  ASSERT_FALSE(bv.IsSet(63));
  ASSERT_FALSE(bv.IsSet(64));
  ASSERT_FALSE(bv.IsSet(510));
  ASSERT_FALSE(bv.IsSet(513));
  ASSERT_FALSE(bv.IsSet(1023));
  ASSERT_FALSE(bv.IsSet(1024));
  ASSERT_FALSE(bv.IsSet(2046));
  ASSERT_FALSE(bv.IsSet(2048));
  ASSERT_FALSE(bv.IsSet(2048));
}

TEST(BitVectorUnittest, Clear) {
  BitVector bv(2049, true);
  bv.Clear(0);
  bv.Clear(1);
  bv.Clear(511);
  bv.Clear(512);
  bv.Clear(2047);

  // Ensure the bits we touched are cleared.
  ASSERT_FALSE(bv.IsSet(0));
  ASSERT_FALSE(bv.IsSet(1));
  ASSERT_FALSE(bv.IsSet(511));
  ASSERT_FALSE(bv.IsSet(512));
  ASSERT_FALSE(bv.IsSet(2047));

  // Ensure that a selection of other interestinng bits are
  // still set.
  ASSERT_TRUE(bv.IsSet(2));
  ASSERT_TRUE(bv.IsSet(63));
  ASSERT_TRUE(bv.IsSet(64));
  ASSERT_TRUE(bv.IsSet(510));
  ASSERT_TRUE(bv.IsSet(513));
  ASSERT_TRUE(bv.IsSet(1023));
  ASSERT_TRUE(bv.IsSet(1024));
  ASSERT_TRUE(bv.IsSet(2046));
  ASSERT_TRUE(bv.IsSet(2048));
}

TEST(BitVectorUnittest, AppendToEmpty) {
  BitVector bv;
  bv.AppendTrue();
  bv.AppendFalse();

  ASSERT_EQ(bv.size(), 2u);
  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_FALSE(bv.IsSet(1));
}

TEST(BitVectorUnittest, AppendToExisting) {
  BitVector bv(2046, false);
  bv.AppendTrue();
  bv.AppendFalse();
  bv.AppendTrue();
  bv.AppendTrue();

  ASSERT_EQ(bv.size(), 2050u);
  ASSERT_TRUE(bv.IsSet(2046));
  ASSERT_FALSE(bv.IsSet(2047));
  ASSERT_TRUE(bv.IsSet(2048));
  ASSERT_TRUE(bv.IsSet(2049));
}

TEST(BitVectorUnittest, CountSetBits) {
  BitVector bv(2049, false);
  bv.Set(0);
  bv.Set(1);
  bv.Set(511);
  bv.Set(512);
  bv.Set(2047);
  bv.Set(2048);

  ASSERT_EQ(bv.CountSetBits(), 6u);

  ASSERT_EQ(bv.CountSetBits(0), 0u);
  ASSERT_EQ(bv.CountSetBits(1), 1u);
  ASSERT_EQ(bv.CountSetBits(2), 2u);
  ASSERT_EQ(bv.CountSetBits(3), 2u);
  ASSERT_EQ(bv.CountSetBits(511), 2u);
  ASSERT_EQ(bv.CountSetBits(512), 3u);
  ASSERT_EQ(bv.CountSetBits(1023), 4u);
  ASSERT_EQ(bv.CountSetBits(1024), 4u);
  ASSERT_EQ(bv.CountSetBits(2047), 4u);
  ASSERT_EQ(bv.CountSetBits(2048), 5u);
  ASSERT_EQ(bv.CountSetBits(2049), 6u);
}

TEST(BitVectorUnittest, IndexOfNthSet) {
  BitVector bv(2050, false);
  bv.Set(0);
  bv.Set(1);
  bv.Set(511);
  bv.Set(512);
  bv.Set(2047);
  bv.Set(2048);

  ASSERT_EQ(bv.IndexOfNthSet(0), 0u);
  ASSERT_EQ(bv.IndexOfNthSet(1), 1u);
  ASSERT_EQ(bv.IndexOfNthSet(2), 511u);
  ASSERT_EQ(bv.IndexOfNthSet(3), 512u);
  ASSERT_EQ(bv.IndexOfNthSet(4), 2047u);
  ASSERT_EQ(bv.IndexOfNthSet(5), 2048u);
}

TEST(BitVectorUnittest, Resize) {
  BitVector bv(1, false);

  bv.Resize(2, true);
  ASSERT_EQ(bv.size(), 2u);
  ASSERT_EQ(bv.IsSet(1), true);

  bv.Resize(2049, false);
  ASSERT_EQ(bv.size(), 2049u);
  ASSERT_EQ(bv.IsSet(2), false);
  ASSERT_EQ(bv.IsSet(2047), false);
  ASSERT_EQ(bv.IsSet(2048), false);

  // Set these two bits; the first should be preserved and the
  // second should disappear.
  bv.Set(512);
  bv.Set(513);

  bv.Resize(513, false);
  ASSERT_EQ(bv.size(), 513u);
  ASSERT_EQ(bv.IsSet(1), true);
  ASSERT_EQ(bv.IsSet(512), true);
  ASSERT_EQ(bv.CountSetBits(), 2u);

  // When we resize up, we need to be sure that the set bit from
  // before we resized down is not still present as a garbage bit.
  bv.Resize(514, false);
  ASSERT_EQ(bv.size(), 514u);
  ASSERT_EQ(bv.IsSet(513), false);
  ASSERT_EQ(bv.CountSetBits(), 2u);
}

TEST(BitVectorUnittest, ResizeHasCorrectCount) {
  BitVector bv(1, false);
  ASSERT_EQ(bv.CountSetBits(), 0u);

  bv.Resize(1024, true);
  ASSERT_EQ(bv.CountSetBits(), 1023u);
}

TEST(BitVectorUnittest, AppendAfterResizeDown) {
  BitVector bv(2049, false);
  bv.Set(2048);
  ASSERT_TRUE(bv.IsSet(2048));
  bv.Resize(2048);
  ASSERT_EQ(bv.size(), 2048u);
  bv.AppendFalse();
  ASSERT_EQ(bv.size(), 2049u);
  ASSERT_FALSE(bv.IsSet(2048));
  ASSERT_EQ(bv.CountSetBits(), 0u);
}

TEST(BitVectorUnittest, UpdateSetBits) {
  BitVector bv(6, false);
  bv.Set(1);
  bv.Set(2);
  bv.Set(4);

  BitVector picker(3u, true);
  picker.Clear(1);

  bv.UpdateSetBits(picker);

  ASSERT_TRUE(bv.IsSet(1));
  ASSERT_FALSE(bv.IsSet(2));
  ASSERT_TRUE(bv.IsSet(4));
}

TEST(BitVectorUnittest, UpdateSetBitsSmallerPicker) {
  BitVector bv(6, false);
  bv.Set(1);
  bv.Set(2);
  bv.Set(4);

  BitVector picker(2u, true);
  picker.Clear(1);

  bv.UpdateSetBits(picker);

  ASSERT_TRUE(bv.IsSet(1));
  ASSERT_FALSE(bv.IsSet(2));
  ASSERT_FALSE(bv.IsSet(4));
}

TEST(BitVectorUnittest, UpdateSetBitsWordBoundary) {
  BitVector bv(65, true);

  BitVector picker(65u, true);
  picker.Clear(64);

  bv.UpdateSetBits(picker);

  ASSERT_FALSE(bv.IsSet(64));
}

TEST(BitVectorUnittest, UpdateSetBitsStress) {
  static constexpr uint32_t kCount = 21903;
  std::minstd_rand0 rand;

  BitVector bv;
  std::bitset<kCount> bv_std_lib;
  for (uint32_t i = 0; i < kCount; ++i) {
    bool res = rand() % 2u;
    if (res) {
      bv.AppendTrue();
    } else {
      bv.AppendFalse();
    }
    bv_std_lib[i] = res;
  }

  BitVector picker;
  for (uint32_t i = 0; i < bv_std_lib.count(); ++i) {
    bool res = rand() % 2u;
    if (res) {
      picker.AppendTrue();
    } else {
      picker.AppendFalse();
    }
  }
  bv.UpdateSetBits(picker);

  ASSERT_EQ(bv.size(), kCount);

  uint32_t set_bit_i = 0;
  for (uint32_t i = 0; i < kCount; ++i) {
    if (bv_std_lib.test(i)) {
      ASSERT_EQ(bv.IsSet(i), picker.IsSet(set_bit_i++));
    } else {
      ASSERT_FALSE(bv.IsSet(i));
    }
  }
}

TEST(BitVectorUnittest, SelectBitsSimple) {
  BitVector bv = {true, false, true, false, true, true, true};
  BitVector mask = {true, false, true, true, false, false, true};
  bv.SelectBits(mask);

  ASSERT_EQ(bv.size(), 4u);
  ASSERT_EQ(bv.IsSet(0), true);
  ASSERT_EQ(bv.IsSet(1), true);
  ASSERT_EQ(bv.IsSet(2), false);
  ASSERT_EQ(bv.IsSet(3), true);
  ASSERT_EQ(bv.CountSetBits(), 3u);
}

TEST(BitVectorUnittest, SelectBitsSmallerMain) {
  BitVector bv = {true, false, true, false};
  BitVector mask = {true, false, true, true, false, false, true};
  bv.SelectBits(mask);

  ASSERT_EQ(bv.size(), 3u);
  ASSERT_EQ(bv.IsSet(0), true);
  ASSERT_EQ(bv.IsSet(1), true);
  ASSERT_EQ(bv.IsSet(2), false);
  ASSERT_EQ(bv.CountSetBits(), 2u);
}

TEST(BitVectorUnittest, SelectBitsLarge) {
  BitVector bv = BitVector::RangeForTesting(
      0, 813, [](uint32_t idx) { return idx % 7 == 0; });
  BitVector mask = BitVector::RangeForTesting(
      0, 813, [](uint32_t idx) { return idx % 3 == 0; });
  bv.SelectBits(mask);

  BitVector expected = BitVector::RangeForTesting(
      0, 271u, [](uint32_t idx) { return (idx * 3) % 7 == 0; });

  ASSERT_EQ(bv.size(), 271u);
  for (uint32_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(expected.IsSet(i), bv.IsSet(i)) << "Index " << i;
    ASSERT_EQ(expected.CountSetBits(i), bv.CountSetBits(i)) << "Index " << i;
  }
  ASSERT_EQ(expected.CountSetBits(), bv.CountSetBits());
}

TEST(BitVectorUnittest, SelectBitsLargeSmallerMain) {
  BitVector bv = BitVector::RangeForTesting(
      0, 279, [](uint32_t idx) { return idx % 7 == 0; });
  BitVector mask = BitVector::RangeForTesting(
      0, 813, [](uint32_t idx) { return idx % 3 == 0; });
  bv.SelectBits(mask);

  BitVector expected = BitVector::RangeForTesting(
      0, 93, [](uint32_t idx) { return (idx * 3) % 7 == 0; });

  ASSERT_EQ(bv.size(), 93u);
  for (uint32_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(expected.IsSet(i), bv.IsSet(i)) << "Index " << i;
    ASSERT_EQ(expected.CountSetBits(i), bv.CountSetBits(i)) << "Index " << i;
  }
  ASSERT_EQ(expected.CountSetBits(), bv.CountSetBits());
}

TEST(BitVectorUnittest, SelectBitsDense) {
  BitVector bv =
      BitVector::RangeForTesting(0, 279, [](uint32_t) { return true; });
  BitVector mask =
      BitVector::RangeForTesting(0, 279, [](uint32_t idx) { return idx < 80; });
  bv.SelectBits(mask);

  BitVector expected =
      BitVector::RangeForTesting(0, 80, [](uint32_t) { return true; });

  ASSERT_EQ(bv.size(), 80u);
  for (uint32_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(expected.IsSet(i), bv.IsSet(i)) << "Index " << i;
    ASSERT_EQ(expected.CountSetBits(i), bv.CountSetBits(i)) << "Index " << i;
  }
  ASSERT_EQ(expected.CountSetBits(), bv.CountSetBits());
}

TEST(BitVectorUnittest, SelectBitsEnd) {
  BitVector bv = BitVector::RangeForTesting(
      0, 279, [](uint32_t idx) { return idx % 7 == 0; });
  BitVector mask = BitVector::RangeForTesting(
      0, 813, [](uint32_t idx) { return idx % 3 == 0; });
  bv.SelectBits(mask);

  BitVector expected = BitVector::RangeForTesting(
      0, 93, [](uint32_t idx) { return (idx * 3) % 7 == 0; });

  ASSERT_EQ(bv.size(), 93u);
  for (uint32_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(expected.IsSet(i), bv.IsSet(i)) << "Index " << i;
    ASSERT_EQ(expected.CountSetBits(i), bv.CountSetBits(i)) << "Index " << i;
  }
  ASSERT_EQ(expected.CountSetBits(), bv.CountSetBits());
}

TEST(BitVectorUnittest, SelectBitsOob) {
  BitVector bv = BitVector::RangeForTesting(
      0, 512, [](uint32_t idx) { return idx % 7 == 0; });
  BitVector mask = BitVector(512, true);
  bv.SelectBits(mask);

  BitVector expected = BitVector::RangeForTesting(
      0, 512, [](uint32_t idx) { return idx % 7 == 0; });

  ASSERT_EQ(bv.size(), 512u);
  for (uint32_t i = 0; i < expected.size(); ++i) {
    ASSERT_EQ(expected.IsSet(i), bv.IsSet(i)) << "Index " << i;
    ASSERT_EQ(expected.CountSetBits(i), bv.CountSetBits(i)) << "Index " << i;
  }
  ASSERT_EQ(expected.CountSetBits(), bv.CountSetBits());
}

TEST(BitVectorUnittest, IntersectRange) {
  BitVector bv =
      BitVector::RangeForTesting(1, 20, [](uint32_t t) { return t % 2 == 0; });
  BitVector intersected = bv.IntersectRange(3, 10);

  ASSERT_EQ(intersected.IndexOfNthSet(0), 4u);
  ASSERT_EQ(intersected.CountSetBits(), 3u);
}

TEST(BitVectorUnittest, IntersectRangeFromStart) {
  BitVector bv =
      BitVector::RangeForTesting(1, 20, [](uint32_t t) { return t % 2 == 0; });
  BitVector intersected = bv.IntersectRange(0, 10);

  ASSERT_EQ(intersected.IndexOfNthSet(0), 2u);
  ASSERT_EQ(intersected.CountSetBits(), 4u);
}

TEST(BitVectorUnittest, IntersectRange2) {
  BitVector bv{true, false, true, true, false, true};
  BitVector intersected = bv.IntersectRange(2, 4);

  ASSERT_EQ(intersected.IndexOfNthSet(0), 2u);
}

TEST(BitVectorUnittest, IntersectRangeAfterWord) {
  BitVector bv = BitVector::RangeForTesting(
      64 + 1, 64 + 20, [](uint32_t t) { return t % 2 == 0; });
  BitVector intersected = bv.IntersectRange(64 + 3, 64 + 10);

  ASSERT_EQ(intersected.IndexOfNthSet(0), 64 + 4u);
  ASSERT_EQ(intersected.CountSetBits(), 3u);
}

TEST(BitVectorUnittest, IntersectRangeSetBitsBeforeRange) {
  BitVector bv =
      BitVector::RangeForTesting(10, 30, [](uint32_t t) { return t < 15; });
  BitVector intersected = bv.IntersectRange(16, 50);

  ASSERT_FALSE(intersected.CountSetBits());
}

TEST(BitVectorUnittest, IntersectRangeSetBitOnBoundary) {
  BitVector bv = BitVector(10, false);
  bv.Set(5);
  BitVector intersected = bv.IntersectRange(5, 20);

  ASSERT_EQ(intersected.CountSetBits(), 1u);
  ASSERT_EQ(intersected.IndexOfNthSet(0), 5u);
}

TEST(BitVectorUnittest, IntersectRangeStressTest) {
  BitVector bv = BitVector::RangeForTesting(
      65, 1024 + 1, [](uint32_t t) { return t % 2 == 0; });
  BitVector intersected = bv.IntersectRange(30, 500);

  ASSERT_EQ(intersected.IndexOfNthSet(0), 66u);
  ASSERT_EQ(intersected.CountSetBits(), 217u);
}

TEST(BitVectorUnittest, IntersectRangeAppendFalse) {
  BitVector bv(70u, true);
  BitVector out = bv.IntersectRange(10, 12u);
  out.Resize(70u);

  ASSERT_TRUE(out.IsSet(10u));
  ASSERT_TRUE(out.IsSet(11u));
  ASSERT_FALSE(out.IsSet(12u));
  ASSERT_FALSE(out.IsSet(60u));
  ASSERT_FALSE(out.IsSet(69u));
}

TEST(BitVectorUnittest, Range) {
  BitVector bv =
      BitVector::RangeForTesting(1, 9, [](uint32_t t) { return t % 3 == 0; });
  ASSERT_EQ(bv.size(), 9u);

  ASSERT_FALSE(bv.IsSet(0));
  ASSERT_TRUE(bv.IsSet(3));
  ASSERT_TRUE(bv.IsSet(6));

  ASSERT_EQ(bv.CountSetBits(), 2u);
}

TEST(BitVectorUnittest, RangeStressTest) {
  BitVector bv = BitVector::RangeForTesting(
      1, 1025, [](uint32_t t) { return t % 3 == 0; });
  ASSERT_EQ(bv.size(), 1025u);
  ASSERT_FALSE(bv.IsSet(0));
  for (uint32_t i = 1; i < 1025; ++i) {
    ASSERT_EQ(i % 3 == 0, bv.IsSet(i));
  }
  ASSERT_EQ(bv.CountSetBits(), 341u);
}

TEST(BitVectorUnittest, BuilderSkip) {
  BitVector::Builder builder(128, 127);
  builder.Append(1);

  BitVector bv = std::move(builder).Build();
  ASSERT_EQ(bv.size(), 128u);

  ASSERT_FALSE(bv.IsSet(10));
  ASSERT_FALSE(bv.IsSet(126));
  ASSERT_TRUE(bv.IsSet(127));
}

TEST(BitVectorUnittest, BuilderSkipAll) {
  BitVector::Builder builder(128, 128);
  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.size(), 128u);
  ASSERT_EQ(bv.CountSetBits(), 0u);
}

TEST(BitVectorUnittest, BuilderBitsInCompleteWordsUntilFull) {
  BitVector::Builder builder(128 + 1);

  ASSERT_EQ(builder.BitsInCompleteWordsUntilFull(), 128u);
}

TEST(BitVectorUnittest, BuilderBitsUntilWordBoundaryOrFull) {
  BitVector::Builder builder(41);

  ASSERT_EQ(builder.BitsUntilWordBoundaryOrFull(), 41u);
}

TEST(BitVectorUnittest, Builder) {
  BitVector::Builder builder(128);

  // 100100011010001010110011110001001 as a hex literal.
  builder.AppendWord(0x123456789);
  builder.AppendWord(0xFF);

  BitVector bv = std::move(builder).Build();
  ASSERT_EQ(bv.size(), 128u);

  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_FALSE(bv.IsSet(1));
  ASSERT_FALSE(bv.IsSet(2));
}

TEST(BitVectorUnittest, BuilderCountSetBits) {
  // 16 words and 1 bit
  BitVector::Builder builder(1025);

  // 100100011010001010110011110001001 as a hex literal, with 15 set bits.
  uint64_t word = 0x123456789;
  for (uint32_t i = 0; i < 16; ++i) {
    builder.AppendWord(word);
  }
  builder.Append(1);
  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.CountSetBits(500), 120u);
  ASSERT_EQ(bv.CountSetBits(), 16 * 15 + 1u);
}

TEST(BitVectorUnittest, BuilderStressTest) {
  // Space for 128 words and 1 bit
  uint32_t size = 8 * 1024 + 1;
  BitVector::Builder builder(size);

  // 15 full words + 40 bits
  for (uint32_t i = 0; i < 1000; ++i) {
    builder.Append(1);
  }
  ASSERT_EQ(builder.BitsUntilFull(), size - 1000);

  // 24 bits to hit word boundary. We filled 16 words now.
  for (uint32_t i = 0; i < 24; ++i) {
    builder.Append(0);
  }
  ASSERT_EQ(builder.BitsUntilFull(), size - 1024);
  ASSERT_EQ(builder.BitsUntilWordBoundaryOrFull(), 0u);

  // 100100011010001010110011110001001 as a hex literal, with 15 set bits.
  uint64_t word = 0x123456789;

  // Add all of the remaining words.
  ASSERT_EQ(builder.BitsInCompleteWordsUntilFull(), (128 - 16) * 64u);
  ASSERT_EQ(builder.BitsUntilFull(), (128 - 16) * 64u + 1);
  for (uint32_t i = 0; i < (128 - 16); ++i) {
    builder.AppendWord(word);
  }

  ASSERT_EQ(builder.BitsUntilWordBoundaryOrFull(), 0u);
  ASSERT_EQ(builder.BitsUntilFull(), 1u);

  // One last bit.
  builder.Append(1);

  BitVector bv = std::move(builder).Build();

  ASSERT_EQ(bv.CountSetBits(), 2681u);
  ASSERT_EQ(bv.size(), 8u * 1024u + 1u);

  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_FALSE(bv.IsSet(1000));

  ASSERT_TRUE(bv.IsSet(1024));
  ASSERT_FALSE(bv.IsSet(1025));

  ASSERT_TRUE(bv.IsSet(8 * 1024));
}

TEST(BitVectorUnittest, FromSortedIndexVectorEmpty) {
  std::vector<int64_t> indices{};
  BitVector bv = BitVector::FromSortedIndexVector(indices);

  ASSERT_EQ(bv.size(), 0u);
}

TEST(BitVectorUnittest, FromSortedIndexVector) {
  std::vector<int64_t> indices{0, 100, 200, 2000};
  BitVector bv = BitVector::FromSortedIndexVector(indices);

  ASSERT_EQ(bv.size(), 2001u);
  ASSERT_EQ(bv.CountSetBits(), 4u);
  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_TRUE(bv.IsSet(100));
  ASSERT_TRUE(bv.IsSet(200));
  ASSERT_TRUE(bv.IsSet(2000));
}

TEST(BitVectorUnittest, FromSortedIndexVectorStressTestLargeValues) {
  std::vector<int64_t> indices{0, 1 << 2, 1 << 10, 1 << 20, 1 << 30};
  BitVector bv = BitVector::FromSortedIndexVector(indices);

  ASSERT_EQ(bv.size(), (1 << 30) + 1u);
  ASSERT_EQ(bv.CountSetBits(), 5u);
  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_TRUE(bv.IsSet(1 << 2));
  ASSERT_TRUE(bv.IsSet(1 << 10));
  ASSERT_TRUE(bv.IsSet(1 << 20));
  ASSERT_TRUE(bv.IsSet(1 << 30));
}

TEST(BitVectorUnittest, FromUnsortedIndexVectorEmpty) {
  std::vector<uint32_t> indices{};
  BitVector bv = BitVector::FromUnsortedIndexVector(indices);

  ASSERT_EQ(bv.size(), 0u);
}

TEST(BitVectorUnittest, FromUnsortedIndexVector) {
  std::vector<uint32_t> indices{0, 2000, 200, 100};
  BitVector bv = BitVector::FromUnsortedIndexVector(indices);

  ASSERT_EQ(bv.size(), 2001u);
  ASSERT_EQ(bv.CountSetBits(), 4u);
  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_TRUE(bv.IsSet(100));
  ASSERT_TRUE(bv.IsSet(200));
  ASSERT_TRUE(bv.IsSet(2000));
}

TEST(BitVectorUnittest, FromUnsortedIndexVectorStressTestLargeValues) {
  std::vector<uint32_t> indices{0, 1 << 30, 1 << 10, 1 << 2, 1 << 20};
  BitVector bv = BitVector::FromUnsortedIndexVector(indices);

  ASSERT_EQ(bv.size(), (1 << 30) + 1u);
  ASSERT_EQ(bv.CountSetBits(), 5u);
  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_TRUE(bv.IsSet(1 << 2));
  ASSERT_TRUE(bv.IsSet(1 << 10));
  ASSERT_TRUE(bv.IsSet(1 << 20));
  ASSERT_TRUE(bv.IsSet(1 << 30));
}

TEST(BitVectorUnittest, Not) {
  BitVector bv(10);
  bv.Set(2);
  bv.Not();

  EXPECT_FALSE(bv.IsSet(2));
  EXPECT_EQ(bv.CountSetBits(), 9u);
  EXPECT_THAT(bv.GetSetBitIndices(),
              UnorderedElementsAre(0u, 1u, 3u, 4u, 5u, 6u, 7u, 8u, 9u));
}

TEST(BitVectorUnittest, NotBig) {
  BitVector bv = BitVector::RangeForTesting(
      0, 1026, [](uint32_t i) { return i % 5 == 0; });
  bv.Not();

  EXPECT_EQ(bv.CountSetBits(), 820u);
}

TEST(BitVectorUnittest, NotAppendAfter) {
  BitVector bv(30);
  bv.Not();
  bv.AppendFalse();

  ASSERT_FALSE(bv.IsSet(30));
}

TEST(BitVectorUnittest, Or) {
  BitVector bv{1, 1, 0, 0};
  BitVector bv_second{1, 0, 1, 0};
  bv.Or(bv_second);

  ASSERT_EQ(bv.CountSetBits(), 3u);
  ASSERT_TRUE(bv.Set(0));
  ASSERT_TRUE(bv.Set(1));
  ASSERT_TRUE(bv.Set(2));
}

TEST(BitVectorUnittest, OrBig) {
  BitVector bv = BitVector::RangeForTesting(
      0, 1025, [](uint32_t i) { return i % 5 == 0; });
  BitVector bv_sec = BitVector::RangeForTesting(
      0, 1025, [](uint32_t i) { return i % 3 == 0; });
  bv.Or(bv_sec);

  BitVector bv_or = BitVector::RangeForTesting(
      0, 1025, [](uint32_t i) { return i % 5 == 0 || i % 3 == 0; });

  ASSERT_EQ(bv.CountSetBits(), bv_or.CountSetBits());
}

TEST(BitVectorUnittest, QueryStressTest) {
  BitVector bv;
  std::vector<bool> bool_vec;
  std::vector<uint32_t> int_vec;

  static constexpr uint32_t kCount = 4096;
  std::minstd_rand0 rand;
  for (uint32_t i = 0; i < kCount; ++i) {
    bool res = rand() % 2u;
    if (res) {
      bv.AppendTrue();
    } else {
      bv.AppendFalse();
    }
    bool_vec.push_back(res);
    if (res)
      int_vec.emplace_back(i);
  }
}

TEST(BitVectorUnittest, GetSetBitIndices) {
  BitVector bv = {true, false, true, false, true, true, false, false};
  ASSERT_THAT(bv.GetSetBitIndices(), ElementsAre(0u, 2u, 4u, 5u));
}

TEST(BitVectorUnittest, GetSetBitIndicesIntersectRange) {
  BitVector bv(130u, true);
  BitVector out = bv.IntersectRange(10, 12);
  ASSERT_THAT(out.GetSetBitIndices(), ElementsAre(10, 11));
}

TEST(BitVectorUnittest, UpdateSetBitsGetSetBitIndices) {
  BitVector bv(130u, true);
  bv.UpdateSetBits(BitVector(60u));
  ASSERT_THAT(bv.GetSetBitIndices(), IsEmpty());
}

TEST(BitVectorUnittest, SerializeSimple) {
  BitVector bv{1, 0, 1, 0, 1, 0, 1};
  protozero::HeapBuffered<protos::pbzero::SerializedColumn::BitVector> msg;
  bv.Serialize(msg.get());
  auto buffer = msg.SerializeAsArray();

  protos::pbzero::SerializedColumn::BitVector::Decoder decoder(buffer.data(),
                                                               buffer.size());
  ASSERT_EQ(decoder.size(), 7u);
}

TEST(BitVectorUnittest, SerializeDeserializeSimple) {
  BitVector bv{1, 0, 1, 0, 1, 0, 1};
  protozero::HeapBuffered<protos::pbzero::SerializedColumn::BitVector> msg;
  bv.Serialize(msg.get());
  auto buffer = msg.SerializeAsArray();

  protos::pbzero::SerializedColumn::BitVector::Decoder decoder(buffer.data(),
                                                               buffer.size());

  BitVector des;
  des.Deserialize(decoder);

  ASSERT_EQ(des.size(), 7u);
  ASSERT_EQ(des.CountSetBits(), 4u);

  ASSERT_TRUE(des.IsSet(0));
  ASSERT_TRUE(des.IsSet(2));
  ASSERT_TRUE(des.IsSet(4));
  ASSERT_TRUE(des.IsSet(6));
}

}  // namespace
}  // namespace perfetto::trace_processor
