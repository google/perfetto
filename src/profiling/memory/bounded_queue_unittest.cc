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

#include "src/profiling/memory/bounded_queue.h"

#include "gtest/gtest.h"

#include <thread>

namespace perfetto {
namespace profiling {
namespace {

TEST(BoundedQueueTest, IsFIFO) {
  BoundedQueue<int> q(2);
  q.Add(1);
  q.Add(2);
  int out;
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 1);
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 2);
  q.Shutdown();
}

TEST(BoundedQueueTest, BlockingAdd) {
  BoundedQueue<int> q(2);
  q.Add(1);
  q.Add(2);
  std::thread th([&q] { q.Add(3); });
  int out;
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 1);
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 2);
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 3);
  th.join();
  q.Shutdown();
}

TEST(BoundedQueueTest, BlockingGet) {
  BoundedQueue<int> q(2);
  std::thread th([&q] {
    int out;
    EXPECT_TRUE(q.Get(&out));
    EXPECT_EQ(out, 1);
  });
  q.Add(1);
  th.join();
  q.Shutdown();
}

TEST(BoundedQueueTest, Resize) {
  BoundedQueue<int> q(2);
  q.Add(1);
  q.Add(2);
  q.SetCapacity(3);
  q.Add(3);
  int out;
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 1);
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 2);
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 3);
  q.Shutdown();
}

TEST(BoundedQueueTest, Shutdown) {
  BoundedQueue<int> q(3);
  q.Add(1);
  q.Add(2);
  q.Add(3);
  int out;
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 1);
  EXPECT_TRUE(q.Get(&out));
  EXPECT_EQ(out, 2);
  q.Shutdown();
  EXPECT_FALSE(q.Get(&out));
}

TEST(BoundedQueueTest, ShutdownBlockingAdd) {
  BoundedQueue<int> q(2);
  q.Add(1);
  q.Add(2);
  std::thread th([&q] { EXPECT_FALSE(q.Add(3)); });
  q.Shutdown();
  th.join();
}

TEST(BoundedQueueTest, ShutdownBlockingGet) {
  BoundedQueue<int> q(1);
  std::thread th([&q] {
    int out;
    EXPECT_FALSE(q.Get(&out));
  });

  q.Shutdown();
  th.join();
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
