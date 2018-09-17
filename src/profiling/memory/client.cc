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

#include "src/profiling/memory/client.h"

#include <inttypes.h>
#include <sys/socket.h>

#include <atomic>

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"
#include "src/profiling/memory/transport_data.h"

namespace perfetto {
namespace {

std::atomic<uint64_t> global_sequence_number(0);
constexpr size_t kFreePageBytes = base::kPageSize;
constexpr size_t kFreePageSize = kFreePageBytes / sizeof(uint64_t);

}  // namespace

FreePage::FreePage() : free_page_(kFreePageSize) {
  free_page_[0] = static_cast<uint64_t>(kFreePageBytes);
  free_page_[1] = static_cast<uint64_t>(RecordType::Free);
  offset_ = 2;
  // Code in Add assumes that offset is aligned to 2.
  PERFETTO_DCHECK(offset_ % 2 == 0);
}

void FreePage::Add(const uint64_t addr, SocketPool* pool) {
  std::lock_guard<std::mutex> l(mtx_);
  if (offset_ == kFreePageSize)
    Flush(pool);
  static_assert(kFreePageSize % 2 == 0,
                "free page size needs to be divisible by two");
  free_page_[offset_++] = ++global_sequence_number;
  free_page_[offset_++] = addr;
  PERFETTO_DCHECK(offset_ % 2 == 0);
}

void FreePage::Flush(SocketPool* pool) {
  BorrowedSocket fd(pool->Borrow());
  size_t written = 0;
  do {
    ssize_t wr = PERFETTO_EINTR(send(*fd, &free_page_[0] + written,
                                     kFreePageBytes - written, MSG_NOSIGNAL));
    if (wr == -1) {
      fd.Close();
      return;
    }
    written += static_cast<size_t>(wr);
  } while (written < kFreePageBytes);
  // Now that we have flushed, reset to after the header.
  offset_ = 2;
}

BorrowedSocket::BorrowedSocket(base::ScopedFile fd, SocketPool* socket_pool)
    : fd_(std::move(fd)), socket_pool_(socket_pool) {}

int BorrowedSocket::operator*() {
  return get();
}

int BorrowedSocket::get() {
  return *fd_;
}

void BorrowedSocket::Close() {
  fd_.reset();
}

BorrowedSocket::~BorrowedSocket() {
  if (socket_pool_ != nullptr)
    socket_pool_->Return(std::move(fd_));
}

SocketPool::SocketPool(std::vector<base::ScopedFile> sockets)
    : sockets_(std::move(sockets)), available_sockets_(sockets_.size()) {}

BorrowedSocket SocketPool::Borrow() {
  std::unique_lock<std::mutex> lck_(mtx_);
  if (available_sockets_ == 0)
    cv_.wait(lck_, [this] { return available_sockets_ > 0; });
  PERFETTO_CHECK(available_sockets_ > 0);
  return {std::move(sockets_[--available_sockets_]), this};
}

void SocketPool::Return(base::ScopedFile sock) {
  if (!sock) {
    // TODO(fmayer): Handle reconnect or similar.
    // This is just to prevent a deadlock.
    PERFETTO_CHECK(++dead_sockets_ != sockets_.size());
    return;
  }
  std::unique_lock<std::mutex> lck_(mtx_);
  PERFETTO_CHECK(available_sockets_ < sockets_.size());
  sockets_[available_sockets_++] = std::move(sock);
  if (available_sockets_ == 1) {
    lck_.unlock();
    cv_.notify_one();
  }
}

}  // namespace perfetto
