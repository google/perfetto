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

#include <inttypes.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <tuple>

#include <sys/system_properties.h>

#include <private/bionic_malloc.h>
#include <private/bionic_malloc_dispatch.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/unix_socket.h"
#include "perfetto/base/utils.h"
#include "src/profiling/memory/client.h"
#include "src/profiling/memory/proc_utils.h"
#include "src/profiling/memory/wire_protocol.h"

// The real malloc function pointers we get in initialize.
static std::atomic<const MallocDispatch*> g_dispatch{nullptr};
static std::atomic<perfetto::profiling::Client*> g_client{nullptr};
static constexpr size_t kNumConnections = 2;

static constexpr char kHeapprofdBinPath[] = "/system/bin/heapprofd";

// The only writes are in the initialization function. Because Bionic does a
// release write after initialization and an acquire read to retrieve the hooked
// malloc functions, we can use relaxed memory mode for both writing, and more
// importantly because in the fast-path, reading.
static constexpr std::memory_order write_order = std::memory_order_relaxed;

static perfetto::profiling::Client* GetClient() {
  return g_client.load(std::memory_order_relaxed);
}

static const MallocDispatch* GetDispatch() {
  return g_dispatch.load(std::memory_order_relaxed);
}

static void MallocDispatchReset(const MallocDispatch* dispatch) {
  android_mallopt(M_RESET_HOOKS, nullptr, 0);
}

// This is so we can make an so that we can swap out with the existing
// libc_malloc_hooks.so
#ifndef HEAPPROFD_PREFIX
#define HEAPPROFD_PREFIX heapprofd
#endif

#define HEAPPROFD_ADD_PREFIX(name) \
  PERFETTO_BUILDFLAG_CAT(HEAPPROFD_PREFIX, name)

#pragma GCC visibility push(default)
extern "C" {

bool HEAPPROFD_ADD_PREFIX(_initialize)(const MallocDispatch* malloc_dispatch,
                                       int* malloc_zygote_child,
                                       const char* options);
void HEAPPROFD_ADD_PREFIX(_finalize)();
void HEAPPROFD_ADD_PREFIX(_dump_heap)(const char* file_name);
void HEAPPROFD_ADD_PREFIX(_get_malloc_leak_info)(uint8_t** info,
                                                 size_t* overall_size,
                                                 size_t* info_size,
                                                 size_t* total_memory,
                                                 size_t* backtrace_size);
bool HEAPPROFD_ADD_PREFIX(_write_malloc_leak_info)(FILE* fp);
ssize_t HEAPPROFD_ADD_PREFIX(_malloc_backtrace)(void* pointer,
                                                uintptr_t* frames,
                                                size_t frame_count);
void HEAPPROFD_ADD_PREFIX(_free_malloc_leak_info)(uint8_t* info);
size_t HEAPPROFD_ADD_PREFIX(_malloc_usable_size)(void* pointer);
void* HEAPPROFD_ADD_PREFIX(_malloc)(size_t size);
void HEAPPROFD_ADD_PREFIX(_free)(void* pointer);
void* HEAPPROFD_ADD_PREFIX(_aligned_alloc)(size_t alignment, size_t size);
void* HEAPPROFD_ADD_PREFIX(_memalign)(size_t alignment, size_t bytes);
void* HEAPPROFD_ADD_PREFIX(_realloc)(void* pointer, size_t bytes);
void* HEAPPROFD_ADD_PREFIX(_calloc)(size_t nmemb, size_t bytes);
struct mallinfo HEAPPROFD_ADD_PREFIX(_mallinfo)();
int HEAPPROFD_ADD_PREFIX(_mallopt)(int param, int value);
int HEAPPROFD_ADD_PREFIX(_posix_memalign)(void** memptr,
                                          size_t alignment,
                                          size_t size);
int HEAPPROFD_ADD_PREFIX(_iterate)(uintptr_t base,
                                   size_t size,
                                   void (*callback)(uintptr_t base,
                                                    size_t size,
                                                    void* arg),
                                   void* arg);
void HEAPPROFD_ADD_PREFIX(_malloc_disable)();
void HEAPPROFD_ADD_PREFIX(_malloc_enable)();

#if defined(HAVE_DEPRECATED_MALLOC_FUNCS)
void* HEAPPROFD_ADD_PREFIX(_pvalloc)(size_t bytes);
void* HEAPPROFD_ADD_PREFIX(_valloc)(size_t size);
#endif
}
#pragma GCC visibility pop

namespace {

std::string ReadSystemProperty(const char* key) {
  std::string prop_value;
  const prop_info* prop = __system_property_find(key);
  if (!prop) {
    return prop_value;  // empty
  }
  __system_property_read_callback(
      prop,
      [](void* cookie, const char* name, const char* value, uint32_t) {
        std::string* prop_value = reinterpret_cast<std::string*>(cookie);
        *prop_value = value;
      },
      &prop_value);
  return prop_value;
}

bool ShouldForkPrivateDaemon() {
  std::string build_type = ReadSystemProperty("ro.build.type");
  if (build_type.empty()) {
    PERFETTO_ELOG(
        "Cannot determine platform build type, proceeding in fork mode "
        "profiling.");
    return true;
  }

  // On development builds, we support both modes of profiling, depending on a
  // system property.
  if (build_type == "userdebug" || build_type == "eng") {
    std::string mode = ReadSystemProperty("heapprofd.userdebug.mode");
    return mode == "fork";
  }

  // User/other builds - always fork private profiler.
  return true;
}

std::unique_ptr<perfetto::profiling::Client> CreateClientForCentralDaemon() {
  PERFETTO_DLOG("Constructing client for central daemon.");

  using perfetto::profiling::Client;
  return std::unique_ptr<Client>(new (std::nothrow) Client(
      perfetto::profiling::kHeapprofdSocketFile, kNumConnections));
}

std::unique_ptr<perfetto::profiling::Client> CreateClientAndPrivateDaemon() {
  PERFETTO_DLOG("Setting up fork mode profiling.");
  // TODO(rsavitski): create kNumConnections socketpairs to match central mode
  // behavior.
  perfetto::base::UnixSocketRaw parent_sock;
  perfetto::base::UnixSocketRaw child_sock;
  std::tie(parent_sock, child_sock) = perfetto::base::UnixSocketRaw::CreatePair(
      perfetto::base::SockType::kStream);

  if (!parent_sock || !child_sock) {
    PERFETTO_PLOG("Failed to create socketpair.");
    return nullptr;
  }

  child_sock.RetainOnExec();

  // Record own pid and cmdline, to pass down to the forked heapprofd.
  pid_t target_pid = getpid();
  std::string target_cmdline;
  if (!perfetto::profiling::GetCmdlineForPID(target_pid, &target_cmdline)) {
    PERFETTO_ELOG("Failed to read own cmdline.");
    return nullptr;
  }

  pid_t fork_pid = fork();
  if (fork_pid == -1) {
    PERFETTO_PLOG("Failed to fork.");
    return nullptr;
  }
  if (fork_pid == 0) {  // child
    // daemon() forks again, terminating the calling thread (i.e. the direct
    // child of the original process). So the rest of this codepath will be
    // executed in a (new) reparented process.
    if (daemon(/*nochdir=*/0, /*noclose=*/0) == -1) {
      PERFETTO_PLOG("Daemonization failed.");
      _exit(1);
    }
    std::string pid_arg =
        std::string("--exclusive-for-pid=") + std::to_string(target_pid);
    std::string cmd_arg =
        std::string("--exclusive-for-cmdline=") + target_cmdline;
    std::string fd_arg =
        std::string("--inherit-socket-fd=") + std::to_string(child_sock.fd());
    const char* argv[] = {kHeapprofdBinPath, pid_arg.c_str(), cmd_arg.c_str(),
                          fd_arg.c_str(), nullptr};

    execv(kHeapprofdBinPath, const_cast<char**>(argv));
    PERFETTO_PLOG("Failed to execute private heapprofd.");
    _exit(1);
  }  // else - parent continuing the client setup

  child_sock.ReleaseFd().reset();  // close child socket's fd
  if (!parent_sock.SetTxTimeout(perfetto::profiling::kClientSockTimeoutMs)) {
    PERFETTO_PLOG("Failed to set socket transmit timeout.");
    return nullptr;
  }

  if (!parent_sock.SetRxTimeout(perfetto::profiling::kClientSockTimeoutMs)) {
    PERFETTO_PLOG("Failed to set socket receive timeout.");
    return nullptr;
  }

  // Wait on the immediate child to exit (allow for ECHILD in the unlikely case
  // we're in a process that has made its children unwaitable).
  siginfo_t unused = {};
  if (PERFETTO_EINTR(waitid(P_PID, fork_pid, &unused, WEXITED)) == -1 &&
      errno != ECHILD) {
    PERFETTO_PLOG("Failed to waitid on immediate child.");
    return nullptr;
  }

  using perfetto::profiling::Client;
  std::vector<perfetto::base::UnixSocketRaw> client_sockets;
  client_sockets.emplace_back(std::move(parent_sock));
  return std::unique_ptr<Client>(new (std::nothrow)
                                     Client(std::move(client_sockets)));
}

}  // namespace

bool HEAPPROFD_ADD_PREFIX(_initialize)(const MallocDispatch* malloc_dispatch,
                                       int*,
                                       const char*) {
  perfetto::profiling::Client* old_client = GetClient();
  if (old_client)
    old_client->Shutdown();

  // Table of pointers to backing implementation.
  g_dispatch.store(malloc_dispatch, write_order);

  std::unique_ptr<perfetto::profiling::Client> client =
      ShouldForkPrivateDaemon() ? CreateClientAndPrivateDaemon()
                                : CreateClientForCentralDaemon();

  if (!client || !client->inited()) {
    PERFETTO_LOG("Client not initialized, not installing hooks.");
    return false;
  }

  g_client.store(client.release());
  return true;
}

void HEAPPROFD_ADD_PREFIX(_finalize)() {
  // TODO(fmayer): This should not leak.
  perfetto::profiling::Client* client = GetClient();
  if (client)
    client->Shutdown();
  MallocDispatchReset(GetDispatch());
}

void HEAPPROFD_ADD_PREFIX(_dump_heap)(const char*) {}

void HEAPPROFD_ADD_PREFIX(
    _get_malloc_leak_info)(uint8_t**, size_t*, size_t*, size_t*, size_t*) {}

bool HEAPPROFD_ADD_PREFIX(_write_malloc_leak_info)(FILE*) {
  return false;
}

ssize_t HEAPPROFD_ADD_PREFIX(_malloc_backtrace)(void*, uintptr_t*, size_t) {
  return -1;
}

void HEAPPROFD_ADD_PREFIX(_free_malloc_leak_info)(uint8_t*) {}

size_t HEAPPROFD_ADD_PREFIX(_malloc_usable_size)(void* pointer) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->malloc_usable_size(pointer);
}

void* HEAPPROFD_ADD_PREFIX(_malloc)(size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  perfetto::profiling::Client* client = GetClient();
  void* addr = dispatch->malloc(size);
  if (client) {
    if (!client->MaybeSampleAlloc(size, reinterpret_cast<uint64_t>(addr),
                                  dispatch->malloc, dispatch->free))
      MallocDispatchReset(GetDispatch());
  }
  return addr;
}

void HEAPPROFD_ADD_PREFIX(_free)(void* pointer) {
  const MallocDispatch* dispatch = GetDispatch();
  perfetto::profiling::Client* client = GetClient();
  if (client)
    if (!client->RecordFree(reinterpret_cast<uint64_t>(pointer)))
      MallocDispatchReset(GetDispatch());
  return dispatch->free(pointer);
}

void* HEAPPROFD_ADD_PREFIX(_aligned_alloc)(size_t alignment, size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  perfetto::profiling::Client* client = GetClient();
  void* addr = dispatch->aligned_alloc(alignment, size);
  if (client) {
    if (!client->MaybeSampleAlloc(size, reinterpret_cast<uint64_t>(addr),
                                  dispatch->malloc, dispatch->free))
      MallocDispatchReset(GetDispatch());
  }
  return addr;
}

void* HEAPPROFD_ADD_PREFIX(_memalign)(size_t alignment, size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  perfetto::profiling::Client* client = GetClient();
  void* addr = dispatch->memalign(alignment, size);
  if (client) {
    if (!client->MaybeSampleAlloc(size, reinterpret_cast<uint64_t>(addr),
                                  dispatch->malloc, dispatch->free))
      MallocDispatchReset(GetDispatch());
  }
  return addr;
}

void* HEAPPROFD_ADD_PREFIX(_realloc)(void* pointer, size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  perfetto::profiling::Client* client = GetClient();
  if (client && pointer)
    if (!client->RecordFree(reinterpret_cast<uint64_t>(pointer)))
      MallocDispatchReset(GetDispatch());
  void* addr = dispatch->realloc(pointer, size);
  if (client && size > 0) {
    if (!client->MaybeSampleAlloc(size, reinterpret_cast<uint64_t>(addr),
                                  dispatch->malloc, dispatch->free))
      MallocDispatchReset(GetDispatch());
  }
  return addr;
}

void* HEAPPROFD_ADD_PREFIX(_calloc)(size_t nmemb, size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  perfetto::profiling::Client* client = GetClient();
  void* addr = dispatch->calloc(nmemb, size);
  if (client) {
    if (!client->MaybeSampleAlloc(size, reinterpret_cast<uint64_t>(addr),
                                  dispatch->malloc, dispatch->free))
      MallocDispatchReset(GetDispatch());
  }
  return addr;
}

struct mallinfo HEAPPROFD_ADD_PREFIX(_mallinfo)() {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->mallinfo();
}

int HEAPPROFD_ADD_PREFIX(_mallopt)(int param, int value) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->mallopt(param, value);
}

int HEAPPROFD_ADD_PREFIX(_posix_memalign)(void** memptr,
                                          size_t alignment,
                                          size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  perfetto::profiling::Client* client = GetClient();
  int res = dispatch->posix_memalign(memptr, alignment, size);
  if (res == 0 && client) {
    if (!client->MaybeSampleAlloc(size, reinterpret_cast<uint64_t>(*memptr),
                                  dispatch->malloc, dispatch->free))
      MallocDispatchReset(GetDispatch());
  }
  return res;
}

int HEAPPROFD_ADD_PREFIX(_iterate)(uintptr_t,
                                   size_t,
                                   void (*)(uintptr_t base,
                                            size_t size,
                                            void* arg),
                                   void*) {
  return 0;
}

void HEAPPROFD_ADD_PREFIX(_malloc_disable)() {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->malloc_disable();
}

void HEAPPROFD_ADD_PREFIX(_malloc_enable)() {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->malloc_enable();
}

#if defined(HAVE_DEPRECATED_MALLOC_FUNCS)
void* HEAPPROFD_ADD_PREFIX(_pvalloc)(size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->pvalloc(size);
}

void* HEAPPROFD_ADD_PREFIX(_valloc)(size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->valloc(size);
}

#endif
