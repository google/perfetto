/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/tracing/service/histogram.h"

#include <random>
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

TEST(HistogramTest, SingleBucket) {
  Histogram<8> h;
  h.Add(0);
  h.Add(1);
  h.Add(8);
  h.Add(10);
  EXPECT_EQ(h.GetBucketCount(0), 3u);
  EXPECT_EQ(h.GetBucketSum(0), 9);

  EXPECT_EQ(h.GetBucketCount(1), 1u);
  EXPECT_EQ(h.GetBucketSum(1), 10);
}

TEST(HistogramTest, ThreeBuckets) {
  Histogram<8, 16, 32> h;
  EXPECT_EQ(h.GetBucketThres(0), 8);
  EXPECT_EQ(h.GetBucketThres(1), 16);
  EXPECT_EQ(h.GetBucketThres(2), 32);
  for (size_t i = 0; i < h.num_buckets(); i++) {
    EXPECT_EQ(h.GetBucketCount(i), 0u);
    EXPECT_EQ(h.GetBucketSum(i), 0);
  }

  h.Add(4);
  h.Add(8);
  h.Add(15);
  EXPECT_EQ(h.GetBucketCount(0), 2u);
  EXPECT_EQ(h.GetBucketSum(0), 4 + 8);

  EXPECT_EQ(h.GetBucketCount(1), 1u);
  EXPECT_EQ(h.GetBucketSum(1), 15);

  EXPECT_EQ(h.GetBucketCount(2), 0u);
  EXPECT_EQ(h.GetBucketSum(2), 0);

  h.Add(17);
  h.Add(31);
  h.Add(32);
  EXPECT_EQ(h.GetBucketCount(2), 3u);
  EXPECT_EQ(h.GetBucketSum(2), 17 + 31 + 32);

  h.Add(1000);
  EXPECT_EQ(h.GetBucketCount(3), 1u);
  EXPECT_EQ(h.GetBucketSum(3), 1000);
}

TEST(HistogramTest, Merge) {
  Histogram<8, 16, 32> h, h2;
  h.Add(4);
  h.Add(15);
  h.Add(90);

  h2.Add(5);
  h2.Add(30);
  h2.Add(91);

  h.Merge(h2);
  EXPECT_EQ(h.GetBucketCount(0), 2u);
  EXPECT_EQ(h.GetBucketSum(0), 4 + 5);

  EXPECT_EQ(h.GetBucketCount(1), 1u);
  EXPECT_EQ(h.GetBucketSum(1), 15);

  EXPECT_EQ(h.GetBucketCount(2), 1u);
  EXPECT_EQ(h.GetBucketSum(2), 30);

  EXPECT_EQ(h.GetBucketCount(3), 2u);
  EXPECT_EQ(h.GetBucketSum(3), 90 + 91);
}

TEST(HistogramTest, CopyAndMoveOperators) {
  using HistType = Histogram<8, 16, 32>;
  HistType h1;
  h1.Add(1);
  h1.Add(15);
  h1.Add(30);
  h1.Add(31);
  h1.Add(99);

  auto check_validity = [](const HistType& h) {
    ASSERT_EQ(h.GetBucketSum(0), 1);
    ASSERT_EQ(h.GetBucketCount(0), 1u);
    ASSERT_EQ(h.GetBucketSum(1), 15);
    ASSERT_EQ(h.GetBucketCount(1), 1u);
    ASSERT_EQ(h.GetBucketSum(2), 30 + 31);
    ASSERT_EQ(h.GetBucketCount(2), 2u);
    ASSERT_EQ(h.GetBucketSum(3), 99);
    ASSERT_EQ(h.GetBucketCount(3), 1u);
  };
  check_validity(h1);

  HistType h2(h1);
  check_validity(h2);
  check_validity(h1);

  HistType h3 = h2;
  check_validity(h3);
  check_validity(h2);

  HistType h4(std::move(h3));
  check_validity(h4);

  HistType h5;
  h5 = std::move(h4);
  check_validity(h5);
}

TEST(HistogramTest, Rand) {
  std::minstd_rand0 rnd_engine(0);
  Histogram<10, 100, 1000> h;

  int64_t expected_sum = 0;
  const uint64_t expected_count = 1000;
  for (uint64_t i = 0; i < expected_count; i++) {
    auto value = static_cast<int32_t>(rnd_engine());
    expected_sum += value;
    h.Add(value);
  }

  int64_t actual_sum = 0;
  uint64_t actual_count = 0;
  for (size_t i = 0; i < h.num_buckets(); i++) {
    actual_count += h.GetBucketCount(i);
    actual_sum += h.GetBucketSum(i);
  }

  EXPECT_EQ(expected_count, actual_count);
  EXPECT_EQ(expected_sum, actual_sum);
}

}  // namespace
}  // namespace perfetto
