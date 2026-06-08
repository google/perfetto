/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/connection_pool.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor {
namespace {

uint32_t ResolveThreadCount(uint32_t thread_count) {
  if (thread_count != 0)
    return thread_count;
  uint32_t hw = std::thread::hardware_concurrency();
  return hw == 0 ? 1 : hw;
}

}  // namespace

ConnectionPool::Handle::~Handle() {
  if (pool_)
    pool_->Release(std::move(conn_));
}

ConnectionPool::ConnectionPool(PerfettoSqlConnection* primary,
                               StringPool* pool,
                               uint32_t thread_count)
    : primary_(primary),
      pool_(pool),
      thread_count_(ResolveThreadCount(thread_count)) {
  PERFETTO_CHECK(primary_);
  PERFETTO_CHECK(pool_);
  // Forked connections read strings concurrently from the shared StringPool, so
  // locking must be enabled before any of them runs. Remember that we enabled
  // it so the destructor restores the prior state.
  pool_->set_locking(true);
  enabled_locking_ = true;
}

ConnectionPool::~ConnectionPool() {
  if (enabled_locking_)
    pool_->set_locking(false);
}

ConnectionPool::Handle ConnectionPool::Acquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!free_list_.empty()) {
    std::unique_ptr<PerfettoSqlConnection> conn = std::move(free_list_.back());
    free_list_.pop_back();
    return Handle(this, std::move(conn));
  }
  PERFETTO_CHECK(forked_count_ < thread_count_);
  ++forked_count_;
  // Fork() opens a new SQLite connection against the shared in-memory backing
  // store; SQLite's memdb VFS mutates shared per-database state at open time
  // and is not safe against concurrent opens, so this must hold |mutex_|.
  // This is only the lazy one-time fork; concurrent querying on already-forked
  // connections does not take this lock.
  return Handle(this, primary_->Fork());
}

void ConnectionPool::Release(std::unique_ptr<PerfettoSqlConnection> conn) {
  std::lock_guard<std::mutex> lock(mutex_);
  free_list_.push_back(std::move(conn));
}

}  // namespace perfetto::trace_processor
