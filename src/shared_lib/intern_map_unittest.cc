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

#include "src/shared_lib/intern_map.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

TEST(InternMapTest, SmallValue) {
  const char kSmallValue[] = "A";
  InternMap iids;

  auto res1 = iids.FindOrAssign(/*type=*/0, kSmallValue, sizeof(kSmallValue));

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_NE(res1.iid, 0u);

  auto res2 = iids.FindOrAssign(/*type=*/0, kSmallValue, sizeof(kSmallValue));

  EXPECT_FALSE(res2.newly_assigned);
  EXPECT_EQ(res1.iid, res1.iid);
}

TEST(InternMapTest, BigValue) {
  const char kBigValue[] = "ABCDEFGHIJKLMNOP";
  InternMap iids;

  auto res1 = iids.FindOrAssign(/*type=*/0, kBigValue, sizeof(kBigValue));

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_NE(res1.iid, 0u);

  auto res2 = iids.FindOrAssign(/*type=*/0, kBigValue, sizeof(kBigValue));

  EXPECT_FALSE(res2.newly_assigned);
  EXPECT_EQ(res1.iid, res1.iid);
}

TEST(InternMapTest, TwoValuesSameType) {
  const char kValue1[] = "A";
  const char kValue2[] = "ABCDEFGHIJKLMNOP";
  InternMap iids;

  auto res1 = iids.FindOrAssign(/*type=*/0, kValue1, sizeof(kValue1));

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_NE(res1.iid, 0u);

  auto res2 = iids.FindOrAssign(/*type=*/0, kValue2, sizeof(kValue2));

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_NE(res1.iid, res2.iid);
}

TEST(InternMapTest, SameValueDifferentTypes) {
  const char kValue[] = "A";
  InternMap iids;

  auto res1 = iids.FindOrAssign(/*type=*/0, kValue, sizeof(kValue));

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_NE(res1.iid, 0u);

  auto res2 = iids.FindOrAssign(/*type=*/1, kValue, sizeof(kValue));

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_NE(res2.iid, 0u);
}

}  // namespace
}  // namespace perfetto
