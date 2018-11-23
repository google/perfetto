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
#include "src/profiling/memory/heapprofd_producer.h"
#include "src/profiling/memory/socket_listener.h"
#include "src/profiling/memory/wire_protocol.h"
#include "src/tracing/ipc/default_socket.h"

#include "perfetto/base/unix_task_runner.h"

namespace perfetto {
namespace profiling {
namespace {

base::Event* g_dump_evt = nullptr;

int HeapprofdMain(int, char**) {
  // We set this up before launching any threads, so we do not have to use a
  // std::atomic for g_dump_evt.
  g_dump_evt = new base::Event();

  base::UnixTaskRunner task_runner;
  HeapprofdProducer producer(&task_runner);

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
