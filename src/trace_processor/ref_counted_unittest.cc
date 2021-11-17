/*
 * Copyright (C) 2018 The Android Open Source Project
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
#include "perfetto/trace_processor/ref_counted.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

int g_instances = 0;

class RObj : public RefCounted {
 public:
  RObj() { ++g_instances; }
  ~RObj() { --g_instances; }
};

TEST(RefCountedTest, CreateAndReset) {
  RefPtr<RObj> ptr;
  EXPECT_FALSE(ptr);
  EXPECT_EQ(ptr.get(), nullptr);

  g_instances = 0;

  for (int i = 0; i < 3; i++) {
    ptr.reset(new RObj());
    EXPECT_TRUE(ptr);
    EXPECT_NE(ptr.get(), nullptr);
    EXPECT_EQ(g_instances, 1);
  }

  ptr.reset();
  EXPECT_EQ(g_instances, 0);
  EXPECT_FALSE(ptr);

  ptr.reset(new RObj());
  ptr.reset(nullptr);
  EXPECT_EQ(g_instances, 0);
  EXPECT_FALSE(ptr);

  // Test RAII.
  {
    RefPtr<RObj> ptr1(new RObj());
    EXPECT_EQ(g_instances, 1);
    {
      RefPtr<RObj> ptr2(new RObj());
      EXPECT_EQ(g_instances, 2);
    }
    EXPECT_EQ(g_instances, 1);
  }
  EXPECT_EQ(g_instances, 0);
}

TEST(RefCountedTest, CopyOperators) {
  g_instances = 0;

  RefPtr<RObj> x1(new RObj());
  RefPtr<RObj> y1(new RObj());
  EXPECT_EQ(g_instances, 2);

  auto x2 = x1;
  EXPECT_EQ(g_instances, 2);

  auto y2 = y1;
  EXPECT_EQ(g_instances, 2);

  EXPECT_EQ(x1.get(), x2.get());
  EXPECT_EQ(&*y1, &*y2);

  x1.reset();
  y2.reset();
  EXPECT_EQ(g_instances, 2);

  x2.reset();
  EXPECT_EQ(g_instances, 1);

  y1 = x2;
  EXPECT_EQ(g_instances, 0);

  {
    RefPtr<RObj> nested1(new RObj());
    EXPECT_EQ(g_instances, 1);
    {
      RefPtr<RObj> nested2(new RObj());
      EXPECT_EQ(g_instances, 2);
      nested1 = nested2;
      EXPECT_EQ(g_instances, 1);
    }
    EXPECT_EQ(g_instances, 1);
  }
  EXPECT_EQ(g_instances, 0);
}

TEST(RefCountedTest, MoveOperators) {
  g_instances = 0;

  RefPtr<RObj> x1(new RObj());
  RefPtr<RObj> y1(new RObj());
  EXPECT_EQ(g_instances, 2);

  auto x2 = std::move(x1);
  EXPECT_EQ(g_instances, 2);
  EXPECT_FALSE(x1);

  auto y2 = std::move(y1);
  EXPECT_EQ(g_instances, 2);
  EXPECT_FALSE(y1);

  // Test recycling.
  x1 = RefPtr<RObj>(new RObj());
  EXPECT_EQ(g_instances, 3);

  // y1 is still null;
  y2 = std::move(y1);
  EXPECT_FALSE(y1);
  EXPECT_FALSE(y2);
  EXPECT_EQ(g_instances, 2);  // y2 goes away.

  // We are left with x1 and x2.
  EXPECT_TRUE(x1);
  EXPECT_TRUE(x2);
  EXPECT_NE(&*x1, &*x2);

  x1 = std::move(x2);  // Now only x1 is left.
  EXPECT_EQ(g_instances, 1);
  EXPECT_FALSE(x2);

  x1 = std::move(x2);
  EXPECT_EQ(g_instances, 0);

  {
    RefPtr<RObj> nested1(new RObj());
    EXPECT_EQ(g_instances, 1);
    {
      RefPtr<RObj> nested2(new RObj());
      EXPECT_EQ(g_instances, 2);
      nested1 = std::move(nested2);
      EXPECT_EQ(g_instances, 1);
    }
    EXPECT_EQ(g_instances, 1);
  }
  EXPECT_EQ(g_instances, 0);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
