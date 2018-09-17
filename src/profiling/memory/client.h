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

#include <mutex>
#include <vector>

#include "perfetto/base/scoped_file.h"

namespace perfetto {

class SocketPool;

class FreePage {
 public:
  FreePage();

  // Can be called from any thread. Must not hold mtx_.`
  void Add(const uint64_t addr, SocketPool* pool);

 private:
  // Needs to be called holding mtx_.
  void Flush(SocketPool* pool);

  std::vector<uint64_t> free_page_;
  std::mutex mtx_;
  size_t offset_;
};

class BorrowedSocket {
 public:
  BorrowedSocket(const BorrowedSocket&) = delete;
  BorrowedSocket& operator=(const BorrowedSocket&) = delete;
  BorrowedSocket(BorrowedSocket&& other) {
    fd_ = std::move(other.fd_);
    socket_pool_ = other.socket_pool_;
    other.socket_pool_ = nullptr;
  }

  BorrowedSocket(base::ScopedFile fd, SocketPool* socket_pool);
  int operator*();
  int get();
  void Close();
  ~BorrowedSocket();

 private:
  base::ScopedFile fd_;
  SocketPool* socket_pool_ = nullptr;
};

class SocketPool {
 public:
  friend class BorrowedSocket;
  SocketPool(std::vector<base::ScopedFile> sockets);

  BorrowedSocket Borrow();

 private:
  void Return(base::ScopedFile fd);
  std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<base::ScopedFile> sockets_;
  size_t available_sockets_;
  size_t dead_sockets_ = 0;
};

}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_CLIENT_H_
