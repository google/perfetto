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

#ifndef SRC_TRACE_PROCESSOR_CONNECTION_POOL_H_
#define SRC_TRACE_PROCESSOR_CONNECTION_POOL_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"

namespace perfetto::trace_processor {

// Hands out PerfettoSqlConnections forked from an already-initialized primary
// connection so that multiple threads can run queries concurrently over a
// single shared, loaded database.
//
// The primary connection (and the StringPool backing it) is owned by the
// caller and must outlive the pool. Forks share the primary's database: its
// committed vtab state, its in-memory backing store and its StringPool. The
// contract for concurrent reads is that the database is fully loaded (every
// static table's Dataframe finalized) before the pool is used; the pool does
// not synchronize against mutations of the shared database.
//
// Connections are forked lazily up to |thread_count| and reused via a
// mutex-guarded free-list. Acquire a connection via |Acquire()|, which returns
// an RAII |Handle| that returns the connection to the pool on destruction.
class ConnectionPool {
 public:
  // RAII handle to a connection borrowed from the pool. Returns the connection
  // to the pool on destruction. Move-only.
  class Handle {
   public:
    Handle(ConnectionPool* pool, std::unique_ptr<PerfettoSqlConnection> conn)
        : pool_(pool), conn_(std::move(conn)) {}
    ~Handle();

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept
        : pool_(other.pool_), conn_(std::move(other.conn_)) {
      other.pool_ = nullptr;
    }
    Handle& operator=(Handle&&) = delete;

    PerfettoSqlConnection* get() const { return conn_.get(); }
    PerfettoSqlConnection* operator->() const { return conn_.get(); }
    PerfettoSqlConnection& operator*() const { return *conn_; }

   private:
    ConnectionPool* pool_;
    std::unique_ptr<PerfettoSqlConnection> conn_;
  };

  // |primary| is the already-initialized connection to fork from; it is *not*
  // owned by the pool and must outlive it. |pool| is the shared StringPool
  // backing |primary| (also owned by the caller). |thread_count| caps the
  // number of live forked connections; 0 means derive it from
  // std::thread::hardware_concurrency().
  ConnectionPool(PerfettoSqlConnection* primary,
                 StringPool* pool,
                 uint32_t thread_count);
  ~ConnectionPool();

  ConnectionPool(const ConnectionPool&) = delete;
  ConnectionPool& operator=(const ConnectionPool&) = delete;
  ConnectionPool(ConnectionPool&&) = delete;
  ConnectionPool& operator=(ConnectionPool&&) = delete;

  // Borrows a connection from the pool, forking a new one from the primary if
  // the free-list is empty and the count cap has not been reached. Blocks
  // (busy-spin free) only on the internal mutex, never on connection
  // availability: the count cap is sized so each calling thread gets its own.
  Handle Acquire();

  uint32_t thread_count() const { return thread_count_; }

 private:
  friend class Handle;

  // Returns a connection to the free-list. Called by ~Handle.
  void Release(std::unique_ptr<PerfettoSqlConnection> conn);

  PerfettoSqlConnection* const primary_;
  StringPool* const pool_;
  const uint32_t thread_count_;

  std::mutex mutex_;
  // Free-list of available forked connections, used as a stack.
  std::vector<std::unique_ptr<PerfettoSqlConnection>> free_list_;
  // Number of connections forked so far (in the free-list or handed out).
  uint32_t forked_count_ = 0;
  // True once we have called pool_->set_locking(true); guards the matching
  // set_locking(false) on destruction so we only flip back what we flipped.
  bool enabled_locking_ = false;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_CONNECTION_POOL_H_
