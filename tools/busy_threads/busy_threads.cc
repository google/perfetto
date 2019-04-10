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

#include <getopt.h>
#include <stdint.h>
#include <unistd.h>

#include <thread>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"

// Spawns the requested number threads that busy-wait for a fixed duration, and
// then yield.

namespace perfetto {
namespace {

constexpr int64_t kBusyWaitPeriodNs = 1000 * 1000;

__attribute__((noreturn)) void BusyWait() {
  while (1) {
    base::TimeNanos start = base::GetWallTimeNs();
    while ((base::GetWallTimeNs() - start).count() < kBusyWaitPeriodNs) {
      for (int i = 0; i < 10000; i++) {
        asm volatile("" ::: "memory");
      }
    }
    std::this_thread::yield();
  }
}

int BusyThreadsMain(int argc, char** argv) {
  int num_threads = -1;

  static struct option long_options[] = {
      {"threads", required_argument, nullptr, 't'}, {nullptr, 0, nullptr, 0}};
  int option_index;
  int c;
  while ((c = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
    switch (c) {
      case 't':
        num_threads = atoi(optarg);
        break;
      default:
        break;
    }
  }
  if (num_threads == -1) {
    PERFETTO_ELOG("Usage: %s [--threads=N]", argv[0]);
    return 1;
  }

  PERFETTO_LOG("Spawning %d threads.", num_threads);
  for (int i = 0; i < num_threads; i++) {
    std::thread th(BusyWait);
    th.detach();
  }
  PERFETTO_LOG("Threads spawned, Ctrl-C to stop.");
  while (sleep(600))
    ;

  return 0;
}

}  // namespace
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::BusyThreadsMain(argc, argv);
}
