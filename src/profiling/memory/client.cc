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
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <new>

#include <unwindstack/MachineArm.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/MachineMips.h>
#include <unwindstack/MachineMips64.h>
#include <unwindstack/MachineX86.h>
#include <unwindstack/MachineX86_64.h>
#include <unwindstack/Regs.h>
#include <unwindstack/RegsGetLocal.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/unix_socket.h"
#include "perfetto/base/utils.h"
#include "src/profiling/memory/wire_protocol.h"

namespace perfetto {
namespace {

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
// glibc does not define a wrapper around gettid, bionic does.
pid_t gettid() {
  return static_cast<pid_t>(syscall(__NR_gettid));
}
#endif

std::vector<base::ScopedFile> ConnectPool(const std::string& sock_name,
                                          size_t n) {
  sockaddr_un addr;
  socklen_t addr_size;
  if (!base::MakeSockAddr(sock_name, &addr, &addr_size))
    return {};

  std::vector<base::ScopedFile> res;
  res.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    auto sock = base::CreateSocket();
    if (connect(*sock, reinterpret_cast<sockaddr*>(&addr), addr_size) == -1) {
      PERFETTO_PLOG("Failed to connect to %s", sock_name.c_str());
      continue;
    }
    res.emplace_back(std::move(sock));
  }
  return res;
}

inline bool IsMainThread() {
  return getpid() == gettid();
}

}  // namespace

void FreePage::Add(const uint64_t addr,
                   const uint64_t sequence_number,
                   SocketPool* pool) {
  std::lock_guard<std::mutex> l(mutex_);
  if (offset_ == kFreePageSize) {
    FlushLocked(pool);
    // Now that we have flushed, reset to after the header.
    offset_ = 0;
  }
  FreePageEntry& current_entry = free_page_.entries[offset_++];
  current_entry.sequence_number = sequence_number;
  current_entry.addr = addr;
}

void FreePage::FlushLocked(SocketPool* pool) {
  WireMessage msg = {};
  msg.record_type = RecordType::Free;
  msg.free_header = &free_page_;
  BorrowedSocket fd(pool->Borrow());
  SendWireMessage(*fd, msg);
}

SocketPool::SocketPool(std::vector<base::ScopedFile> sockets)
    : sockets_(std::move(sockets)), available_sockets_(sockets_.size()) {}

BorrowedSocket SocketPool::Borrow() {
  std::unique_lock<std::mutex> lck_(mutex_);
  if (available_sockets_ == 0)
    cv_.wait(lck_, [this] { return available_sockets_ > 0; });
  PERFETTO_CHECK(available_sockets_ > 0);
  return {std::move(sockets_[--available_sockets_]), this};
}

void SocketPool::Return(base::ScopedFile sock) {
  PERFETTO_CHECK(dead_sockets_ + available_sockets_ < sockets_.size());
  if (!sock) {
    // TODO(fmayer): Handle reconnect or similar.
    // This is just to prevent a deadlock.
    PERFETTO_CHECK(++dead_sockets_ != sockets_.size());
    return;
  }
  std::unique_lock<std::mutex> lck_(mutex_);
  PERFETTO_CHECK(available_sockets_ < sockets_.size());
  sockets_[available_sockets_++] = std::move(sock);
  if (available_sockets_ == 1) {
    lck_.unlock();
    cv_.notify_one();
  }
}

const char* GetThreadStackBase() {
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0)
    return nullptr;
  base::ScopedResource<pthread_attr_t*, pthread_attr_destroy, nullptr> cleanup(
      &attr);

  char* stackaddr;
  size_t stacksize;
  if (pthread_attr_getstack(&attr, reinterpret_cast<void**>(&stackaddr),
                            &stacksize) != 0)
    return nullptr;
  return stackaddr + stacksize;
}

Client::Client(std::vector<base::ScopedFile> socks)
    : socket_pool_(std::move(socks)) {
  uint64_t size = 0;
  int fds[2];
  fds[0] = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  fds[1] = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
  base::SockSend(*socket_pool_.Borrow(), &size, sizeof(size), fds, 2);
}

Client::Client(const std::string& sock_name, size_t conns)
    : Client(ConnectPool(sock_name, conns)) {}

const char* Client::GetStackBase() {
  if (IsMainThread()) {
    if (!main_thread_stack_base_)
      // Because pthread_attr_getstack reads and parses /proc/self/maps and
      // /proc/self/stat, we have to cache the result here.
      main_thread_stack_base_ = GetThreadStackBase();
    return main_thread_stack_base_;
  }
  return GetThreadStackBase();
}

// The stack grows towards numerically smaller addresses, so the stack layout
// of main calling malloc is as follows.
//
//               +------------+
//               |SendWireMsg |
// stacktop +--> +------------+ 0x1000
//               |RecordMalloc|    +
//               +------------+    |
//               | malloc     |    |
//               +------------+    |
//               |  main      |    v
// stackbase +-> +------------+ 0xffff
void Client::RecordMalloc(uint64_t alloc_size, uint64_t alloc_address) {
  AllocMetadata metadata;
  const char* stackbase = GetStackBase();
  const char* stacktop = reinterpret_cast<char*>(__builtin_frame_address(0));
  unwindstack::AsmGetRegs(metadata.register_data);

  if (stackbase < stacktop) {
    PERFETTO_DCHECK(false);
    return;
  }

  uint64_t stack_size = static_cast<uint64_t>(stackbase - stacktop);
  metadata.alloc_size = alloc_size;
  metadata.alloc_address = alloc_address;
  metadata.stack_pointer = reinterpret_cast<uint64_t>(stacktop);
  metadata.stack_pointer_offset = sizeof(AllocMetadata);
  metadata.arch = unwindstack::Regs::CurrentArch();
  metadata.sequence_number = ++sequence_number_;

  WireMessage msg{};
  msg.alloc_header = &metadata;
  msg.payload = const_cast<char*>(stacktop);
  msg.payload_size = static_cast<size_t>(stack_size);

  BorrowedSocket sockfd = socket_pool_.Borrow();
  // TODO(fmayer): Handle failure.
  PERFETTO_CHECK(SendWireMessage(*sockfd, msg));
}

void Client::RecordFree(uint64_t alloc_address) {
  free_page_.Add(alloc_address, ++sequence_number_, &socket_pool_);
}

}  // namespace perfetto
