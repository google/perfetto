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

#ifndef SRC_PROFILING_MEMORY_CLIENT_H_
#define SRC_PROFILING_MEMORY_CLIENT_H_

#include <stddef.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "perfetto/base/unix_socket.h"
#include "src/profiling/memory/sampler.h"
#include "src/profiling/memory/wire_protocol.h"

namespace perfetto {
namespace profiling {

class BorrowedSocket;

class SocketPool {
 public:
  friend class BorrowedSocket;
  SocketPool(std::vector<base::UnixSocketRaw> sockets);

  BorrowedSocket Borrow();
  void Shutdown();

 private:
  bool shutdown_ = false;

  void Return(base::UnixSocketRaw);
  std::timed_mutex mutex_;
  std::condition_variable_any cv_;
  std::vector<base::UnixSocketRaw> sockets_;
  size_t available_sockets_;
  size_t dead_sockets_ = 0;
};

// Socket borrowed from a SocketPool. Gets returned once it goes out of scope.
class BorrowedSocket {
 public:
  BorrowedSocket(const BorrowedSocket&) = delete;
  BorrowedSocket& operator=(const BorrowedSocket&) = delete;
  BorrowedSocket(BorrowedSocket&& other) noexcept
      : sock_(std::move(other.sock_)), socket_pool_(other.socket_pool_) {
    other.socket_pool_ = nullptr;
  }

  BorrowedSocket(base::UnixSocketRaw sock, SocketPool* socket_pool)
      : sock_(std::move(sock)), socket_pool_(socket_pool) {}

  ~BorrowedSocket() {
    if (socket_pool_ != nullptr)
      socket_pool_->Return(std::move(sock_));
  }

  base::UnixSocketRaw* operator->() { return &sock_; }
  base::UnixSocketRaw* get() { return &sock_; }
  void Shutdown() { sock_.Shutdown(); }
  explicit operator bool() const { return !!sock_; }

 private:
  base::UnixSocketRaw sock_;
  SocketPool* socket_pool_ = nullptr;
};

// Cache for frees that have been observed. It is infeasible to send every
// free separately, so we batch and send the whole buffer once it is full.
class FreePage {
 public:
  FreePage(uint64_t client_generation) {
    free_page_.client_generation = client_generation;
  }

  // Add address to buffer. Flush if necessary using a socket borrowed from
  // pool.
  // Can be called from any thread. Must not hold mutex_.
  bool Add(const uint64_t addr, uint64_t sequence_number, SocketPool* pool);

 private:
  // Needs to be called holding mutex_.
  bool FlushLocked(SocketPool* pool);

  FreeMetadata free_page_;
  std::timed_mutex mutex_;
  size_t offset_ = 0;
};

const char* GetThreadStackBase();

constexpr uint32_t kClientSockTimeoutMs = 1000;

// Profiling client, used to sample and record the malloc/free family of calls,
// and communicate the necessary state to a separate profiling daemon process.
//
// Created and owned by the malloc hooks.
//
// Methods of this class are thread-safe unless otherwise stated, in which case
// the caller needs to synchronize calls behind a mutex or similar.
class Client {
 public:
  Client(std::vector<base::UnixSocketRaw> sockets);
  Client(const std::string& sock_name, size_t conns);
  bool RecordMalloc(uint64_t alloc_size,
                    uint64_t total_size,
                    uint64_t alloc_address);
  bool RecordFree(uint64_t alloc_address);
  void Shutdown();

  // Returns the number of bytes to assign to an allocation with the given
  // |alloc_size|, based on the current sampling rate. A return value of zero
  // means that the allocation should not be recorded. Not idempotent, each
  // invocation mutates the sampler state.
  //
  // Not thread-safe.
  size_t GetSampleSizeLocked(size_t alloc_size) {
    if (!inited_.load(std::memory_order_acquire))
      return 0;
    return sampler_.SampleSize(alloc_size);
  }

  ClientConfiguration client_config_for_testing() { return client_config_; }
  bool inited() { return inited_; }

 private:
  const char* GetStackBase();

  static std::atomic<uint64_t> max_generation_;
  const uint64_t generation_;

  // TODO(rsavitski): used to check if the client is completely initialized
  // after construction. The reads in RecordFree & GetSampleSizeLocked are no
  // longer necessary (was an optimization to not do redundant work after
  // shutdown). Turn into a normal bool, or indicate construction failures
  // differently.
  std::atomic<bool> inited_{false};
  ClientConfiguration client_config_;
  // sampler_ operations are not thread-safe.
  Sampler sampler_;
  SocketPool socket_pool_;
  FreePage free_page_;
  const char* main_thread_stack_base_ = nullptr;
  std::atomic<uint64_t> sequence_number_{0};
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_CLIENT_H_
