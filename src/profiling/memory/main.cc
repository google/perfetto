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

int HeapprofdMain(int, char**) {
  base::UnixTaskRunner task_runner;
  HeapprofdProducer producer(&task_runner);
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
