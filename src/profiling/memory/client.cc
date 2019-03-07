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
#include <sys/prctl.h>
#include <sys/syscall.h>
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
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/thread_utils.h"
#include "perfetto/base/unix_socket.h"
#include "perfetto/base/utils.h"
#include "src/profiling/memory/sampler.h"
#include "src/profiling/memory/scoped_spinlock.h"
#include "src/profiling/memory/wire_protocol.h"

namespace perfetto {
namespace profiling {
namespace {

const char kSingleByte[1] = {'x'};
constexpr std::chrono::seconds kLockTimeout{1};

base::Optional<base::UnixSocketRaw> Connect(const std::string& sock_name) {
  auto sock = base::UnixSocketRaw::CreateMayFail(base::SockType::kStream);
  if (!sock || !sock.Connect(sock_name)) {
    PERFETTO_PLOG("Failed to connect to %s", sock_name.c_str());
    return base::nullopt;
  }
  if (!sock.SetTxTimeout(kClientSockTimeoutMs)) {
    PERFETTO_PLOG("Failed to set send timeout for %s", sock_name.c_str());
    return base::nullopt;
  }
  if (!sock.SetRxTimeout(kClientSockTimeoutMs)) {
    PERFETTO_PLOG("Failed to set receive timeout for %s", sock_name.c_str());
    return base::nullopt;
  }
  return std::move(sock);
}

inline bool IsMainThread() {
  return getpid() == base::GetThreadId();
}

// TODO(b/117203899): Remove this after making bionic implementation safe to
// use.
char* FindMainThreadStack() {
  base::ScopedFstream maps(fopen("/proc/self/maps", "r"));
  if (!maps) {
    return nullptr;
  }
  while (!feof(*maps)) {
    char line[1024];
    char* data = fgets(line, sizeof(line), *maps);
    if (data != nullptr && strstr(data, "[stack]")) {
      char* sep = strstr(data, "-");
      if (sep == nullptr)
        continue;
      sep++;
      return reinterpret_cast<char*>(strtoll(sep, nullptr, 16));
    }
  }
  return nullptr;
}

int UnsetDumpable(int) {
  prctl(PR_SET_DUMPABLE, 0);
  return 0;
}

}  // namespace

bool FreePage::Add(const uint64_t addr,
                   const uint64_t sequence_number,
                   Client* client) {
  std::unique_lock<std::timed_mutex> l(mutex_, kLockTimeout);
  if (!l.owns_lock())
    return false;
  if (free_page_.num_entries == kFreePageSize) {
    if (!client->FlushFrees(&free_page_))
      return false;
    // Now that we have flushed, reset to after the header.
    free_page_.num_entries = 0;
  }
  FreePageEntry& current_entry = free_page_.entries[free_page_.num_entries++];
  current_entry.sequence_number = sequence_number;
  current_entry.addr = addr;
  return true;
}

bool Client::FlushFrees(FreeMetadata* free_metadata) {
  WireMessage msg = {};
  msg.record_type = RecordType::Free;
  msg.free_header = free_metadata;
  if (!SendWireMessage(&shmem_, msg)) {
    PERFETTO_PLOG("Failed to send wire message");
    Shutdown();
    return false;
  }
  if (!sock_.Send(kSingleByte, sizeof(kSingleByte))) {
    Shutdown();
    return false;
  }
  return true;
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

Client::Client(base::Optional<base::UnixSocketRaw> sock)
    : sampler_(8192),  // placeholder until we receive the config (within ctor)
      main_thread_stack_base_(FindMainThreadStack()) {
  if (!sock || !sock.value()) {
    PERFETTO_DFATAL("Socket not connected.");
    return;
  }
  sock_ = std::move(sock.value());

  // We might be running in a process that is not dumpable (such as app
  // processes on user builds), in which case the /proc/self/mem will be chown'd
  // to root:root, and will not be accessible even to the process itself (see
  // man 5 proc). In such situations, temporarily mark the process dumpable to
  // be able to open the files, unsetting dumpability immediately afterwards.
  int orig_dumpable = prctl(PR_GET_DUMPABLE);

  enum { kNop, kDoUnset };
  base::ScopedResource<int, UnsetDumpable, kNop, false> unset_dumpable(kNop);
  if (orig_dumpable == 0) {
    unset_dumpable.reset(kDoUnset);
    prctl(PR_SET_DUMPABLE, 1);
  }

  base::ScopedFile maps(base::OpenFile("/proc/self/maps", O_RDONLY));
  if (!maps) {
    PERFETTO_DFATAL("Failed to open /proc/self/maps");
    return;
  }
  base::ScopedFile mem(base::OpenFile("/proc/self/mem", O_RDONLY));
  if (!mem) {
    PERFETTO_DFATAL("Failed to open /proc/self/mem");
    return;
  }
  // Restore original dumpability value if we overrode it.
  unset_dumpable.reset();

  int fds[kHandshakeSize];
  fds[kHandshakeMaps] = *maps;
  fds[kHandshakeMem] = *mem;

  // Send an empty record to transfer fds for /proc/self/maps and
  // /proc/self/mem.
  if (sock_.Send(kSingleByte, sizeof(kSingleByte), fds, kHandshakeSize) !=
      sizeof(kSingleByte)) {
    PERFETTO_DFATAL("Failed to send file descriptors.");
    return;
  }

  base::ScopedFile shmem_fd;
  if (sock_.Receive(&client_config_, sizeof(client_config_), &shmem_fd, 1) !=
      sizeof(client_config_)) {
    PERFETTO_DFATAL("Failed to receive client config.");
    return;
  }

  auto shmem = SharedRingBuffer::Attach(std::move(shmem_fd));
  if (!shmem || !shmem->is_valid()) {
    PERFETTO_DFATAL("Failed to attach to shmem.");
    return;
  }
  shmem_ = std::move(shmem.value());

  PERFETTO_DCHECK(client_config_.interval >= 1);
  sampler_ = Sampler(client_config_.interval);

  PERFETTO_DLOG("Initialized client.");
  inited_.store(true, std::memory_order_release);
}

Client::Client(const std::string& sock_name) : Client(Connect(sock_name)) {}

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
bool Client::RecordMalloc(uint64_t alloc_size,
                          uint64_t total_size,
                          uint64_t alloc_address) {
  if (!inited_.load(std::memory_order_acquire)) {
    return false;
  }
  AllocMetadata metadata;
  const char* stackbase = GetStackBase();
  const char* stacktop = reinterpret_cast<char*>(__builtin_frame_address(0));
  unwindstack::AsmGetRegs(metadata.register_data);

  if (stackbase < stacktop) {
    PERFETTO_DFATAL("Stackbase >= stacktop.");
    Shutdown();
    return false;
  }

  uint64_t stack_size = static_cast<uint64_t>(stackbase - stacktop);
  metadata.total_size = total_size;
  metadata.alloc_size = alloc_size;
  metadata.alloc_address = alloc_address;
  metadata.stack_pointer = reinterpret_cast<uint64_t>(stacktop);
  metadata.stack_pointer_offset = sizeof(AllocMetadata);
  metadata.arch = unwindstack::Regs::CurrentArch();
  metadata.sequence_number =
      1 + sequence_number_.fetch_add(1, std::memory_order_acq_rel);

  WireMessage msg{};
  msg.record_type = RecordType::Malloc;
  msg.alloc_header = &metadata;
  msg.payload = const_cast<char*>(stacktop);
  msg.payload_size = static_cast<size_t>(stack_size);

  if (!SendWireMessage(&shmem_, msg)) {
    PERFETTO_PLOG("Failed to send wire message.");
    Shutdown();
    return false;
  }
  if (sock_.Send(kSingleByte, sizeof(kSingleByte)) == -1) {
    PERFETTO_PLOG("Failed to send wire message.");
    Shutdown();
    return false;
  }
  return true;
}

bool Client::RecordFree(uint64_t alloc_address) {
  if (!inited_.load(std::memory_order_acquire))
    return false;
  bool success = free_page_.Add(
      alloc_address,
      1 + sequence_number_.fetch_add(1, std::memory_order_acq_rel), this);
  if (!success)
    Shutdown();
  return success;
}

void Client::Shutdown() {
  inited_.store(false, std::memory_order_release);
}

}  // namespace profiling
}  // namespace perfetto
