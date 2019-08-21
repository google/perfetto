/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/db/bit_vector.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(BitVectorUnittest, Set) {
  BitVector bv(3, true);
  bv.Set(0, false);
  bv.Set(1, true);

  ASSERT_EQ(bv.size(), 3u);
  ASSERT_FALSE(bv.IsSet(0));
  ASSERT_TRUE(bv.IsSet(1));
  ASSERT_TRUE(bv.IsSet(2));
}

TEST(BitVectorUnittest, Append) {
  BitVector bv;
  bv.Append(true);
  bv.Append(false);

  ASSERT_EQ(bv.size(), 2u);
  ASSERT_TRUE(bv.IsSet(0));
  ASSERT_FALSE(bv.IsSet(1));
}

TEST(BitVectorUnittest, NextSet) {
  BitVector bv(6, false);
  bv.Set(1, true);
  bv.Set(2, true);
  bv.Set(4, true);

  ASSERT_EQ(bv.NextSet(0), 1u);
  ASSERT_EQ(bv.NextSet(1), 1u);
  ASSERT_EQ(bv.NextSet(2), 2u);
  ASSERT_EQ(bv.NextSet(3), 4u);
  ASSERT_EQ(bv.NextSet(4), 4u);
  ASSERT_EQ(bv.NextSet(5), 6u);
}

TEST(BitVectorUnittest, GetNumBitsSet) {
  BitVector bv(6, false);
  bv.Set(1, true);
  bv.Set(2, true);
  bv.Set(4, true);

  ASSERT_EQ(bv.GetNumBitsSet(), 3u);

  ASSERT_EQ(bv.GetNumBitsSet(0), 0u);
  ASSERT_EQ(bv.GetNumBitsSet(1), 0u);
  ASSERT_EQ(bv.GetNumBitsSet(2), 1u);
  ASSERT_EQ(bv.GetNumBitsSet(3), 2u);
  ASSERT_EQ(bv.GetNumBitsSet(4), 2u);
  ASSERT_EQ(bv.GetNumBitsSet(5), 3u);
  ASSERT_EQ(bv.GetNumBitsSet(6), 3u);
}

TEST(BitVectorUnittest, IndexOfNthSet) {
  BitVector bv(6, false);
  bv.Set(1, true);
  bv.Set(2, true);
  bv.Set(4, true);

  ASSERT_EQ(bv.IndexOfNthSet(0), 1u);
  ASSERT_EQ(bv.IndexOfNthSet(1), 2u);
  ASSERT_EQ(bv.IndexOfNthSet(2), 4u);
}

TEST(BitVectorUnittest, Resize) {
  BitVector bv(1, false);
  bv.Resize(2, true);
  bv.Resize(3, false);

  ASSERT_EQ(bv.IsSet(1), true);
  ASSERT_EQ(bv.IsSet(2), false);

  bv.Resize(2, false);

  ASSERT_EQ(bv.size(), 2u);
  ASSERT_EQ(bv.IsSet(1), true);
}

TEST(BitVectorUnittest, UpdateSetBits) {
  BitVector bv(6, false);
  bv.Set(1, true);
  bv.Set(2, true);
  bv.Set(4, true);

  BitVector picker(3u, true);
  picker.Set(1, false);

  bv.UpdateSetBits(picker);

  ASSERT_TRUE(bv.IsSet(1));
  ASSERT_FALSE(bv.IsSet(2));
  ASSERT_TRUE(bv.IsSet(4));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
