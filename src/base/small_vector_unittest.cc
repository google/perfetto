/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/small_vector.h"

#include <tuple>
#include <utility>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

int g_instances = 0;

struct Obj {
  explicit Obj(size_t v = 0) : value(v) {
    EXPECT_FALSE(constructed);
    constructed = true;
    g_instances++;
  }

  ~Obj() {
    EXPECT_TRUE(constructed);
    g_instances--;
  }

  // Move operators.
  Obj(Obj&& other) noexcept {
    g_instances++;
    constructed = true;
    moved_into = true;
    value = other.value;
    other.moved_from = true;
    other.value = 0xffffffff - value;
  }

  Obj& operator=(Obj&& other) noexcept {
    this->~Obj();
    new (this) Obj(std::move(other));
    return *this;
  }

  // Copy operators.
  Obj(const Obj& other) {
    other.copied_from = true;
    g_instances++;
    constructed = true;
    copied_into = true;
    value = other.value;
  }

  Obj& operator=(const Obj& other) {
    this->~Obj();
    new (this) Obj(other);
    return *this;
  }

  uintptr_t addr = reinterpret_cast<uintptr_t>(this);
  bool constructed = false;
  size_t value = 0;
  bool moved_from = false;
  mutable bool copied_from = false;
  bool moved_into = false;
  bool copied_into = false;
};

TEST(SmallVectorTest, StaySmall) {
  SmallVector<Obj, 8> v;
  EXPECT_EQ(g_instances, 0);
  EXPECT_EQ(v.size(), 0u);
  EXPECT_TRUE(v.empty());
  EXPECT_EQ(v.begin(), v.end());

  for (size_t i = 1; i <= 8; i++) {
    v.emplace_back(i);
    EXPECT_EQ(g_instances, static_cast<int>(i));
    EXPECT_FALSE(v.empty());
    EXPECT_EQ(v.end(), v.begin() + i);
    EXPECT_EQ(v.back().value, i);
    EXPECT_EQ(v[static_cast<size_t>(i - 1)].value, i);
    EXPECT_EQ(v[static_cast<size_t>(i - 1)].value, i);
  }

  for (size_t i = 1; i <= 3; i++) {
    v.pop_back();
    EXPECT_EQ(g_instances, 8 - static_cast<int>(i));
  }

  v.clear();
  EXPECT_EQ(g_instances, 0);
}

TEST(SmallVectorTest, GrowOnHeap) {
  SmallVector<Obj, 4> v;
  for (size_t i = 0; i < 10; i++) {
    v.emplace_back(i);
    EXPECT_EQ(g_instances, static_cast<int>(i + 1));
    EXPECT_FALSE(v.empty());
    EXPECT_EQ(v.end(), v.begin() + i + 1);
    EXPECT_EQ(v[i].value, i);
  }

  // Do a second pass and check that the initial elements aren't corrupt.
  for (size_t i = 0; i < 10; i++) {
    EXPECT_EQ(v[i].value, i);
    EXPECT_TRUE(v[i].constructed);
  }

  // The first 4 elements must have been moved into because of the heap growth.
  for (size_t i = 0; i < 4; i++)
    EXPECT_TRUE(v[i].moved_into);
  EXPECT_FALSE(v.back().moved_into);
}

class SmallVectorTestP : public testing::TestWithParam<size_t> {};

TEST_P(SmallVectorTestP, MoveOperators) {
  size_t num_elements = GetParam();
  static constexpr size_t kInlineCapacity = 4;
  SmallVector<Obj, kInlineCapacity> v1;
  for (size_t i = 0; i < num_elements; i++)
    v1.emplace_back(i);

  SmallVector<Obj, kInlineCapacity> v2(std::move(v1));
  EXPECT_TRUE(v1.empty());
  EXPECT_EQ(v2.size(), num_elements);

  // Check that v2 (the moved into vector) is consistent.
  for (size_t i = 0; i < num_elements; i++) {
    EXPECT_EQ(v2[i].value, i);
    EXPECT_TRUE(v2[i].constructed);
    if (num_elements <= kInlineCapacity) {
      EXPECT_TRUE(v2[i].moved_into);
    }
  }

  // Check that v1 (the moved-from object) is still usable.
  EXPECT_EQ(v1.size(), 0u);

  for (size_t i = 0; i < num_elements; i++) {
    v1.emplace_back(1000 + i);
    EXPECT_EQ(v1.size(), i + 1);
  }

  EXPECT_NE(v1.data(), v2.data());

  for (size_t i = 0; i < num_elements; i++) {
    EXPECT_EQ(v1[i].value, 1000 + i);
    EXPECT_EQ(v2[i].value, i);
    EXPECT_TRUE(v1[i].constructed);
    EXPECT_FALSE(v1[i].moved_from);
  }

  // Now swap again using the move-assignment.

  v1 = std::move(v2);
  EXPECT_EQ(v1.size(), num_elements);
  EXPECT_TRUE(v2.empty());
  for (size_t i = 0; i < num_elements; i++) {
    EXPECT_EQ(v1[i].value, i);
    EXPECT_TRUE(v1[i].constructed);
  }

  { auto destroy = std::move(v1); }

  EXPECT_EQ(g_instances, 0);
}

TEST_P(SmallVectorTestP, CopyOperators) {
  size_t num_elements = GetParam();
  static constexpr size_t kInlineCapacity = 4;
  SmallVector<Obj, kInlineCapacity> v1;
  for (size_t i = 0; i < num_elements; i++)
    v1.emplace_back(i);

  SmallVector<Obj, kInlineCapacity> v2(v1);
  EXPECT_EQ(v1.size(), num_elements);
  EXPECT_EQ(v2.size(), num_elements);
  EXPECT_EQ(g_instances, static_cast<int>(num_elements * 2));

  for (size_t i = 0; i < num_elements; i++) {
    EXPECT_EQ(v1[i].value, i);
    EXPECT_TRUE(v1[i].copied_from);
    EXPECT_EQ(v2[i].value, i);
    EXPECT_TRUE(v2[i].copied_into);
  }

  // Now edit v2.
  for (size_t i = 0; i < num_elements; i++)
    v2[i].value = i + 100;
  EXPECT_EQ(g_instances, static_cast<int>(num_elements * 2));

  // Append some extra elements.
  for (size_t i = 0; i < num_elements; i++)
    v2.emplace_back(i + 200);
  EXPECT_EQ(g_instances, static_cast<int>(num_elements * 3));

  for (size_t i = 0; i < num_elements * 2; i++) {
    if (i < num_elements) {
      EXPECT_EQ(v1[i].value, i);
      EXPECT_EQ(v2[i].value, 100 + i);
    } else {
      EXPECT_EQ(v2[i].value, 200 + i - num_elements);
    }
  }

  v2.clear();
  EXPECT_EQ(g_instances, static_cast<int>(num_elements));
}

INSTANTIATE_TEST_SUITE_P(SmallVectorTest,
                         SmallVectorTestP,
                         testing::Values(2, 4, 7, 512));

}  // namespace
}  // namespace base
}  // namespace perfetto
