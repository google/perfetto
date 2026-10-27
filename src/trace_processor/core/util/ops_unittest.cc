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

#include "src/trace_processor/core/util/ops.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::ops {
namespace {

TEST(OpsTest, GatherRows) {
  const int64_t source[] = {10, 20, 30, 40};
  const uint32_t rows[] = {3, 1};
  int64_t output[2];
  GatherRows(MakeSpan(source), MakeMutableSpan(output), MakeSpan(rows));
  EXPECT_THAT(output, testing::ElementsAre(40, 20));
}

TEST(OpsTest, GatherNullableRows) {
  const int64_t source[] = {10, 20, 30};
  BitVector source_non_null = BitVector::CreateWithSize(3, true);
  source_non_null.clear(1);
  const uint32_t rows[] = {1, 2};
  int64_t output[2];
  BitVector output_non_null = BitVector::CreateWithSize(2, false);
  GatherNullableRows(MakeSpan(source), source_non_null, MakeMutableSpan(output),
                     &output_non_null, MakeSpan(rows));
  EXPECT_FALSE(output_non_null.is_set(0));
  EXPECT_TRUE(output_non_null.is_set(1));
  EXPECT_EQ(output[1], 30);
}

TEST(OpsTest, GatherRowsInPlace) {
  int64_t values[] = {10, 20, 30, 40};
  const uint32_t rows[] = {1, 3};
  GatherRows(MakeSpan(values), MakeMutableSpan(values), MakeSpan(rows));
  EXPECT_EQ(values[0], 20);
  EXPECT_EQ(values[1], 40);
}

TEST(OpsTest, GatherNullableRowsInPlace) {
  int64_t values[] = {10, 20, 30, 40};
  BitVector non_null = BitVector::CreateWithSize(4, false);
  non_null.set(1);
  non_null.set(3);
  const uint32_t rows[] = {1, 2};

  GatherNullableRows(MakeSpan(values), non_null, MakeMutableSpan(values),
                     &non_null, MakeSpan(rows));

  EXPECT_EQ(values[0], 20);
  EXPECT_TRUE(non_null.is_set(0));
  EXPECT_FALSE(non_null.is_set(1));
}

TEST(OpsTest, EstimateDistinctCount) {
  base::FlatHashMap<int64_t, uint32_t> counts;
  const int32_t values[] = {1, 2, 1, 3};
  EXPECT_EQ(EstimateDistinctCount(&counts, MakeSpan(values)), 3u);

  const StringPool::Id strings[] = {
      StringPool::Id::Raw(1), StringPool::Id::Raw(2), StringPool::Id::Raw(1),
      StringPool::Id::Raw(2)};
  EXPECT_EQ(EstimateDistinctCount(&counts, MakeSpan(strings)), 2u);
}

TEST(OpsTest, DistinctRows) {
  const uint32_t rows[] = {1, 2, 1, 3};
  uint32_t indices[] = {10, 20, 30, 40};
  Span<uint32_t> index_span = MakeMutableSpan(indices);
  DistinctRows(AsBytes(MakeSpan(rows)), sizeof(uint32_t), &index_span);
  EXPECT_THAT(index_span, testing::ElementsAre(10u, 20u, 40u));
}

TEST(OpsTest, SortRowLayoutStably) {
  const uint32_t rows[] = {2, 1, 2};
  uint32_t indices[] = {10, 20, 30};
  Span<uint32_t> index_span = MakeMutableSpan(indices);
  SortRowLayout(AsBytes(MakeSpan(rows)), sizeof(uint32_t), &index_span);
  EXPECT_THAT(index_span, testing::ElementsAre(20u, 10u, 30u));
}

// Against a stable sort comparing whole rows, at sizes sorted by comparison and
// by radix, with few and many bytes differing between rows, on input in no
// order, in order, in a few ascending runs whose values tie across them, in
// reverse with and without ties, or all alike. The indices are not in order,
// so ties must keep their rows' order, not their indices'.
TEST(OpsTest, SortRowLayoutMatchesAStableSort) {
  enum class Order {
    kShuffled,
    kSorted,
    kRuns,
    kReversed,
    kReversedTies,
    kAlike
  };
  for (uint32_t rows : {2u, 30u, 100u, 1000u, 5000u, 70000u}) {
    for (uint32_t stride : {3u, 9u, 17u}) {
      for (Order order :
           {Order::kShuffled, Order::kSorted, Order::kRuns, Order::kReversed,
            Order::kReversedTies, Order::kAlike}) {
        std::vector<uint8_t> buffer(static_cast<size_t>(rows) * stride);
        for (uint32_t r = 0; r < rows; ++r) {
          uint64_t value = 0;
          switch (order) {
            case Order::kShuffled:
              value = (r * 2654435761u) % 97;
              break;
            case Order::kSorted:
              value = r / 3;
              break;
            case Order::kRuns:
              value = (r % ((rows + 4) / 5)) / 2;
              break;
            case Order::kReversed:
              value = rows - r;
              break;
            case Order::kReversedTies:
              value = (rows - r) / 2;
              break;
            case Order::kAlike:
              value = 7;
              break;
          }
          // Big-endian in the last bytes, so rows compare as their values do,
          // with every byte before them a function of the value too.
          for (uint32_t b = 0; b < stride; ++b) {
            uint32_t from_end = stride - 1 - b;
            buffer[static_cast<size_t>(r) * stride + b] =
                from_end < 4 ? static_cast<uint8_t>(value >> (from_end * 8))
                             : static_cast<uint8_t>(value / 50);
          }
        }
        std::vector<uint32_t> input(rows);
        for (uint32_t r = 0; r < rows; ++r) {
          input[r] = (rows - r) * 3;
        }
        std::vector<uint32_t> order_of(rows);
        for (uint32_t r = 0; r < rows; ++r) {
          order_of[r] = r;
        }
        std::stable_sort(
            order_of.begin(), order_of.end(), [&](uint32_t a, uint32_t b) {
              return memcmp(&buffer[static_cast<size_t>(a) * stride],
                            &buffer[static_cast<size_t>(b) * stride],
                            stride) < 0;
            });
        std::vector<uint32_t> expected(rows);
        for (uint32_t r = 0; r < rows; ++r) {
          expected[r] = input[order_of[r]];
        }
        std::vector<uint32_t> indices = input;
        Span<uint32_t> span(indices.data(), indices.data() + rows);
        SortRowLayout(
            Span<const uint8_t>(buffer.data(), buffer.data() + buffer.size()),
            stride, &span);
        EXPECT_EQ(indices, expected) << "rows=" << rows << " stride=" << stride
                                     << " order=" << static_cast<int>(order);
      }
    }
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core::ops
