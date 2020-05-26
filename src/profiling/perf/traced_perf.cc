/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/profiling/perf/traced_perf.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/ext/tracing/ipc/default_socket.h"
#include "src/profiling/perf/perf_producer.h"

namespace perfetto {

// TODO(rsavitski): watchdog.
int TracedPerfMain(int, char**) {
  base::UnixTaskRunner task_runner;
  profiling::PerfProducer producer(&task_runner);
  producer.ConnectWithRetries(GetProducerSocket());
  task_runner.Run();
  return 0;
}

}  // namespace perfetto
