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

#include <stdlib.h>
#include <unistd.h>
#include <array>
#include <memory>
#include <vector>

#include <getopt.h>
#include <signal.h>

#include "perfetto/base/event.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/unix_socket.h"
#include "src/profiling/memory/bounded_queue.h"
#include "src/profiling/memory/heapprofd_producer.h"
#include "src/profiling/memory/socket_listener.h"
#include "src/profiling/memory/wire_protocol.h"
#include "src/tracing/ipc/default_socket.h"

#include "perfetto/base/unix_task_runner.h"

namespace perfetto {
namespace profiling {
namespace {

int StartChildHeapprofd(pid_t target_pid,
                        std::vector<base::ScopedFile> inherited_sock_fds);
int StartCentralHeapprofd();

base::Event* g_dump_evt = nullptr;

int HeapprofdMain(int argc, char** argv) {
  bool cleanup_crash = false;
  pid_t target_pid = base::kInvalidPid;
  std::vector<base::ScopedFile> inherited_sock_fds;

  enum { kCleanupCrash = 256, kTargetPid, kInheritFd };
  static struct option long_options[] = {
      {"cleanup-after-crash", no_argument, nullptr, kCleanupCrash},
      {"exclusive-for-pid", required_argument, nullptr, kTargetPid},
      {"inherit-socket-fd", required_argument, nullptr, kInheritFd},
      {nullptr, 0, nullptr, 0}};
  int option_index;
  int c;
  while ((c = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
    switch (c) {
      case kCleanupCrash:
        cleanup_crash = true;
        break;
      case kTargetPid:
        target_pid = static_cast<pid_t>(atoi(optarg));
        break;
      case kInheritFd:  // repetition supported
        inherited_sock_fds.emplace_back(atoi(optarg));
        break;
      default:
        PERFETTO_ELOG("Usage: %s [--cleanup-after-crash]", argv[0]);
        return 1;
    }
  }

  if (cleanup_crash) {
    SystemProperties::ResetProperties();
    return 0;
  }

  // If |target_pid| is given, we're supposed to be operating as a private
  // heapprofd for that process. Note that we might not be a direct child due to
  // reparenting.
  bool ppid_set = target_pid != base::kInvalidPid;
  bool fds_set = inherited_sock_fds.size() > 0;
  if (ppid_set || fds_set) {
    if (!ppid_set || !fds_set) {
      PERFETTO_ELOG(
          "If starting in child mode, requires both --exclusive-for-pid and "
          "--inherit_socket_fd");
      return 1;
    }

    return StartChildHeapprofd(target_pid, std::move(inherited_sock_fds));
  }

  // Otherwise start as a central daemon.
  return StartCentralHeapprofd();
}

int StartChildHeapprofd(pid_t target_pid,
                        std::vector<base::ScopedFile> inherited_sock_fds) {
  PERFETTO_ELOG("Exiting from unimplemented child mode.");
  return 0;
}

int StartCentralHeapprofd() {
  // We set this up before launching any threads, so we do not have to use a
  // std::atomic for g_dump_evt.
  g_dump_evt = new base::Event();

  base::UnixTaskRunner task_runner;
  HeapprofdProducer producer(HeapprofdMode::kCentral, &task_runner);

  struct sigaction action = {};
  action.sa_handler = [](int) { g_dump_evt->Notify(); };
  // Allow to trigger a full dump by sending SIGUSR1 to heapprofd.
  // This will allow manually deciding when to dump on userdebug.
  PERFETTO_CHECK(sigaction(SIGUSR1, &action, nullptr) == 0);
  task_runner.AddFileDescriptorWatch(g_dump_evt->fd(), [&producer] {
    g_dump_evt->Clear();
    producer.DumpAll();
  });
  producer.ConnectWithRetries(GetProducerSocket());
  task_runner.Run();
  return 0;
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::profiling::HeapprofdMain(argc, argv);
}
