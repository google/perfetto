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
  std::vector<void*> AllocateAll() {
    std::vector<void*> allocated;

    for (size_t i = 0; i < kCapacity; ++i) {
      auto* p = allocator_.Allocate();
      EXPECT_NE(p, nullptr);
      if (!allocated.empty()) {
        EXPECT_NE(p, allocated.front());
      }
      allocated.push_back(p);
    }

    return allocated;
  }

  void FreeAll(const std::vector<void*>& allocated) {
    for (auto* p : allocated) {
      allocator_.Free(p);
    }
  }

  void CheckPointersAreDistinct(const std::vector<void*>& pointers) const {
    auto sorted = std::vector<uintptr_t>{};
    for (const void* p : pointers) {
      sorted.push_back(reinterpret_cast<uintptr_t>(p));
    }

    std::sort(sorted.begin(), sorted.end());
    auto new_end = std::unique(sorted.begin(), sorted.end());
    bool has_duplicates = new_end != sorted.cend();
    EXPECT_FALSE(has_duplicates);
  }

  void CheckPointersAlignment(const std::vector<void*> pointers) const {
    for (const void* p : pointers) {
      uintptr_t i = reinterpret_cast<uintptr_t>(p);
      EXPECT_TRUE(i % alignof(ElementType) == 0);
    }
  }

  // Slab capacity and total capacity chosen to internally allocate two slabs
  // in order to test allocations across multiple slabs.
  static constexpr size_t kSlabCapacity = 64;
  static constexpr size_t kMaxSlabs = 4;
  static constexpr size_t kCapacity = kMaxSlabs * kSlabCapacity;

  struct ElementType {
    alignas(32) std::byte buf[32];
  };
  using AllocatorType =
      SlabAllocator<sizeof(ElementType), alignof(ElementType), kSlabCapacity>;

  AllocatorType allocator_{};
};

TEST_F(SlabAllocatorTest, AllocatesDeallocates) {
  std::vector<void*> allocated = AllocateAll();
  CheckPointersAreDistinct(allocated);
  CheckPointersAlignment(allocated);

  FreeAll(allocated);

  allocated = AllocateAll();
  CheckPointersAreDistinct(allocated);
  CheckPointersAlignment(allocated);

  // Free in different (reverse) order
  std::reverse(allocated.begin(), allocated.end());
  FreeAll(allocated);

  allocated = AllocateAll();
  CheckPointersAreDistinct(allocated);
  CheckPointersAlignment(allocated);

  FreeAll(allocated);
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
