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

#include "src/trace_processor/dataframe/impl/flex_vector.h"

#include <cstddef>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe::impl {
namespace {

// Test that the default constructor creates an empty vector
TEST(FlexVectorTest, DefaultConstructor) {
  FlexVector<int> vec;
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_EQ(vec.capacity(), 0u);
  EXPECT_TRUE(vec.empty());
}

// Test basic push_back functionality
TEST(FlexVectorTest, PushBack) {
  auto vec = FlexVector<int>::CreateWithCapacity(8);

  // Add a few elements
  vec.push_back(42);
  vec.push_back(123);
  vec.push_back(7);

  EXPECT_EQ(vec.size(), 3u);
  EXPECT_FALSE(vec.empty());

  // Check values
  EXPECT_EQ(vec[0], 42);
  EXPECT_EQ(vec[1], 123);
  EXPECT_EQ(vec[2], 7);
}

// Test automatic capacity growth
TEST(FlexVectorTest, CapacityGrowth) {
  // Start with small capacity
  constexpr size_t kInitialCapacity = 64;
  auto vec = FlexVector<int>::CreateWithCapacity(kInitialCapacity);

  EXPECT_EQ(vec.capacity(), kInitialCapacity);

  // Fill to initial capacity
  for (size_t i = 0; i < kInitialCapacity; ++i) {
    vec.push_back(static_cast<int>(i));
  }

  EXPECT_EQ(vec.size(), kInitialCapacity);

  // Add one more element to trigger resize
  vec.push_back(100);

  // Capacity should have doubled
  EXPECT_GE(vec.capacity(), kInitialCapacity * 2);
  EXPECT_EQ(vec.size(), kInitialCapacity + 1);

  // Verify all elements preserved correctly after resize
  for (size_t i = 0; i < kInitialCapacity; ++i) {
    EXPECT_EQ(vec[i], static_cast<int>(i));
  }
  EXPECT_EQ(vec[kInitialCapacity], 100);
}

// Test that capacity always grows to at least 64
TEST(FlexVectorTest, MinimumCapacityGrowth) {
  // Start with capacity 1
  auto vec = FlexVector<int>::CreateWithCapacity(1);

  vec.push_back(42);
  EXPECT_EQ(vec.size(), 1u);

  // This should trigger growth to at least 64, not just doubling to 2
  vec.push_back(43);

  EXPECT_GE(vec.capacity(), 64u);
  EXPECT_EQ(vec[0], 42);
  EXPECT_EQ(vec[1], 43);
}

// Test large growth across multiple power-of-two boundaries
TEST(FlexVectorTest, LargeGrowth) {
  auto vec = FlexVector<int>::CreateWithCapacity(2);

  // Add many elements requiring multiple resizes
  constexpr size_t kNumElements = 1000;
  for (size_t i = 0; i < kNumElements; ++i) {
    vec.push_back(static_cast<int>(i));
  }

  EXPECT_EQ(vec.size(), kNumElements);
  EXPECT_GE(vec.capacity(), kNumElements);

  // Verify all elements are correct
  for (size_t i = 0; i < kNumElements; ++i) {
    EXPECT_EQ(vec[i], static_cast<int>(i));
  }
}

// Test using different data types
TEST(FlexVectorTest, DifferentDataTypes) {
  // Test with double
  {
    auto vec = FlexVector<double>::CreateWithCapacity(4);
    vec.push_back(3.14);
    vec.push_back(2.71);

    EXPECT_EQ(vec.size(), 2u);
    EXPECT_DOUBLE_EQ(vec[0], 3.14);
    EXPECT_DOUBLE_EQ(vec[1], 2.71);
  }

  // Test with a struct
  {
    struct Point {
      int x;
      int y;

      bool operator==(const Point& other) const {
        return x == other.x && y == other.y;
      }
    };

    auto vec = FlexVector<Point>::CreateWithCapacity(4);
    vec.push_back({1, 2});
    vec.push_back({3, 4});

    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec[0], (Point{1, 2}));
    EXPECT_EQ(vec[1], (Point{3, 4}));
  }
}

// Test iteration with range-based for loop
TEST(FlexVectorTest, RangeBasedForLoop) {
  auto vec = FlexVector<int>::CreateWithCapacity(8);

  // Add some elements
  vec.push_back(10);
  vec.push_back(20);
  vec.push_back(30);

  // Sum using range-based for
  int sum = 0;
  for (int value : vec) {
    sum += value;
  }

  EXPECT_EQ(sum, 60);
}

// Test data() accessor
TEST(FlexVectorTest, DataAccessor) {
  auto vec = FlexVector<int>::CreateWithCapacity(8);

  vec.push_back(1);
  vec.push_back(2);
  vec.push_back(3);

  // Use data() to access elements
  int* data = vec.data();
  EXPECT_EQ(data[0], 1);
  EXPECT_EQ(data[1], 2);
  EXPECT_EQ(data[2], 3);

  // Modify through data()
  data[1] = 42;
  EXPECT_EQ(vec[1], 42);
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe::impl
