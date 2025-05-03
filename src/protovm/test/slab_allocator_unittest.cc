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

#include "test/gtest_and_gmock.h"

#include "src/protovm/slab_allocator.h"

namespace perfetto {
namespace protovm {
namespace test {

class SlabAllocatorTest : public ::testing::Test {
 protected:
  // Slab capacity and total capacity chosen to internally allocate two slabs
  // in order to test allocations across multiple slabs.
  static constexpr size_t kSlabCapacity = 64;
  static constexpr size_t kMaxSlabs = 2;
  static constexpr size_t kCapacity = kMaxSlabs * kSlabCapacity;

  struct ElementType {
    alignas(32) std::byte buf[32];
  };
  using AllocatorType =
      SlabAllocator<sizeof(ElementType), alignof(ElementType), kSlabCapacity>;

  AllocatorType allocator_{};
};

TEST_F(SlabAllocatorTest, AllocatesDeallocates) {
  std::vector<void*> allocated;
  for (size_t i = 0; i < kCapacity; ++i) {
    auto* p = allocator_.Allocate();
    ASSERT_NE(p, nullptr);
    if (!allocated.empty()) {
      ASSERT_NE(p, allocated.front());
    }
    allocated.push_back(p);
  }

  for (auto* p : allocated) {
    allocator_.Free(p);
  }

  std::vector<void*> reallocated;
  for (size_t i = 0; i < kCapacity; ++i) {
    auto* p = allocator_.Allocate();
    ASSERT_NE(p, nullptr);
    reallocated.push_back(p);
  }
  std::reverse(reallocated.begin(), reallocated.end());

  ASSERT_EQ(allocated, reallocated);
}

TEST_F(SlabAllocatorTest, RespectsAlignment) {
  for (size_t i = 0; i < kCapacity; ++i) {
    auto* p = allocator_.Allocate();
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(reinterpret_cast<size_t>(p) % alignof(ElementType),
              static_cast<size_t>(0));
  }
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
