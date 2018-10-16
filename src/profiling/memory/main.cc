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

#include <signal.h>

#include "perfetto/base/event.h"
#include "perfetto/base/unix_socket.h"
#include "src/profiling/memory/bounded_queue.h"
#include "src/profiling/memory/socket_listener.h"

#include "perfetto/base/unix_task_runner.h"

namespace perfetto {
namespace {

constexpr size_t kUnwinderQueueSize = 1000;
constexpr size_t kBookkeepingQueueSize = 1000;
constexpr size_t kUnwinderThreads = 5;
constexpr double kDefaultSamplingRate = 1;

base::Event* g_dump_evt = nullptr;

void DumpSignalHandler(int) {
  g_dump_evt->Notify();
}

// We create kUnwinderThreads unwinding threads and one bookeeping thread.
// The bookkeeping thread is singleton in order to avoid expensive and
// complicated synchronisation in the bookkeeping.
//
// We wire up the system by creating BoundedQueues between the threads. The main
// thread runs the TaskRunner driving the SocketListener. The unwinding thread
// takes the data received by the SocketListener and if it is a malloc does
// stack unwinding, and if it is a free just forwards the content of the record
// to the bookkeeping thread.
//
//             +--------------+
//             |SocketListener|
//             +------+-------+
//                    |
//          +--UnwindingRecord -+
//          |                   |
// +--------v-------+   +-------v--------+
// |Unwinding Thread|   |Unwinding Thread|
// +--------+-------+   +-------+--------+
//          |                   |
//          +-BookkeepingRecord +
//                    |
//           +--------v---------+
//           |Bookkeeping Thread|
//           +------------------+
int HeapprofdMain(int argc, char** argv) {
  // TODO(fmayer): This is temporary until heapprofd is integrated with Perfetto
  // and receives its configuration via that.
  double sampling_rate = kDefaultSamplingRate;
  int opt;
  while ((opt = getopt(argc, argv, "r:")) != -1) {
    switch (opt) {
      case 'r': {
        char* end;
        sampling_rate = strtol(optarg, &end, 10);
        if (*end != '\0' || *optarg == '\0')
          PERFETTO_FATAL("Invalid sampling rate: %s", optarg);
        break;
      }
    }
  }

  base::UnixTaskRunner task_runner;
  BoundedQueue<BookkeepingRecord> bookkeeping_queue(kBookkeepingQueueSize);
  // We set this up before launching any threads, so we do not have to use a
  // std::atomic for g_dump_evt.
  g_dump_evt = new base::Event();

  struct sigaction action = {};
  action.sa_handler = DumpSignalHandler;
  PERFETTO_CHECK(sigaction(SIGUSR1, &action, nullptr) == 0);
  task_runner.AddFileDescriptorWatch(g_dump_evt->fd(), [&bookkeeping_queue] {
    g_dump_evt->Clear();

    BookkeepingRecord rec = {};
    rec.record_type = BookkeepingRecord::Type::Dump;
    bookkeeping_queue.Add(std::move(rec));
  });

  std::unique_ptr<base::UnixSocket> sock;

  BookkeepingThread bookkeeping_thread("/data/local/tmp/heap_dump");
  std::thread bookkeeping_th([&bookkeeping_thread, &bookkeeping_queue] {
    bookkeeping_thread.Run(&bookkeeping_queue);
  });

  std::array<BoundedQueue<UnwindingRecord>, kUnwinderThreads> unwinder_queues;
  for (size_t i = 0; i < kUnwinderThreads; ++i)
    unwinder_queues[i].SetCapacity(kUnwinderQueueSize);
  std::vector<std::thread> unwinding_threads;
  unwinding_threads.reserve(kUnwinderThreads);
  for (size_t i = 0; i < kUnwinderThreads; ++i) {
    unwinding_threads.emplace_back([&unwinder_queues, &bookkeeping_queue, i] {
      UnwindingMainLoop(&unwinder_queues[i], &bookkeeping_queue);
    });
  }

  auto on_record_received = [&unwinder_queues](UnwindingRecord r) {
    unwinder_queues[static_cast<size_t>(r.pid) % kUnwinderThreads].Add(
        std::move(r));
  };
  SocketListener listener({sampling_rate}, std::move(on_record_received),
                          &bookkeeping_thread);

  if (optind == argc - 1) {
    // Allow to be able to manually specify the socket to listen on
    // for testing and sideloading purposes.
    sock = base::UnixSocket::Listen(argv[argc - 1], &listener, &task_runner);
  } else if (optind == argc) {
    // When running as a service launched by init on Android, the socket
    // is created by init and passed to the application using an environment
    // variable.
    const char* sock_fd = getenv("ANDROID_SOCKET_heapprofd");
    if (sock_fd == nullptr)
      PERFETTO_FATAL(
          "No argument given and environment variable ANDROID_SOCKET_heapprof "
          "is unset.");
    char* end;
    int raw_fd = static_cast<int>(strtol(sock_fd, &end, 10));
    if (*end != '\0')
      PERFETTO_FATAL(
          "Invalid ANDROID_SOCKET_heapprofd. Expected decimal integer.");
    sock = base::UnixSocket::Listen(base::ScopedFile(raw_fd), &listener,
                                    &task_runner);
  } else {
    PERFETTO_FATAL("Invalid number of arguments. %s [-r rate] [SOCKET]",
                   argv[0]);
  }

  if (sock->last_error() != 0)
    PERFETTO_FATAL("Failed to initialize socket: %s",
                   strerror(sock->last_error()));

  task_runner.Run();
  return 0;
}
}  // namespace
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::HeapprofdMain(argc, argv);
}
