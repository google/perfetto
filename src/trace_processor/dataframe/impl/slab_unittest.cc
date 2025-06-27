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

#include "src/trace_processor/dataframe/impl/slab.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe::impl {
namespace {

// Test that the default constructor creates an empty slab
TEST(SlabTest, DefaultConstructor) {
  Slab<int> slab;
  EXPECT_EQ(slab.size(), 0u);
  EXPECT_EQ(slab.data(), nullptr);
}

// Test that Alloc creates a slab of the requested size
TEST(SlabTest, AllocationWithSize) {
  constexpr size_t kSize = 10;
  auto slab = Slab<int>::Alloc(kSize);

  EXPECT_EQ(slab.size(), kSize);
  EXPECT_NE(slab.data(), nullptr);
}

// Test element access and modification
TEST(SlabTest, ElementAccessAndModification) {
  constexpr size_t kSize = 5;
  auto slab = Slab<int>::Alloc(kSize);

  // Initialize elements
  for (size_t i = 0; i < kSize; ++i) {
    slab[i] = static_cast<int>(i * 100);
  }

  // Verify elements
  for (size_t i = 0; i < kSize; ++i) {
    EXPECT_EQ(slab[i], static_cast<int>(i * 100));
  }

  // Modify elements
  slab[2] = 42;
  EXPECT_EQ(slab[2], 42);
}

// Test move constructor
TEST(SlabTest, MoveConstructor) {
  constexpr size_t kSize = 5;
  auto slab1 = Slab<int>::Alloc(kSize);

  // Initialize elements
  for (size_t i = 0; i < kSize; ++i) {
    slab1[i] = static_cast<int>(i);
  }

  // Move construct
  Slab<int> slab2(std::move(slab1));

  // New slab should have the data
  EXPECT_EQ(slab2.size(), kSize);
  for (size_t i = 0; i < kSize; ++i) {
    EXPECT_EQ(slab2[i], static_cast<int>(i));
  }
}

// Test move assignment
TEST(SlabTest, MoveAssignment) {
  constexpr size_t kSize1 = 5;
  constexpr size_t kSize2 = 3;

  auto slab1 = Slab<int>::Alloc(kSize1);
  auto slab2 = Slab<int>::Alloc(kSize2);

  // Initialize elements
  for (size_t i = 0; i < kSize1; ++i) {
    slab1[i] = static_cast<int>(i + 10);
  }

  for (size_t i = 0; i < kSize2; ++i) {
    slab2[i] = static_cast<int>(i + 20);
  }

  // Move assign
  slab2 = std::move(slab1);

  // Target slab should have new data
  EXPECT_EQ(slab2.size(), kSize1);
  for (size_t i = 0; i < kSize1; ++i) {
    EXPECT_EQ(slab2[i], static_cast<int>(i + 10));
  }
}

// Test range-based for loop iteration
TEST(SlabTest, RangeBasedForLoop) {
  constexpr size_t kSize = 5;
  auto slab = Slab<int>::Alloc(kSize);

  // Initialize elements
  for (size_t i = 0; i < kSize; ++i) {
    slab[i] = static_cast<int>(i + 1);
  }

  // Use range-based for loop to sum values
  int sum = 0;
  for (int value : slab) {
    sum += value;
  }

  // Sum should be 1+2+3+4+5 = 15
  EXPECT_EQ(sum, 15);
}

// Test with different data types
TEST(SlabTest, DifferentDataTypes) {
  // Test with a larger type
  struct LargeType {
    double values[16];
  };

  auto slab = Slab<LargeType>::Alloc(5);
  EXPECT_EQ(slab.size(), 5u);

  // Test we can modify the data
  LargeType item;
  for (int i = 0; i < 16; ++i) {
    item.values[i] = static_cast<double>(i);
  }

  slab[2] = item;

  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(slab[2].values[i], static_cast<double>(i));
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe::impl
