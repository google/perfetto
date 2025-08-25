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

#include "perfetto/ext/base/bits.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(BitsTest, CountLeadZeros) {
  EXPECT_EQ(CountLeadZeros(uint32_t(0)), 32u);
  EXPECT_EQ(CountLeadZeros(uint32_t(1)), 31u);
  EXPECT_EQ(CountLeadZeros(uint32_t(2)), 30u);
  EXPECT_EQ(CountLeadZeros(uint32_t(3)), 30u);
  EXPECT_EQ(CountLeadZeros(uint32_t(255)), 24u);
  EXPECT_EQ(CountLeadZeros(uint32_t(256)), 23u);
  EXPECT_EQ(CountLeadZeros(UINT32_MAX), 0u);

  EXPECT_EQ(CountLeadZeros(uint64_t(0)), 64u);
  EXPECT_EQ(CountLeadZeros(uint64_t(1)), 63u);
  EXPECT_EQ(CountLeadZeros(uint64_t(2)), 62u);
  EXPECT_EQ(CountLeadZeros(uint64_t(3)), 62u);
  EXPECT_EQ(CountLeadZeros(uint64_t(255)), 56u);
  EXPECT_EQ(CountLeadZeros(uint64_t(256)), 55u);
  EXPECT_EQ(CountLeadZeros(UINT64_MAX), 0u);
}

TEST(BitsTest, CountTrailZeros) {
  EXPECT_EQ(CountTrailZeros(uint32_t(0)), 32u);
  EXPECT_EQ(CountTrailZeros(uint32_t(1)), 0u);
  EXPECT_EQ(CountTrailZeros(uint32_t(2)), 1u);
  EXPECT_EQ(CountTrailZeros(uint32_t(3)), 0u);
  EXPECT_EQ(CountTrailZeros(uint32_t(255)), 0u);
  EXPECT_EQ(CountTrailZeros(uint32_t(256)), 8u);
  EXPECT_EQ(CountTrailZeros(UINT32_MAX), 0u);

  EXPECT_EQ(CountTrailZeros(uint64_t(0)), 64u);
  EXPECT_EQ(CountTrailZeros(uint64_t(1)), 0u);
  EXPECT_EQ(CountTrailZeros(uint64_t(2)), 1u);
  EXPECT_EQ(CountTrailZeros(uint64_t(3)), 0u);
  EXPECT_EQ(CountTrailZeros(uint64_t(255)), 0u);
  EXPECT_EQ(CountTrailZeros(uint64_t(256)), 8u);
  EXPECT_EQ(CountTrailZeros(UINT64_MAX), 0u);
}
TEST(BitsTest, AllBitsSet) {
  EXPECT_TRUE(AllBitsSet(uint8_t(0xFF)));
  EXPECT_FALSE(AllBitsSet(uint16_t(0xFF)));
  EXPECT_TRUE(AllBitsSet(uint16_t(0xFFFF)));
  EXPECT_TRUE(AllBitsSet(uint32_t(0xFFFFFFFF)));
  EXPECT_TRUE(AllBitsSet(uint64_t(0xFFFFFFFFFFFFFFFF)));
  EXPECT_FALSE(AllBitsSet(uint64_t(0xFFFFFFFF)));
  EXPECT_TRUE(AllBitsSet(int32_t(-1)));
  EXPECT_TRUE(AllBitsSet(int64_t(-1)));
  EXPECT_FALSE(AllBitsSet(int32_t(-2)));
  EXPECT_FALSE(AllBitsSet(int64_t(-2)));
  EXPECT_FALSE(AllBitsSet(uint32_t(0)));
  EXPECT_FALSE(AllBitsSet(uint64_t(0)));
  EXPECT_FALSE(AllBitsSet(int32_t(0)));
  EXPECT_FALSE(AllBitsSet(int64_t(0)));
  EXPECT_FALSE(AllBitsSet(int32_t(1)));
  EXPECT_FALSE(AllBitsSet(int64_t(1)));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
