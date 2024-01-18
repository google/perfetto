/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/common/address_range.h"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {

// Limited support for abseil in perfetto so we can not use the recommended
// AbslStringify()
inline void PrintTo(const AddressRange& r, std::ostream* os) {
  if (r.empty()) {
    *os << "(empty)";
  } else {
    *os << "[" << r.start() << "," << r.end() << ")";
  }
}

namespace {

using ::testing::A;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::MockFunction;
using ::testing::Ne;
using ::testing::Pair;
using ::testing::Pointee;
using ::testing::SizeIs;

auto AppendRangesTo(std::vector<AddressRange>& ranges) {
  return [&ranges](std::pair<const AddressRange, int>& e) {
    ranges.push_back(e.first);
  };
}

TEST(AddressRange, EmptyByDefault) {
  constexpr AddressRange kRange;
  // This is more of an implementation detail (that start and end are
  // initialized to zero). But this "knowledge" is used for the contains tests,
  // to probe for those specific values.
  EXPECT_THAT(kRange.end(), Eq(0u));
  EXPECT_THAT(kRange.start(), Eq(0u));
  EXPECT_THAT(kRange.length(), Eq(0u));
  EXPECT_THAT(kRange, IsEmpty());
}

TEST(AddressRange, EmptyRangeContainsNothing) {
  constexpr AddressRange kEmptyRange;
  EXPECT_FALSE(kEmptyRange.Contains(0));
}

TEST(AddressRange, ContainsAddress) {
  constexpr AddressRange kRange(1, 10);
  EXPECT_FALSE(kRange.Contains(0));
  EXPECT_TRUE(kRange.Contains(1));
  EXPECT_TRUE(kRange.Contains(9));
  EXPECT_FALSE(kRange.Contains(10));
}

TEST(AddressRange, MaxRangeContainsAll) {
  constexpr AddressRange kMaxRange(0, std::numeric_limits<uint64_t>::max());
  EXPECT_TRUE(kMaxRange.Contains(0));
  EXPECT_TRUE(kMaxRange.Contains(std::numeric_limits<uint64_t>::max() - 1));
  // End is not inclusive.
  EXPECT_FALSE(kMaxRange.Contains(std::numeric_limits<uint64_t>::max()));
}

TEST(AddressRange, ContainsRange) {
  constexpr AddressRange kRange(10, 20);
  EXPECT_TRUE(kRange.Contains(kRange));
  EXPECT_TRUE(kRange.Contains(AddressRange(11, 19)));
  EXPECT_TRUE(kRange.Contains(AddressRange(10, 19)));
  EXPECT_TRUE(kRange.Contains(AddressRange(11, 20)));

  EXPECT_FALSE(kRange.Contains(AddressRange(9, 20)));
  EXPECT_FALSE(kRange.Contains(AddressRange(10, 21)));
  EXPECT_FALSE(kRange.Contains(AddressRange(9, 10)));
  EXPECT_FALSE(kRange.Contains(AddressRange(20, 21)));
}

TEST(AddressRange, Intersect) {
  EXPECT_THAT(AddressRange(0, 10).IntersectWith(AddressRange(0, 10)),
              Eq(AddressRange(0, 10)));

  EXPECT_THAT(AddressRange(0, 10).IntersectWith(AddressRange(10, 20)),
              IsEmpty());

  EXPECT_THAT(AddressRange(0, 10).IntersectWith(AddressRange(0, 0)),
              Eq(AddressRange(0, 0)));
  EXPECT_THAT(AddressRange(0, 10).IntersectWith(AddressRange(1, 10)),
              Eq(AddressRange(1, 10)));

  EXPECT_THAT(AddressRange(0, 10).IntersectWith(AddressRange()), IsEmpty());
}

TEST(AddressRangeMap, Empty) {
  AddressRangeMap<int> empty;
  EXPECT_THAT(empty, IsEmpty());
}

TEST(AddressRangeMap, EmplaceFailsForOverlaps) {
  AddressRangeMap<int> map;
  ASSERT_TRUE(map.Emplace(AddressRange(10, 20)).second);

  EXPECT_FALSE(map.Emplace(AddressRange(10, 20)).second);
  EXPECT_FALSE(map.Emplace(AddressRange(11, 19)).second);
  EXPECT_FALSE(map.Emplace(AddressRange(0, 11)).second);
  EXPECT_FALSE(map.Emplace(AddressRange(19, 30)).second);
  EXPECT_THAT(map, SizeIs(1));
}

TEST(AddressRangeMap, EmplaceSucceedsForNonOverlaps) {
  AddressRangeMap<int> map;

  EXPECT_TRUE(map.Emplace(AddressRange(10, 20)).second);
  EXPECT_TRUE(map.Emplace(AddressRange(0, 10)).second);
  EXPECT_TRUE(map.Emplace(AddressRange(20, 30)).second);

  EXPECT_THAT(map, SizeIs(3));
}

TEST(AddressRangeMap, FindAddress) {
  AddressRangeMap<int> map;
  map.Emplace(AddressRange(0, 10), 0);
  map.Emplace(AddressRange(10, 20), 1);
  map.Emplace(AddressRange(25, 30), 2);

  ASSERT_THAT(map.Find(0), Ne(map.end()));
  EXPECT_THAT(map.Find(0)->second, Eq(0));

  ASSERT_THAT(map.Find(9), Ne(map.end()));
  EXPECT_THAT(map.Find(9)->second, Eq(0));

  ASSERT_THAT(map.Find(10), Ne(map.end()));
  EXPECT_THAT(map.Find(10)->second, Eq(1));

  ASSERT_THAT(map.Find(10), Ne(map.end()));
  EXPECT_THAT(map.Find(19)->second, Eq(1));

  EXPECT_THAT(map.Find(20), Eq(map.end()));

  EXPECT_THAT(map.Find(24), Eq(map.end()));

  ASSERT_THAT(map.Find(25), Ne(map.end()));
  EXPECT_THAT(map.Find(25)->second, Eq(2));

  ASSERT_THAT(map.Find(29), Ne(map.end()));
  EXPECT_THAT(map.Find(29)->second, Eq(2));

  EXPECT_THAT(map.Find(30), Eq(map.end()));
}

TEST(AddressRangeMap, FindRangeThatContains) {
  AddressRangeMap<int> map;
  const auto it_1 = map.Emplace(AddressRange(0, 10), 0).first;
  const auto it_2 = map.Emplace(AddressRange(10, 20), 1).first;
  const auto it_3 = map.Emplace(AddressRange(25, 30), 2).first;
  const auto end = map.end();

  EXPECT_THAT(map.FindRangeThatContains({0, 10}), Eq(it_1));
  EXPECT_THAT(map.FindRangeThatContains({0, 1}), Eq(it_1));
  EXPECT_THAT(map.FindRangeThatContains({3, 4}), Eq(it_1));
  EXPECT_THAT(map.FindRangeThatContains({9, 10}), Eq(it_1));

  EXPECT_THAT(map.FindRangeThatContains({10, 11}), Eq(it_2));
  EXPECT_THAT(map.FindRangeThatContains({11, 12}), Eq(it_2));
  EXPECT_THAT(map.FindRangeThatContains({19, 20}), Eq(it_2));
  EXPECT_THAT(map.FindRangeThatContains({10, 20}), Eq(it_2));

  EXPECT_THAT(map.FindRangeThatContains({25, 26}), Eq(it_3));
  EXPECT_THAT(map.FindRangeThatContains({26, 27}), Eq(it_3));
  EXPECT_THAT(map.FindRangeThatContains({29, 30}), Eq(it_3));
  EXPECT_THAT(map.FindRangeThatContains({25, 30}), Eq(it_3));

  EXPECT_THAT(map.FindRangeThatContains({9, 11}), Eq(end));
  EXPECT_THAT(map.FindRangeThatContains({20, 21}), Eq(end));
  EXPECT_THAT(map.FindRangeThatContains({24, 25}), Eq(end));
  EXPECT_THAT(map.FindRangeThatContains({14, 27}), Eq(end));
}

TEST(AddressRangeMap, DeleteOverlapsAndEmplace) {
  const AddressRangeMap<int> entries = []() {
    AddressRangeMap<int> map;
    map.Emplace(AddressRange(0, 10), 0);
    map.Emplace(AddressRange(10, 20), 1);
    map.Emplace(AddressRange(25, 30), 2);
    return map;
  }();
  auto entry = [](uint64_t start, uint64_t end, int value) {
    return std::make_pair(AddressRange(start, end), value);
  };

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {30, 100}, 5);
    EXPECT_THAT(deleted, ElementsAre());
    EXPECT_THAT(map, ElementsAre(entry(0, 10, 0), entry(10, 20, 1),
                                 entry(25, 30, 2), entry(30, 100, 5)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {9, 10}, 5);
    EXPECT_THAT(deleted, ElementsAre(AddressRange(0, 10)));
    EXPECT_THAT(
        map, ElementsAre(entry(9, 10, 5), entry(10, 20, 1), entry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {5, 11}, 5);
    EXPECT_THAT(deleted,
                ElementsAre(AddressRange(0, 10), AddressRange(10, 20)));
    EXPECT_THAT(map, ElementsAre(entry(5, 11, 5), entry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {5, 25}, 5);
    EXPECT_THAT(deleted,
                ElementsAre(AddressRange(0, 10), AddressRange(10, 20)));
    EXPECT_THAT(map, ElementsAre(entry(5, 25, 5), entry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {5, 31}, 5);
    EXPECT_THAT(deleted, ElementsAre(AddressRange(0, 10), AddressRange(10, 20),
                                     AddressRange(25, 30)));
    EXPECT_THAT(map, ElementsAre(entry(5, 31, 5)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {0, 100}, 5);
    EXPECT_THAT(deleted, ElementsAre(AddressRange(0, 10), AddressRange(10, 20),
                                     AddressRange(25, 30)));
    EXPECT_THAT(map, ElementsAre(entry(0, 100, 5)));
  }
}

// No need to test all cases as the impl calls the one with callback underneath
// and this has already been tested. This test is more about making sure the
// template code is correct and it can be instantiated.
TEST(AddressRangeMap, DeleteOverlapsAndEmplaceWithoutCallback) {
  auto entry = [](uint64_t start, uint64_t end, int value) {
    return std::make_pair(AddressRange(start, end), value);
  };
  AddressRangeMap<int> map;
  map.Emplace(AddressRange(0, 10), 0);
  map.Emplace(AddressRange(10, 20), 1);
  map.Emplace(AddressRange(25, 30), 2);

  std::vector<AddressRange> deleted;
  map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {5, 11}, 5);
  EXPECT_THAT(deleted, ElementsAre(AddressRange(0, 10), AddressRange(10, 20)));
  EXPECT_THAT(map, ElementsAre(entry(5, 11, 5), entry(25, 30, 2)));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
