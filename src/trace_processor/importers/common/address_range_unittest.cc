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

using ::testing::_;
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

MATCHER_P2(IteratorPointsTo, container, matcher, "") {
  return ExplainMatchResult(Ne(container.end()), arg, result_listener) &&
         ExplainMatchResult(matcher, *arg, result_listener);
}

template <typename Value>
auto MapEntry(uint64_t start, uint64_t end, Value value) {
  return Pair(AddressRange(start, end), value);
}

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

TEST(AddressRange, Overlap) {
  EXPECT_FALSE(AddressRange(0, 10).Overlaps(AddressRange(5, 5)));
  EXPECT_FALSE(AddressRange(5, 5).Overlaps(AddressRange(0, 10)));
  EXPECT_FALSE(AddressRange(0, 10).Overlaps(AddressRange(10, 20)));
  EXPECT_FALSE(AddressRange(10, 20).Overlaps(AddressRange(0, 10)));

  EXPECT_TRUE(AddressRange(0, 10).Overlaps(AddressRange(9, 10)));
  EXPECT_TRUE(AddressRange(10, 20).Overlaps(AddressRange(0, 11)));
  EXPECT_TRUE(AddressRange(0, 10).Overlaps(AddressRange(5, 6)));
  EXPECT_TRUE(AddressRange(0, 10).Overlaps(AddressRange(5, 20)));
}

TEST(AddressRangeMap, Empty) {
  AddressRangeMap<int> empty;
  EXPECT_THAT(empty, IsEmpty());
}

TEST(AddressRangeMap, EmplaceFailsForOverlaps) {
  AddressRangeMap<int> map;
  ASSERT_TRUE(map.Emplace(AddressRange(10, 20), 42));

  EXPECT_FALSE(map.Emplace(AddressRange(10, 20)));
  EXPECT_FALSE(map.Emplace(AddressRange(11, 19)));
  EXPECT_FALSE(map.Emplace(AddressRange(0, 11)));
  EXPECT_FALSE(map.Emplace(AddressRange(19, 30)));
  EXPECT_THAT(map, ElementsAre(MapEntry(10, 20, 42)));
}

TEST(AddressRangeMap, EmplaceSucceedsForNonOverlaps) {
  AddressRangeMap<int> map;

  EXPECT_TRUE(map.Emplace(AddressRange(10, 20)));
  EXPECT_TRUE(map.Emplace(AddressRange(0, 10)));
  EXPECT_TRUE(map.Emplace(AddressRange(20, 30)));

  EXPECT_THAT(map, SizeIs(3));
}

TEST(AddressRangeMap, EmplaceFailsForEmptyRange) {
  AddressRangeMap<int> map;

  EXPECT_FALSE(map.Emplace(AddressRange(0, 0)));
  EXPECT_FALSE(map.Emplace(AddressRange(100, 100)));

  EXPECT_THAT(map, IsEmpty());
}

TEST(AddressRangeMap, DeleteOverlapsAndEmplaceFailsForEmptyRange) {
  AddressRangeMap<int> map;
  EXPECT_TRUE(map.Emplace(AddressRange(0, 10), 42));
  EXPECT_FALSE(map.Emplace(AddressRange(0, 0)));
  EXPECT_FALSE(map.Emplace(AddressRange(100, 100)));

  EXPECT_THAT(map, ElementsAre(MapEntry(0, 10, 42)));
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
  map.Emplace(AddressRange(0, 10), 0);
  map.Emplace(AddressRange(10, 20), 1);
  map.Emplace(AddressRange(25, 30), 2);

  auto match_1 = IteratorPointsTo(map, MapEntry(0, 10, 0));
  auto match_2 = IteratorPointsTo(map, MapEntry(10, 20, 1));
  auto match_3 = IteratorPointsTo(map, MapEntry(25, 30, 2));

  EXPECT_THAT(map.FindRangeThatContains({0, 10}), match_1);
  EXPECT_THAT(map.FindRangeThatContains({0, 1}), match_1);
  EXPECT_THAT(map.FindRangeThatContains({3, 4}), match_1);
  EXPECT_THAT(map.FindRangeThatContains({9, 10}), match_1);

  EXPECT_THAT(map.FindRangeThatContains({10, 11}), match_2);
  EXPECT_THAT(map.FindRangeThatContains({11, 12}), match_2);
  EXPECT_THAT(map.FindRangeThatContains({19, 20}), match_2);
  EXPECT_THAT(map.FindRangeThatContains({10, 20}), match_2);

  EXPECT_THAT(map.FindRangeThatContains({25, 26}), match_3);
  EXPECT_THAT(map.FindRangeThatContains({26, 27}), match_3);
  EXPECT_THAT(map.FindRangeThatContains({29, 30}), match_3);
  EXPECT_THAT(map.FindRangeThatContains({25, 30}), match_3);

  EXPECT_THAT(map.FindRangeThatContains({9, 11}), Eq(map.end()));
  EXPECT_THAT(map.FindRangeThatContains({20, 21}), Eq(map.end()));
  EXPECT_THAT(map.FindRangeThatContains({24, 25}), Eq(map.end()));
  EXPECT_THAT(map.FindRangeThatContains({14, 27}), Eq(map.end()));
}

TEST(AddressRangeMap, TrimOverlapsAndEmplace) {
  const AddressRangeMap<int> entries = []() {
    AddressRangeMap<int> map;
    map.Emplace(AddressRange(0, 10), 0);
    map.Emplace(AddressRange(10, 20), 1);
    map.Emplace(AddressRange(25, 30), 2);
    return map;
  }();

  {
    AddressRangeMap<int> map = entries;
    map.TrimOverlapsAndEmplace({30, 100}, 5);
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 10, 0), MapEntry(10, 20, 1),
                                 MapEntry(25, 30, 2), MapEntry(30, 100, 5)));
  }

  {
    AddressRangeMap<int> map = entries;
    map.TrimOverlapsAndEmplace({9, 10}, 5);
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 9, 0), MapEntry(9, 10, 5),
                                 MapEntry(10, 20, 1), MapEntry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    map.TrimOverlapsAndEmplace({5, 11}, 5);
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 5, 0), MapEntry(5, 11, 5),
                                 MapEntry(11, 20, 1), MapEntry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    map.TrimOverlapsAndEmplace({5, 25}, 5);
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 5, 0), MapEntry(5, 25, 5),
                                 MapEntry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    map.TrimOverlapsAndEmplace({5, 31}, 5);
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 5, 0), MapEntry(5, 31, 5)));
  }

  {
    AddressRangeMap<int> map = entries;
    map.TrimOverlapsAndEmplace({0, 100}, 5);
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 100, 5)));
  }

  {
    AddressRangeMap<int> map = entries;
    map.TrimOverlapsAndEmplace({3, 7}, 5);
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 3, 0), MapEntry(3, 7, 5),
                                 MapEntry(7, 10, 0), MapEntry(10, 20, 1),
                                 MapEntry(25, 30, 2)));
  }
}

TEST(AddressRangeMap, DeleteOverlapsAndEmplace) {
  const AddressRangeMap<int> entries = []() {
    AddressRangeMap<int> map;
    map.Emplace(AddressRange(0, 10), 0);
    map.Emplace(AddressRange(10, 20), 1);
    map.Emplace(AddressRange(25, 30), 2);
    return map;
  }();

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {30, 100}, 5);
    EXPECT_THAT(deleted, ElementsAre());
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 10, 0), MapEntry(10, 20, 1),
                                 MapEntry(25, 30, 2), MapEntry(30, 100, 5)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {9, 10}, 5);
    EXPECT_THAT(deleted, ElementsAre(AddressRange(0, 10)));
    EXPECT_THAT(map, ElementsAre(MapEntry(9, 10, 5), MapEntry(10, 20, 1),
                                 MapEntry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {5, 11}, 5);
    EXPECT_THAT(deleted,
                ElementsAre(AddressRange(0, 10), AddressRange(10, 20)));
    EXPECT_THAT(map, ElementsAre(MapEntry(5, 11, 5), MapEntry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {5, 25}, 5);
    EXPECT_THAT(deleted,
                ElementsAre(AddressRange(0, 10), AddressRange(10, 20)));
    EXPECT_THAT(map, ElementsAre(MapEntry(5, 25, 5), MapEntry(25, 30, 2)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {5, 31}, 5);
    EXPECT_THAT(deleted, ElementsAre(AddressRange(0, 10), AddressRange(10, 20),
                                     AddressRange(25, 30)));
    EXPECT_THAT(map, ElementsAre(MapEntry(5, 31, 5)));
  }

  {
    AddressRangeMap<int> map = entries;
    std::vector<AddressRange> deleted;
    map.DeleteOverlapsAndEmplace(AppendRangesTo(deleted), {0, 100}, 5);
    EXPECT_THAT(deleted, ElementsAre(AddressRange(0, 10), AddressRange(10, 20),
                                     AddressRange(25, 30)));
    EXPECT_THAT(map, ElementsAre(MapEntry(0, 100, 5)));
  }
}

TEST(AddressRangeMap, ForOverlapsEmptyRangeDoesNothing) {
  AddressRangeMap<int> map;
  map.Emplace(AddressRange(0, 10), 0);
  map.Emplace(AddressRange(10, 20), 1);
  map.Emplace(AddressRange(25, 30), 2);

  MockFunction<void(AddressRangeMap<int>::value_type&)> cb;
  EXPECT_CALL(cb, Call).Times(0);

  map.ForOverlaps(AddressRange(5, 5), cb.AsStdFunction());
}

TEST(AddressRangeMap, ForOverlaps) {
  AddressRangeMap<int> map;
  map.Emplace(AddressRange(0, 10), 0);
  map.Emplace(AddressRange(10, 20), 1);
  map.Emplace(AddressRange(20, 30), 2);
  map.Emplace(AddressRange(35, 40), 3);
  map.Emplace(AddressRange(40, 50), 4);

  MockFunction<void(AddressRangeMap<int>::value_type&)> cb;
  EXPECT_CALL(cb, Call(MapEntry(10, 20, 1)));
  EXPECT_CALL(cb, Call(MapEntry(20, 30, 2)));
  EXPECT_CALL(cb, Call(MapEntry(35, 40, 3)));

  map.ForOverlaps(AddressRange(15, 36), cb.AsStdFunction());
}

TEST(AddressSet, Empty) {
  AddressSet empty;
  EXPECT_THAT(empty, ElementsAre());
}

TEST(AddressSet, EmptyRangesAreNotAdded) {
  AddressSet empty;

  empty.Add({0, 0});
  empty.Add({10, 10});

  EXPECT_THAT(empty, ElementsAre());
}

TEST(AddressSet, NonOverlapingNonContiguousAreNotMerged) {
  AddressSet set;
  set.Add({0, 10});
  set.Add({11, 20});

  EXPECT_THAT(set, ElementsAre(AddressRange(0, 10), AddressRange(11, 20)));
}

TEST(AddressSet, ContiguousAreMerged) {
  AddressSet set;
  set.Add({0, 10});
  set.Add({30, 40});
  set.Add({10, 30});

  EXPECT_THAT(set, ElementsAre(AddressRange(0, 40)));
}

TEST(AddressSet, OverlapsAreMerged) {
  AddressSet set;
  set.Add({0, 10});
  set.Add({30, 40});
  set.Add({5, 35});

  EXPECT_THAT(set, ElementsAre(AddressRange(0, 40)));
}

TEST(AddressSet, SpliceRemove) {
  AddressSet set;
  set.Add({0, 10});
  set.Remove({2, 5});

  EXPECT_THAT(set, ElementsAre(AddressRange(0, 2), AddressRange(5, 10)));
}

TEST(AddressSet, PartialRemove) {
  AddressSet set;
  set.Add({0, 10});
  set.Remove({0, 2});
  set.Remove({8, 10});

  EXPECT_THAT(set, ElementsAre(AddressRange(2, 8)));
}

TEST(AddressSet, MultipleRemove) {
  AddressSet set;
  set.Add({0, 10});
  set.Add({12, 15});
  set.Add({20, 30});
  set.Remove({5, 25});

  EXPECT_THAT(set, ElementsAre(AddressRange(0, 5), AddressRange(25, 30)));
}

TEST(AddressSet, RemoveEmptyRangeDoesNothing) {
  AddressSet set;
  set.Add({0, 10});
  set.Add({20, 30});

  set.Remove({0, 0});
  set.Remove({2, 2});
  set.Remove({10, 10});
  set.Remove({11, 11});

  EXPECT_THAT(set, ElementsAre(AddressRange(0, 10), AddressRange(20, 30)));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
