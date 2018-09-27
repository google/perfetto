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

#ifndef SRC_PROFILING_MEMORY_BOUNDED_QUEUE_H_
#define SRC_PROFILING_MEMORY_BOUNDED_QUEUE_H_

#include <condition_variable>
#include <deque>
#include <mutex>

#include "perfetto/base/logging.h"

// Transport messages between threads. Multiple-producer / single-consumer.
//
// This has to outlive both the consumer and the producer who have to
// negotiate termination separately, if needed. This is currently only used
// in a scenario where the producer and consumer both are loops that never
// terminate.
template <typename T>
class BoundedQueue {
 public:
  BoundedQueue() : BoundedQueue(1) {}
  BoundedQueue(size_t capacity) : capacity_(capacity) {
    PERFETTO_CHECK(capacity > 0);
  }

  void Add(T item) {
    std::unique_lock<std::mutex> l(mutex_);
    if (deque_.size() == capacity_)
      full_cv_.wait(l, [this] { return deque_.size() < capacity_; });
    deque_.emplace_back(std::move(item));
    if (deque_.size() == 1)
      empty_cv_.notify_all();
  }

  T Get() {
    std::unique_lock<std::mutex> l(mutex_);
    if (elements_ == 0)
      empty_cv_.wait(l, [this] { return !deque_.empty(); });
    T item(std::move(deque_.front()));
    deque_.pop_front();
    if (deque_.size() == capacity_ - 1) {
      l.unlock();
      full_cv_.notify_all();
    }
    return item;
  }

  void SetCapacity(size_t capacity) {
    PERFETTO_CHECK(capacity > 0);
    {
      std::lock_guard<std::mutex> l(mutex_);
      capacity_ = capacity;
    }
    full_cv_.notify_all();
  }

 private:
  size_t capacity_;
  size_t elements_ = 0;
  std::deque<T> deque_;
  std::condition_variable full_cv_;
  std::condition_variable empty_cv_;
  std::mutex mutex_;
};

#endif  // SRC_PROFILING_MEMORY_BOUNDED_QUEUE_H_
