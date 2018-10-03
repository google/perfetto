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

#include <pthread.h>
#include <stddef.h>

#include <mutex>
#include <vector>

#include "perfetto/base/scoped_file.h"
#include "src/profiling/memory/wire_protocol.h"

namespace perfetto {

class BorrowedSocket;

class SocketPool {
 public:
  friend class BorrowedSocket;
  SocketPool(std::vector<base::ScopedFile> sockets);

  BorrowedSocket Borrow();

 private:
  void Return(base::ScopedFile fd);
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<base::ScopedFile> sockets_;
  size_t available_sockets_;
  size_t dead_sockets_ = 0;
};

// Socket borrowed from a SocketPool. Gets returned once it goes out of scope.
class BorrowedSocket {
 public:
  BorrowedSocket(const BorrowedSocket&) = delete;
  BorrowedSocket& operator=(const BorrowedSocket&) = delete;
  BorrowedSocket(BorrowedSocket&& other) noexcept {
    fd_ = std::move(other.fd_);
    socket_pool_ = other.socket_pool_;
    other.socket_pool_ = nullptr;
  }

  BorrowedSocket(base::ScopedFile fd, SocketPool* socket_pool)
      : fd_(std::move(fd)), socket_pool_(socket_pool) {}

  ~BorrowedSocket() {
    if (socket_pool_ != nullptr)
      socket_pool_->Return(std::move(fd_));
  }

  int operator*() { return get(); }

  int get() { return *fd_; }

  void Close() { fd_.reset(); }

 private:
  base::ScopedFile fd_;
  SocketPool* socket_pool_ = nullptr;
};

// Cache for frees that have been observed. It is infeasible to send every
// free separately, so we batch and send the whole buffer once it is full.
class FreePage {
 public:
  // Add address to buffer. Flush if necessary using a socket borrowed from
  // pool.
  // Can be called from any thread. Must not hold mutex_.`
  void Add(const uint64_t addr, uint64_t sequence_number, SocketPool* pool);

 private:
  // Needs to be called holding mutex_.
  void FlushLocked(SocketPool* pool);

  FreeMetadata free_page_;
  std::mutex mutex_;
  size_t offset_ = 0;
};

const char* GetThreadStackBase();

// RAII wrapper around pthread_key_t. This is different from a ScopedResource
// because it needs a separate boolean indicating validity.
class PThreadKey {
 public:
  PThreadKey(const PThreadKey&) = delete;
  PThreadKey& operator=(const PThreadKey&) = delete;

  PThreadKey(void (*destructor)(void*)) noexcept
      : valid_(pthread_key_create(&key_, destructor) == 0) {}
  ~PThreadKey() noexcept {
    if (valid_)
      pthread_key_delete(key_);
  }
  bool valid() const { return valid_; }
  pthread_key_t get() const {
    PERFETTO_DCHECK(valid_);
    return key_;
  }

 private:
  pthread_key_t key_;
  bool valid_;
};

// This is created and owned by the malloc hooks.
class Client {
 public:
  Client(std::vector<base::ScopedFile> sockets);
  Client(const std::string& sock_name, size_t conns);
  void RecordMalloc(uint64_t alloc_size, uint64_t alloc_address);
  void RecordFree(uint64_t alloc_address);
  bool ShouldSampleAlloc(uint64_t alloc_size,
                         void* (*unhooked_malloc)(size_t),
                         void (*unhooked_free)(void*));

  ClientConfiguration client_config_for_testing() { return client_config_; }

 private:
  const char* GetStackBase();

  ClientConfiguration client_config_;
  PThreadKey pthread_key_;
  SocketPool socket_pool_;
  FreePage free_page_;
  const char* main_thread_stack_base_ = nullptr;
  std::atomic<uint64_t> sequence_number_{0};
};

}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_CLIENT_H_
