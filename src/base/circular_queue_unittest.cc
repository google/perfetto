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

#include "perfetto/ext/base/circular_queue.h"

#include <random>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using testing::ElementsAre;

TEST(CircularQueueTest, Int) {
  CircularQueue<int> queue(/*initial_capacity=*/1);
  ASSERT_EQ(queue.size(), 0u);
  queue.emplace_back(101);
  ASSERT_EQ(queue.size(), 1u);
  queue.emplace_back(102);
  queue.emplace_back(103);
  queue.emplace_back(104);
  ASSERT_EQ(queue.size(), 4u);

  auto it = queue.begin();
  for (int i = 101; i <= 104; i++) {
    ASSERT_EQ(*it, i);
    it++;
  }
  ASSERT_EQ(it, queue.end());

  queue.erase_front(1);
  ASSERT_EQ(queue.size(), 3u);
  ASSERT_EQ(*queue.begin(), 102);

  *(queue.begin() + 1) = 42;
  ASSERT_EQ(*(queue.end() - 2), 42);

  queue.erase_front(2);
  ASSERT_EQ(queue.size(), 1u);
  ASSERT_EQ(*queue.begin(), 104);

  queue.pop_front();
  ASSERT_EQ(queue.size(), 0u);
  ASSERT_EQ(queue.begin(), queue.end());

  const size_t kNumInts = 100000u;

  {
    std::minstd_rand0 rnd_engine(0);
    for (size_t i = 0; i < kNumInts; i++) {
      int n = static_cast<int>(rnd_engine());
      queue.emplace_back(n);
    }
  }
  ASSERT_EQ(queue.size(), kNumInts);
  ASSERT_EQ(static_cast<size_t>(queue.end() - queue.begin()), kNumInts);
  {
    std::minstd_rand0 rnd_engine(0);
    it = queue.begin();
    for (size_t i = 0; i < kNumInts; ++i, ++it) {
      ASSERT_LT(it, queue.end());
      ASSERT_EQ(*it, static_cast<int>(rnd_engine()));
    }
  }

  {
    std::minstd_rand0 del_rnd(42);
    std::minstd_rand0 rnd_engine(0);
    while (!queue.empty()) {
      ASSERT_EQ(*queue.begin(), static_cast<int>(rnd_engine()));
      size_t num_del = (del_rnd() % 8) + 1;
      queue.erase_front(num_del + 1);  // +1 because of the read 2 lines above.
      rnd_engine.discard(num_del);
    }
  }
}

TEST(CircularQueueTest, Sorting) {
  CircularQueue<uint64_t> queue;
  std::minstd_rand0 rnd_engine(0);
  for (int i = 0; i < 100000; i++) {
    queue.emplace_back(static_cast<uint64_t>(rnd_engine()));
    if ((i % 100) == 0)
      queue.erase_front(29);
  }
  ASSERT_FALSE(std::is_sorted(queue.begin(), queue.end()));
  std::sort(queue.begin(), queue.end());
  ASSERT_TRUE(std::is_sorted(queue.begin(), queue.end()));
}

TEST(CircularQueueTest, MoveOperators) {
  CircularQueue<int> queue;
  queue.emplace_back(1);
  queue.emplace_back(2);

  {
    CircularQueue<int> moved(std::move(queue));
    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(moved.size(), 2u);

    moved.emplace_back(3);
    moved.emplace_back(4);
    ASSERT_EQ(moved.size(), 4u);
  }
  queue.emplace_back(10);
  queue.emplace_back(11);
  queue.emplace_back(12);
  ASSERT_EQ(queue.size(), 3u);
  ASSERT_EQ(queue.front(), 10);
  ASSERT_EQ(queue.back(), 12);

  {
    CircularQueue<int> moved;
    moved.emplace_back(42);
    moved = std::move(queue);
    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(moved.size(), 3u);
    ASSERT_EQ(moved.size(), 3u);
    ASSERT_EQ(moved.front(), 10);
    ASSERT_EQ(moved.back(), 12);
  }
}

TEST(CircularQueueTest, Iterators) {
  for (size_t repeat = 1; repeat < 8; repeat++) {
    size_t capacity = 8 * (1 << repeat);
    CircularQueue<size_t> queue(capacity);
    for (size_t i = 0; i < capacity - 2; i++)
      queue.emplace_back(0u);
    queue.erase_front(queue.size());
    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(queue.capacity(), capacity);

    // Now the queue is empty and the internal write iterator is abut to wrap.

    // Add a bit more than half-capacity and check that the queue didn't resize.
    for (size_t i = 0; i < capacity / 2 + 3; i++)
      queue.emplace_back(i);
    ASSERT_EQ(queue.capacity(), capacity);

    // Check that all iterators are consistent.
    auto begin = queue.begin();
    auto end = queue.end();
    auto mid = begin + std::distance(begin, end) / 2;
    ASSERT_TRUE(std::is_sorted(begin, end));
    ASSERT_TRUE(begin < end);
    ASSERT_TRUE(begin <= begin);
    ASSERT_TRUE(begin >= begin);
    ASSERT_FALSE(begin < begin);
    ASSERT_FALSE(begin > begin);
    ASSERT_TRUE(begin + 1 > begin);
    ASSERT_TRUE(begin + 1 >= begin);
    ASSERT_FALSE(begin >= begin + 1);
    ASSERT_TRUE(begin <= begin);
    ASSERT_TRUE(begin <= begin + 1);
    ASSERT_TRUE(end > mid);
    ASSERT_TRUE(mid > begin);
    ASSERT_TRUE(std::is_sorted(begin, mid));
    ASSERT_TRUE(std::is_sorted(mid, end));
  }
}

TEST(CircularQueueTest, ReverseIterators) {
  CircularQueue<int> queue;
  ASSERT_EQ(queue.rbegin(), queue.rend());
  queue.emplace_back(1);

  ASSERT_EQ(*queue.rbegin(), 1);
  ASSERT_EQ(++queue.rbegin(), queue.rend());

  queue.emplace_back(2);
  queue.emplace_back(3);

  auto it = queue.rbegin();
  ASSERT_EQ(*(it++), 3);
  ASSERT_EQ(*(it++), 2);
  ASSERT_EQ(*(it++), 1);
  ASSERT_EQ(it, queue.rend());
}

TEST(CircularQueueTest, InsertBefore) {
  CircularQueue<int> queue;
  queue.InsertBefore(queue.end(), 20);
  ASSERT_THAT(queue, ElementsAre(20));

  queue.InsertBefore(queue.begin(), 10);
  ASSERT_THAT(queue, ElementsAre(10, 20));

  queue.InsertBefore(queue.end(), 40);
  ASSERT_THAT(queue, ElementsAre(10, 20, 40));

  queue.InsertBefore(queue.Find(40), 30);
  ASSERT_THAT(queue, ElementsAre(10, 20, 30, 40));

  queue.InsertBefore(queue.begin(), 0);
  ASSERT_THAT(queue, ElementsAre(0, 10, 20, 30, 40));

  // Now test InsertAfter(reverse_iterator). There is a catch here,
  // Insert on a reverse iterator actually inserts *after* the iterator.
  // As bad as it sound, this is consistent with what other STL containers do.
  // It's tied to the fact that std::reverse_iterator creates maps r == i-1.
  // From STL docs:
  // > For a reverse iterator r constructed from an iterator i, the relationship
  // > &*r == &*(i - 1) is always true (as long as r is dereferenceable); thus a
  // > reverse iterator constructed from a one-past-the-end iterator
  // > dereferences to the last element in a sequence.
  // ```
  queue.InsertAfter(queue.rbegin(), 60);
  ASSERT_THAT(queue, ElementsAre(0, 10, 20, 30, 40, 60));

  for (auto it = queue.rbegin(); it != queue.rend(); ++it) {
    if (*it == 40) {
      queue.InsertAfter(it, 50);
      break;
    }
  }
  ASSERT_THAT(queue, ElementsAre(0, 10, 20, 30, 40, 50, 60));

  // I know you don't believe me, so here's the proof.
  std::vector<int> v{10, 20};
  v.emplace(v.rbegin().base(), 40);
  ASSERT_THAT(v, ElementsAre(10, 20, 40));
  for (auto it = v.rbegin(); it != v.rend(); ++it) {
    if (*it == 20) {
      v.insert(it.base(), 30);
      break;
    }
  }
  ASSERT_THAT(v, ElementsAre(10, 20, 30, 40));
}

TEST(CircularQueueTest, InsertBeforeReverse) {
  CircularQueue<int> queue;

  std::array<int, 5> new_entries{4, 1, 5, 2, 3};
  for (int n : new_entries) {
    auto it = queue.rbegin();
    while (it != queue.rend() && n < *it) {
      ++it;
    }
    queue.InsertAfter(it, n);
  }
  ASSERT_THAT(queue, ElementsAre(1, 2, 3, 4, 5));
}

// Test that InsertBefore works correctly when it triggers a capacity growth.
// This verifies that the iterator's pos_ (which is an abstract index) remains
// valid after the internal storage is reallocated during Grow().
TEST(CircularQueueTest, InsertBeforeWithGrow) {
  // Use a small initial capacity so we can easily trigger growth.
  CircularQueue<int> queue(/*initial_capacity=*/4);

  // Fill the queue to capacity.
  queue.emplace_back(10);
  queue.emplace_back(20);
  queue.emplace_back(30);
  queue.emplace_back(40);
  ASSERT_EQ(queue.size(), 4u);
  ASSERT_EQ(queue.capacity(), 4u);

  // Get the position where we want to insert (at element 30).
  // Note: We can't use the iterator directly after InsertBefore because
  // increment_generation() invalidates it. We use pos_ which is accessed
  // directly by InsertBefore.
  auto it = queue.begin() + 2;
  ASSERT_EQ(*it, 30);

  // This InsertBefore should trigger Grow() since we're at capacity.
  // After Grow(), the iterator's pos_ (an abstract index) should still be
  // valid because CircularQueue uses monotonic indices, not pointers.
  queue.InsertBefore(it, 25);

  // Verify the queue grew.
  ASSERT_GT(queue.capacity(), 4u);
  ASSERT_EQ(queue.size(), 5u);

  // Verify all elements are in the correct order.
  ASSERT_THAT(queue, ElementsAre(10, 20, 25, 30, 40));
}

// Test InsertBefore with growth when the queue has wrapped around internally.
TEST(CircularQueueTest, InsertBeforeWithGrowAfterWrap) {
  CircularQueue<int> queue(/*initial_capacity=*/4);

  // Fill and then pop to move the internal begin_ forward.
  queue.emplace_back(1);
  queue.emplace_back(2);
  queue.pop_front();
  queue.pop_front();

  // Now internal begin_ is at position 2. Add elements to wrap around.
  queue.emplace_back(10);
  queue.emplace_back(20);
  queue.emplace_back(30);
  queue.emplace_back(40);
  ASSERT_EQ(queue.size(), 4u);
  ASSERT_EQ(queue.capacity(), 4u);

  // Insert in the middle, triggering growth while wrapped.
  auto it = queue.begin() + 2;
  ASSERT_EQ(*it, 30);
  queue.InsertBefore(it, 25);

  ASSERT_GT(queue.capacity(), 4u);
  ASSERT_EQ(queue.size(), 5u);
  ASSERT_THAT(queue, ElementsAre(10, 20, 25, 30, 40));
}

TEST(CircularQueueTest, ObjectLifetime) {
  class Checker {
   public:
    struct Stats {
      int num_ctors = 0;
      int num_dtors = 0;
      int num_alive = 0;
    };
    Checker(Stats* stats, int n) : stats_(stats), n_(n) {
      EXPECT_GE(n, 0);
      stats_->num_ctors++;
      stats_->num_alive++;
      ptr_ = reinterpret_cast<uintptr_t>(this);
    }

    ~Checker() {
      EXPECT_EQ(ptr_, reinterpret_cast<uintptr_t>(this));
      if (n_ >= 0)
        stats_->num_alive--;
      n_ = -1;
      stats_->num_dtors++;
    }

    Checker(Checker&& other) noexcept {
      n_ = other.n_;
      other.n_ = -1;
      stats_ = other.stats_;
      ptr_ = reinterpret_cast<uintptr_t>(this);
    }

    Checker& operator=(Checker&& other) {
      new (this) Checker(std::move(other));
      return *this;
    }

    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;

    Stats* stats_ = nullptr;
    uintptr_t ptr_ = 0;
    int n_ = -1;
  };

  // Check that dtors are invoked on old elements when growing capacity.
  Checker::Stats stats;
  {
    CircularQueue<Checker> queue(/*initial_capacity=*/2);
    for (int i = 0; i < 2; i++)
      queue.emplace_back(&stats, i);
    ASSERT_EQ(stats.num_ctors, 2);
    ASSERT_EQ(stats.num_dtors, 0);

    // This further insertion will grow the queue, causing two std::move()s
    // for the previous elements and one emplace.
    queue.emplace_back(&stats, 2);
    ASSERT_EQ(stats.num_ctors, 3);

    // The two old elements that have std::move()d should be destroyed upon
    // expansion.
    ASSERT_EQ(stats.num_dtors, 2);
  }
  ASSERT_EQ(stats.num_dtors, 2 + 3);

  stats = Checker::Stats();
  {
    CircularQueue<Checker> queue(/*initial_capacity=*/1);
    for (int i = 0; i < 5; i++)
      queue.emplace_back(&stats, i);
    ASSERT_EQ(stats.num_ctors, 5);
    Checker c5(&stats, 5);
    queue.emplace_back(std::move(c5));
    ASSERT_EQ(stats.num_alive, 5 + 1);

    queue.erase_front(2);
    ASSERT_EQ(stats.num_alive, 5 + 1 - 2);

    for (int i = 0; i < 4; i++)
      queue.emplace_back(&stats, 10 + i);
    ASSERT_EQ(stats.num_alive, 5 + 1 - 2 + 4);
  }
  ASSERT_EQ(stats.num_ctors, 5 + 1 + 4);
  ASSERT_EQ(stats.num_alive, 0);

  stats = Checker::Stats();
  {
    CircularQueue<Checker> q1(1);
    CircularQueue<Checker> q2(64);
    for (int i = 0; i < 100; i++) {
      q1.emplace_back(&stats, 1000 + i * 2);
      q2.emplace_back(&stats, 1001 + i * 2);
    }

    ASSERT_EQ(stats.num_alive, 200);

    for (int i = 0; i < 100; i += 2) {
      auto it1 = q1.begin() + i;
      auto it2 = q2.begin() + i;
      std::swap(*it1, *it2);
    }
    auto comparer = [](const Checker& lhs, const Checker& rhs) {
      return lhs.n_ < rhs.n_;
    };
    ASSERT_TRUE(std::is_sorted(q1.begin(), q1.end(), comparer));
    ASSERT_TRUE(std::is_sorted(q2.begin(), q2.end(), comparer));
    ASSERT_EQ(stats.num_alive, 200);
  }
}

}  // namespace
}  // namespace base
}  // namespace perfetto
