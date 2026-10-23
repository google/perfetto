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

#include "src/shared_lib/track_event/intern_map.h"

#include <stdio.h>
#include <string.h>

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

TEST(InternMapTest, StringSameAsBytes) {
  InternMap iids;

  auto res1 = iids.FindOrAssign(/*type=*/0, "name", strlen("name"));
  auto res2 = iids.FindOrAssignString(/*type=*/0, "name");
  auto res3 = iids.FindOrAssignString(/*type=*/0, "name");

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_FALSE(res2.newly_assigned);
  EXPECT_FALSE(res3.newly_assigned);
  EXPECT_EQ(res2.iid, res1.iid);
  EXPECT_EQ(res3.iid, res1.iid);
}

TEST(InternMapTest, StringReusedBuffer) {
  InternMap iids;
  char buf[16];

  // The same buffer holds a different string each time, so the cache keyed by
  // the pointer must not answer for the old content.
  snprintf(buf, sizeof(buf), "first");
  auto first = iids.FindOrAssignString(/*type=*/0, buf);
  snprintf(buf, sizeof(buf), "second");
  auto second = iids.FindOrAssignString(/*type=*/0, buf);
  snprintf(buf, sizeof(buf), "first");
  auto first_again = iids.FindOrAssignString(/*type=*/0, buf);

  EXPECT_TRUE(first.newly_assigned);
  EXPECT_TRUE(second.newly_assigned);
  EXPECT_NE(second.iid, first.iid);
  EXPECT_FALSE(first_again.newly_assigned);
  EXPECT_EQ(first_again.iid, first.iid);
}

TEST(InternMapTest, StringDifferentTypes) {
  InternMap iids;

  auto res1 = iids.FindOrAssignString(/*type=*/0, "name");
  auto res2 = iids.FindOrAssignString(/*type=*/1, "name");

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_TRUE(res2.newly_assigned);
}

TEST(InternMapTest, StringLongerThanCacheEntry) {
  const char kLongString[] = "a string longer than one entry of the cache";
  InternMap iids;

  auto res1 = iids.FindOrAssignString(/*type=*/0, kLongString);
  auto res2 = iids.FindOrAssignString(/*type=*/0, kLongString);

  EXPECT_TRUE(res1.newly_assigned);
  EXPECT_FALSE(res2.newly_assigned);
  EXPECT_EQ(res2.iid, res1.iid);
}

TEST(InternMapTest, StringAfterClear) {
  InternMap iids;

  iids.FindOrAssignString(/*type=*/0, "name");
  iids.Clear();
  auto res = iids.FindOrAssignString(/*type=*/0, "name");

  EXPECT_TRUE(res.newly_assigned);
}

}  // namespace
}  // namespace perfetto
