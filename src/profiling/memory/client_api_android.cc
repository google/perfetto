/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/profiling/memory/client_api_factory.h"

#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/unix_socket.h"
#include "src/profiling/common/proc_utils.h"
#include "src/profiling/memory/client.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#error "Must be built on Android."
#endif

#include <android/fdsan.h>
#include <sys/system_properties.h>

namespace perfetto {
namespace profiling {
namespace {

constexpr char kHeapprofdBinPath[] = "/system/bin/heapprofd";

int CloneWithoutSigchld() {
  auto ret = clone(nullptr, nullptr, 0, nullptr);
  if (ret == 0)
    android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_DISABLED);
  return ret;
}

int ForklikeClone() {
  auto ret = clone(nullptr, nullptr, SIGCHLD, nullptr);
  if (ret == 0)
    android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_DISABLED);
  return ret;
}

// Like daemon(), but using clone to avoid invoking pthread_atfork(3) handlers.
int Daemonize() {
  switch (ForklikeClone()) {
    case -1:
      PERFETTO_PLOG("Daemonize.clone");
      return -1;
    case 0:
      break;
    default:
      _exit(0);
  }
  if (setsid() == -1) {
    PERFETTO_PLOG("Daemonize.setsid");
    return -1;
  }
  // best effort chdir & fd close
  chdir("/");
  int fd = open("/dev/null", O_RDWR, 0);  // NOLINT(android-cloexec-open)
  if (fd != -1) {
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
      close(fd);
  }
  return 0;
}

std::string ReadSystemProperty(const char* key) {
  std::string prop_value;
  const prop_info* prop = __system_property_find(key);
  if (!prop) {
    return prop_value;  // empty
  }
  __system_property_read_callback(
      prop,
      [](void* cookie, const char*, const char* value, uint32_t) {
        std::string* pv = reinterpret_cast<std::string*>(cookie);
        *pv = value;
      },
      &prop_value);
  return prop_value;
}

bool ForceForkPrivateDaemon() {
  // Note: if renaming the property, also update system_property.cc
  std::string mode = ReadSystemProperty("heapprofd.userdebug.mode");
  return mode == "fork";
}

std::shared_ptr<perfetto::profiling::Client> CreateClientForCentralDaemon(
    UnhookedAllocator<perfetto::profiling::Client> unhooked_allocator) {
  PERFETTO_LOG("Constructing client for central daemon.");
  using perfetto::profiling::Client;

  perfetto::base::Optional<perfetto::base::UnixSocketRaw> sock =
      Client::ConnectToHeapprofd(perfetto::profiling::kHeapprofdSocketFile);
  if (!sock) {
    PERFETTO_ELOG("Failed to connect to %s. This is benign on user builds.",
                  perfetto::profiling::kHeapprofdSocketFile);
    return nullptr;
  }
  return Client::CreateAndHandshake(std::move(sock.value()),
                                    unhooked_allocator);
}

std::shared_ptr<perfetto::profiling::Client> CreateClientAndPrivateDaemon(
    UnhookedAllocator<perfetto::profiling::Client> unhooked_allocator) {
  PERFETTO_LOG("Setting up fork mode profiling.");
  perfetto::base::UnixSocketRaw parent_sock;
  perfetto::base::UnixSocketRaw child_sock;
  std::tie(parent_sock, child_sock) = perfetto::base::UnixSocketRaw::CreatePair(
      perfetto::base::SockFamily::kUnix, perfetto::base::SockType::kStream);

  if (!parent_sock || !child_sock) {
    PERFETTO_PLOG("Failed to create socketpair.");
    return nullptr;
  }

  child_sock.RetainOnExec();

  // Record own pid and cmdline, to pass down to the forked heapprofd.
  pid_t target_pid = getpid();
  std::string target_cmdline;
  if (!perfetto::profiling::GetCmdlineForPID(target_pid, &target_cmdline)) {
    target_cmdline = "failed-to-read-cmdline";
    PERFETTO_ELOG(
        "Failed to read own cmdline, proceeding as this might be a by-pid "
        "profiling request (which will still work).");
  }

  // Prepare arguments for heapprofd.
  std::string pid_arg =
      std::string("--exclusive-for-pid=") + std::to_string(target_pid);
  std::string cmd_arg =
      std::string("--exclusive-for-cmdline=") + target_cmdline;
  std::string fd_arg =
      std::string("--inherit-socket-fd=") + std::to_string(child_sock.fd());
  const char* argv[] = {kHeapprofdBinPath, pid_arg.c_str(), cmd_arg.c_str(),
                        fd_arg.c_str(), nullptr};

  // Use fork-like clone to avoid invoking the host's pthread_atfork(3)
  // handlers. Also avoid sending the current process a SIGCHILD to further
  // reduce our interference.
  pid_t clone_pid = CloneWithoutSigchld();
  if (clone_pid == -1) {
    PERFETTO_PLOG("Failed to clone.");
    return nullptr;
  }
  if (clone_pid == 0) {  // child
    // Daemonize clones again, terminating the calling thread (i.e. the direct
    // child of the original process). So the rest of this codepath will be
    // executed in a new reparented process.
    if (Daemonize() == -1) {
      PERFETTO_PLOG("Daemonization failed.");
      _exit(1);
    }
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
  // __WCLONE is defined in hex on glibc, so is unsigned per C++ semantics.
  // waitpid expects a signed integer, so clang complains without the cast.
  int unused = 0;
  if (PERFETTO_EINTR(waitpid(clone_pid, &unused, static_cast<int>(__WCLONE))) ==
          -1 &&
      errno != ECHILD) {
    PERFETTO_PLOG("Failed to waitpid on immediate child.");
    return nullptr;
  }

  return perfetto::profiling::Client::CreateAndHandshake(std::move(parent_sock),
                                                         unhooked_allocator);
}

}  // namespace

void StartHeapprofdIfStatic() {}

std::shared_ptr<Client> ConstructClient(
    UnhookedAllocator<perfetto::profiling::Client> unhooked_allocator) {
  std::shared_ptr<perfetto::profiling::Client> client;
  if (!ForceForkPrivateDaemon())
    client = CreateClientForCentralDaemon(unhooked_allocator);
  if (!client)
    client = CreateClientAndPrivateDaemon(unhooked_allocator);
  return client;
}

}  // namespace profiling
}  // namespace perfetto
