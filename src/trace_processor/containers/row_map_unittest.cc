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

#include "src/trace_processor/containers/row_map.h"

#include <memory>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(RowMapUnittest, SingleRow) {
  RowMap rm(10, 20);
  RowMap rm_row = rm.SingleRow(15u);
  ASSERT_EQ(rm_row.size(), 1u);
  ASSERT_TRUE(rm_row.Contains(15));
  ASSERT_FALSE(rm_row.Contains(11));
}

TEST(RowMapUnittest, CopyRange) {
  RowMap rm(10, 20);
  RowMap rm_copy = rm.Copy();
  ASSERT_EQ(rm_copy.size(), 10u);
}

TEST(RowMapUnittest, CopyBitVector) {
  RowMap rm(BitVector{true, false, false, false, true, true});
  RowMap rm_copy = rm.Copy();
  ASSERT_EQ(rm_copy.size(), 3u);
}

TEST(RowMapUnittest, CopyIndexVector) {
  RowMap rm(std::vector<uint32_t>{10, 17, 20, 21});
  RowMap rm_copy = rm.Copy();
  ASSERT_EQ(rm_copy.size(), 4u);
}

TEST(RowMapUnittest, GetFromRange) {
  RowMap rm(10, 20);
  ASSERT_EQ(rm.Get(5), 15u);
}

TEST(RowMapUnittest, GetFromBitVector) {
  RowMap rm(BitVector{true, false, false, false, true, true});
  ASSERT_EQ(rm.Get(1), 4u);
}

TEST(RowMapUnittest, GetFromIndexVector) {
  RowMap rm(std::vector<uint32_t>{10, 17, 20, 21});
  ASSERT_EQ(rm.Get(1), 17u);
}

TEST(RowMapUnittest, ContainsFromRange) {
  RowMap rm(10, 20);
  ASSERT_FALSE(rm.Contains(5));
  ASSERT_TRUE(rm.Contains(15));
}

TEST(RowMapUnittest, ContainsFromBitVector) {
  RowMap rm(BitVector{true, false, false, false, true, true});
  ASSERT_FALSE(rm.Contains(3));
  ASSERT_TRUE(rm.Contains(5));
}

TEST(RowMapUnittest, ContainsFromIndexVector) {
  RowMap rm(std::vector<uint32_t>{10, 17, 20, 21});
  ASSERT_FALSE(rm.Contains(5));
  ASSERT_TRUE(rm.Contains(10));
}

TEST(RowMapUnittest, RowOfRange) {
  RowMap rm(10, 20);
  ASSERT_EQ(rm.RowOf(15).value(), 5u);
  ASSERT_EQ(rm.RowOf(5), std::nullopt);
}

TEST(RowMapUnittest, RowOfBitVector) {
  RowMap rm(BitVector{true, false, false, false, true, true});
  ASSERT_EQ(rm.RowOf(4), 1u);
  ASSERT_EQ(rm.RowOf(1), std::nullopt);
}

TEST(RowMapUnittest, RowOfIndexVector) {
  RowMap rm(std::vector<uint32_t>{10, 17, 20, 21});
  ASSERT_EQ(rm.RowOf(17), 1u);
  ASSERT_EQ(rm.RowOf(5), std::nullopt);
}

TEST(RowMapUnittest, InsertIntoRangeAtTheEnd) {
  RowMap rm(10, 20);
  rm.Insert(21);
  ASSERT_EQ(rm.size(), 11u);
  ASSERT_TRUE(rm.Contains(21));
}

TEST(RowMapUnittest, InsertIntoRange) {
  RowMap rm(10, 20);
  rm.Insert(25);
  ASSERT_EQ(rm.size(), 11u);
  ASSERT_TRUE(rm.Contains(25));
}

TEST(RowMapUnittest, InsertIntoBitVector) {
  RowMap rm(BitVector{true, false, false, false, true, true});
  rm.Insert(25);
  ASSERT_EQ(rm.size(), 4u);
  ASSERT_TRUE(rm.Contains(25));
}

TEST(RowMapUnittest, InsertIntoIndexVector) {
  RowMap rm(std::vector<uint32_t>{10, 17, 20, 21});
  rm.Insert(25);
  ASSERT_EQ(rm.size(), 5u);
  ASSERT_TRUE(rm.Contains(25));
}

TEST(RowMapUnittest, SelectRowsFromRangeWithRange) {
  RowMap rm(10, 20);

  RowMap selector(4, 8);
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 4u);
  ASSERT_EQ(selected.Get(0), 14u);
}

TEST(RowMapUnittest, SelectRowsFromRangeWithBV) {
  RowMap rm(10, 20);
  // BitVector with values at 16, 18, 20 and so on.
  RowMap selector(
      BitVector::RangeForTesting(4, 8, [](uint32_t x) { return x % 2 == 0; }));
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 2u);
  ASSERT_EQ(selected.Get(0), 14u);
}

TEST(RowMapUnittest, SelectRowsFromRangeWithIV) {
  RowMap rm(10, 20);
  RowMap selector(std::vector<uint32_t>{4, 6});
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 2u);
  ASSERT_EQ(selected.Get(0), 14u);
}

TEST(RowMapUnittest, SelectRowsFromBVWithRange) {
  RowMap rm(BitVector::RangeForTesting(10, 50,
                                       [](uint32_t x) { return x % 2 == 0; }));

  RowMap selector(4, 8);
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 4u);
  ASSERT_EQ(selected.Get(0), 18u);
}

TEST(RowMapUnittest, SelectRowsFromBVWithBV) {
  RowMap rm(BitVector::RangeForTesting(10, 50,
                                       [](uint32_t x) { return x % 2 == 0; }));
  // BitVector with values at 16, 18, 20 and so on.
  RowMap selector(
      BitVector::RangeForTesting(4, 8, [](uint32_t x) { return x % 2 == 0; }));
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 2u);
  ASSERT_EQ(selected.Get(0), 18u);
}

TEST(RowMapUnittest, SelectRowsFromBVWithIV) {
  RowMap rm(BitVector::RangeForTesting(10, 50,
                                       [](uint32_t x) { return x % 2 == 0; }));
  RowMap selector(std::vector<uint32_t>{4, 6});
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 2u);
  ASSERT_EQ(selected.Get(0), 18u);
}

TEST(RowMapUnittest, SelectRowsFromIVWithRange) {
  RowMap rm(std::vector<uint32_t>{10, 12, 14, 16, 18, 20, 22, 24});

  RowMap selector(4, 8);
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 4u);
  ASSERT_EQ(selected.Get(0), 18u);
}

TEST(RowMapUnittest, SelectRowsFromIVWithBV) {
  RowMap rm(std::vector<uint32_t>{10, 12, 14, 16, 18, 20, 22, 24});
  RowMap selector(
      BitVector::RangeForTesting(4, 8, [](uint32_t x) { return x % 2 == 0; }));
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 2u);
  ASSERT_EQ(selected.Get(0), 18u);
}

TEST(RowMapUnittest, SelectRowsFromIVWithIV) {
  RowMap rm(std::vector<uint32_t>{10, 12, 14, 16, 18, 20, 22, 24});
  RowMap selector(std::vector<uint32_t>{4, 6});
  RowMap selected = rm.SelectRows(selector);
  ASSERT_EQ(selected.size(), 2u);
  ASSERT_EQ(selected.Get(0), 18u);
}

TEST(RowMapUnittest, SmokeRange) {
  RowMap rm(30, 47);

  ASSERT_EQ(rm.size(), 17u);

  ASSERT_EQ(rm.Get(0), 30u);
  ASSERT_EQ(rm.Get(1), 31u);
  ASSERT_EQ(rm.Get(16), 46u);

  ASSERT_EQ(rm.RowOf(29), std::nullopt);
  ASSERT_EQ(rm.RowOf(30), 0u);
  ASSERT_EQ(rm.RowOf(37), 7u);
  ASSERT_EQ(rm.RowOf(46), 16u);
  ASSERT_EQ(rm.RowOf(47), std::nullopt);
}

TEST(RowMapUnittest, SmokeBitVector) {
  RowMap rm(BitVector{true, false, false, false, true, true});

  ASSERT_EQ(rm.size(), 3u);

  ASSERT_EQ(rm.Get(0u), 0u);
  ASSERT_EQ(rm.Get(1u), 4u);
  ASSERT_EQ(rm.Get(2u), 5u);

  ASSERT_EQ(rm.RowOf(0u), 0u);
  ASSERT_EQ(rm.RowOf(4u), 1u);
  ASSERT_EQ(rm.RowOf(5u), 2u);

  ASSERT_EQ(rm.RowOf(1u), std::nullopt);
  ASSERT_EQ(rm.RowOf(100u), std::nullopt);
}

TEST(RowMapUnittest, SmokeIndexVector) {
  RowMap rm(std::vector<uint32_t>{32u, 56u, 24u, 0u, 100u, 1u});

  ASSERT_EQ(rm.size(), 6u);

  ASSERT_EQ(rm.Get(0u), 32u);
  ASSERT_EQ(rm.Get(1u), 56u);
  ASSERT_EQ(rm.Get(2u), 24u);
  ASSERT_EQ(rm.Get(3u), 0u);
  ASSERT_EQ(rm.Get(4u), 100u);
  ASSERT_EQ(rm.Get(5u), 1u);

  ASSERT_EQ(rm.RowOf(32u), 0u);
  ASSERT_EQ(rm.RowOf(56u), 1u);
  ASSERT_EQ(rm.RowOf(24u), 2u);
  ASSERT_EQ(rm.RowOf(0u), 3u);
  ASSERT_EQ(rm.RowOf(100u), 4u);
  ASSERT_EQ(rm.RowOf(1u), 5u);
}

TEST(RowMapUnittest, InsertToRangeAfter) {
  RowMap rm(3u, 7u);
  rm.Insert(10u);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(4u), 10u);
  ASSERT_EQ(rm.RowOf(10u), 4u);
}

TEST(RowMapUnittest, InsertToBitVectorBefore) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  rm.Insert(1u);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(0u), 0u);
  ASSERT_EQ(rm.Get(1u), 1u);
  ASSERT_EQ(rm.Get(2u), 2u);
  ASSERT_EQ(rm.Get(3u), 3u);
  ASSERT_EQ(rm.Get(4u), 5u);
}

TEST(RowMapUnittest, InsertToBitVectorAfter) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  rm.Insert(10u);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(4u), 10u);
  ASSERT_EQ(rm.RowOf(10u), 4u);
}

TEST(RowMapUnittest, InsertToIndexVectorAfter) {
  RowMap rm(std::vector<uint32_t>{0u, 2u, 3u, 5u});
  rm.Insert(10u);

  ASSERT_EQ(rm.size(), 5u);
  ASSERT_EQ(rm.Get(4u), 10u);
  ASSERT_EQ(rm.RowOf(10u), 4u);
}

TEST(RowMapUnittest, ContainsRange) {
  RowMap rm(93, 157);

  ASSERT_TRUE(rm.Contains(93));
  ASSERT_TRUE(rm.Contains(105));
  ASSERT_TRUE(rm.Contains(156));

  ASSERT_FALSE(rm.Contains(0));
  ASSERT_FALSE(rm.Contains(92));
  ASSERT_FALSE(rm.Contains(157));
}

TEST(RowMapUnittest, ContainsBitVector) {
  RowMap rm(BitVector{true, false, true, true, false, true});

  ASSERT_TRUE(rm.Contains(0));
  ASSERT_TRUE(rm.Contains(2));
  ASSERT_TRUE(rm.Contains(3));

  ASSERT_FALSE(rm.Contains(1));
  ASSERT_FALSE(rm.Contains(4));
  ASSERT_FALSE(rm.Contains(6));
}

TEST(RowMapUnittest, ContainsIndexVector) {
  RowMap rm(std::vector<uint32_t>{0u, 2u, 3u, 5u});

  ASSERT_TRUE(rm.Contains(0));
  ASSERT_TRUE(rm.Contains(2));
  ASSERT_TRUE(rm.Contains(3));

  ASSERT_FALSE(rm.Contains(1));
  ASSERT_FALSE(rm.Contains(4));
  ASSERT_FALSE(rm.Contains(6));
}

TEST(RowMapUnittest, OutputSizeIV) {
  RowMap rm(std::vector<uint32_t>{0u, 2u, 3u, 5u});
  ASSERT_EQ(rm.Max(), 6u);
}

TEST(RowMapUnittest, OutputSizeRange) {
  RowMap rm(0, 10);
  ASSERT_EQ(rm.Max(), 10u);
}

TEST(RowMapUnittest, OutputSizeBV) {
  RowMap rm(BitVector{0, 1, 0, 1, 0, 1, 0});
  ASSERT_EQ(rm.Max(), 7u);
}

TEST(RowMapUnittest, SelectRangeWithRange) {
  RowMap rm(93, 157);
  RowMap picker(4, 7);
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 3u);
  ASSERT_EQ(res.Get(0u), 97u);
  ASSERT_EQ(res.Get(1u), 98u);
  ASSERT_EQ(res.Get(2u), 99u);
}

TEST(RowMapUnittest, SelectBitVectorWithRange) {
  RowMap rm(BitVector{true, false, false, true, false, true, false});
  RowMap picker(1u, 3u);
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0u), 3u);
  ASSERT_EQ(res.Get(1u), 5u);
}

TEST(RowMapUnittest, SelectIndexVectorWithRange) {
  RowMap rm(std::vector<uint32_t>{33, 2u, 45u, 7u, 8u, 9u});
  RowMap picker(2, 5);
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 3u);
  ASSERT_EQ(res.Get(0u), 45u);
  ASSERT_EQ(res.Get(1u), 7u);
  ASSERT_EQ(res.Get(2u), 8u);
}

TEST(RowMapUnittest, SelectRangeWithBitVector) {
  RowMap rm(27, 31);
  RowMap picker(BitVector{true, false, false, true});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0u), 27u);
  ASSERT_EQ(res.Get(1u), 30u);
}

TEST(RowMapUnittest, SelectRangeWithSingleBitVector) {
  RowMap rm(27, 31);
  RowMap picker(BitVector{false, true});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 1u);
  ASSERT_EQ(res.Get(0u), 28u);
}

TEST(RowMapUnittest, SelectRangeWithSmallBitVector) {
  RowMap rm(27, 31);
  RowMap picker(BitVector{false, true, true});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0u), 28u);
  ASSERT_EQ(res.Get(1u), 29u);
}

TEST(RowMapUnittest, SelectBitVectorWithBitVector) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  RowMap picker(BitVector{true, false, false, true});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0u), 0u);
  ASSERT_EQ(res.Get(1u), 5u);
}

TEST(RowMapUnittest, SelectBitVectorWithSingleBitVector) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  RowMap picker(BitVector{false, true});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 1u);
  ASSERT_EQ(res.Get(0u), 2u);
}

TEST(RowMapUnittest, SelectBitVectorWithSmallBitVector) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  RowMap picker(BitVector{false, true, true});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0u), 2u);
  ASSERT_EQ(res.Get(1u), 3u);
}

TEST(RowMapUnittest, SelectIndexVectorWithBitVector) {
  RowMap rm(std::vector<uint32_t>{0u, 2u, 3u, 5u});
  RowMap picker(BitVector{true, false, false, true});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0u), 0u);
  ASSERT_EQ(res.Get(1u), 5u);
}

TEST(RowMapUnittest, SelectIndexVectorWithSmallBitVector) {
  RowMap rm(std::vector<uint32_t>{0u, 2u, 3u, 5u});
  RowMap picker(BitVector{false, true, true});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 2u);
  ASSERT_EQ(res.Get(0u), 2u);
  ASSERT_EQ(res.Get(1u), 3u);
}

TEST(RowMapUnittest, SelectRangeWithIndexVector) {
  RowMap rm(27, 31);
  RowMap picker(std::vector<uint32_t>{3u, 2u, 0u, 1u, 1u, 3u});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 6u);
  ASSERT_EQ(res.Get(0u), 30u);
  ASSERT_EQ(res.Get(1u), 29u);
  ASSERT_EQ(res.Get(2u), 27u);
  ASSERT_EQ(res.Get(3u), 28u);
  ASSERT_EQ(res.Get(4u), 28u);
  ASSERT_EQ(res.Get(5u), 30u);
}

TEST(RowMapUnittest, SelectBitVectorWithIndexVector) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  RowMap picker(std::vector<uint32_t>{3u, 2u, 0u, 1u, 1u, 3u});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 6u);
  ASSERT_EQ(res.Get(0u), 5u);
  ASSERT_EQ(res.Get(1u), 3u);
  ASSERT_EQ(res.Get(2u), 0u);
  ASSERT_EQ(res.Get(3u), 2u);
  ASSERT_EQ(res.Get(4u), 2u);
  ASSERT_EQ(res.Get(5u), 5u);
}

TEST(RowMapUnittest, SelectIndexVectorWithIndexVector) {
  RowMap rm(std::vector<uint32_t>{33u, 2u, 45u, 7u, 8u, 9u});
  RowMap picker(std::vector<uint32_t>{3u, 2u, 0u, 1u, 1u, 3u});
  auto res = rm.SelectRows(picker);

  ASSERT_EQ(res.size(), 6u);
  ASSERT_EQ(res.Get(0u), 7u);
  ASSERT_EQ(res.Get(1u), 45u);
  ASSERT_EQ(res.Get(2u), 33u);
  ASSERT_EQ(res.Get(3u), 2u);
  ASSERT_EQ(res.Get(4u), 2u);
  ASSERT_EQ(res.Get(5u), 7u);
}

TEST(RowMapUnittest, Clear) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  rm.Clear();

  ASSERT_EQ(rm.size(), 0u);
}

TEST(RowMapUnittest, IntersectSinglePresent) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  rm.IntersectExact(2u);

  ASSERT_EQ(rm.size(), 1u);
  ASSERT_EQ(rm.Get(0u), 2u);
}

TEST(RowMapUnittest, IntersectSingleAbsent) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  rm.IntersectExact(1u);

  ASSERT_EQ(rm.size(), 0u);
}

TEST(RowMapUnittest, IntersectRangeWithRange) {
  RowMap rm(3, 7);
  RowMap sec(2, 4);
  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 1u);
  ASSERT_EQ(rm.Get(0u), 3u);
}

TEST(RowMapUnittest, IntersectRangeWithBV) {
  RowMap rm(2, 4);
  RowMap sec(BitVector{true, false, true, true, false, true});
  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 2u);
  ASSERT_EQ(rm.Get(0), 2u);
}

TEST(RowMapUnittest, IntersectRangeWithIV) {
  RowMap rm(2, 10);
  RowMap sec(std::vector<uint32_t>{0, 2, 5});
  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 2u);
  ASSERT_EQ(rm.Get(0u), 2u);
}

TEST(RowMapUnittest, IntersectBVWithRange) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  RowMap sec(2, 4);
  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 2u);
  ASSERT_EQ(rm.Get(0), 2u);
}

TEST(RowMapUnittest, IntersectBVWithBV) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  RowMap sec(BitVector{false, true, true, false, false, true, true});
  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 2u);
  ASSERT_EQ(rm.Get(0), 2u);
}

TEST(RowMapUnittest, IntersectBVWithIV) {
  RowMap rm(BitVector{true, false, true, true, false, true});
  RowMap sec(std::vector<uint32_t>{0, 2, 5});
  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 3u);
  ASSERT_EQ(rm.Get(0), 0u);
}

TEST(RowMapUnittest, IntersectIVWithRange) {
  RowMap rm(std::vector<uint32_t>{0, 2, 5});
  RowMap sec(2, 10);
  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 2u);
  ASSERT_EQ(rm.Get(0u), 2u);
}

TEST(RowMapUnittest, IntersectIVWithBV) {
  RowMap rm(std::vector<uint32_t>{0, 2, 5});
  RowMap sec(BitVector{true, false, true, true, false, true});
  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 3u);
  ASSERT_EQ(rm.Get(0), 0u);
}

TEST(RowMapUnittest, IntersectIVWithIV) {
  RowMap rm(std::vector<uint32_t>{0, 2, 5});
  RowMap sec(std::vector<uint32_t>{1, 2, 6});

  rm.Intersect(sec);

  ASSERT_EQ(rm.size(), 1u);
  ASSERT_EQ(rm.Get(0u), 2u);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
