/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/tracing/core/id_allocator.h"

#include "gtest/gtest.h"

namespace perfetto {
namespace {

TEST(IdAllocatorTest, IdAllocation) {
  using IdType = IdAllocator::IdType;
  const IdType kMaxId = 1024;
  IdAllocator id_allocator(kMaxId);

  for (int repetition = 0; repetition < 3; repetition++) {
    std::set<IdType> ids;
    for (IdType i = 0; i < kMaxId - 1; i++) {
      auto id = id_allocator.Allocate();
      EXPECT_NE(0u, id);
      ASSERT_EQ(0u, ids.count(id));
      ids.insert(id);
    }

    // A further call should fail as we exhausted IDs.
    ASSERT_EQ(0u, id_allocator.Allocate());

    // Removing one ID should be enough to make room for another one.
    for (int i = 0; i < 3; i++) {
      id_allocator.Free(42);
      auto id = id_allocator.Allocate();
      ASSERT_EQ(42u, id);
    }

    // Remove the IDs at the boundaries and saturate again.
    id_allocator.Free(1);
    id_allocator.Free(kMaxId - 1);
    ASSERT_EQ(kMaxId - 1, id_allocator.Allocate());
    ASSERT_EQ(1u, id_allocator.Allocate());

    // Should be saturated again.
    ASSERT_EQ(0u, id_allocator.Allocate());

    // Release IDs in reverse order.
    for (IdType i = 0; i < kMaxId - 1; i++)
      id_allocator.Free(kMaxId - 1 - i);
  }
}

}  // namespace
}  // namespace perfetto
