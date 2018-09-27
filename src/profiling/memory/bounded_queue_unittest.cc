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
namespace {

TEST(BoundedQueueTest, IsFIFO) {
  BoundedQueue<int> q(2);
  q.Add(1);
  q.Add(2);
  EXPECT_EQ(q.Get(), 1);
  EXPECT_EQ(q.Get(), 2);
}

TEST(BoundedQueueTest, Blocking) {
  BoundedQueue<int> q(2);
  q.Add(1);
  q.Add(2);
  std::thread th([&q] { q.Add(3); });
  EXPECT_EQ(q.Get(), 1);
  EXPECT_EQ(q.Get(), 2);
  EXPECT_EQ(q.Get(), 3);
  th.join();
}

TEST(BoundedQueueTest, Resize) {
  BoundedQueue<int> q(2);
  q.Add(1);
  q.Add(2);
  q.SetCapacity(3);
  q.Add(3);
  EXPECT_EQ(q.Get(), 1);
  EXPECT_EQ(q.Get(), 2);
  EXPECT_EQ(q.Get(), 3);
}

}  // namespace
}  // namespace perfetto
