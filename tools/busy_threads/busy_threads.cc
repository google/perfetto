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

// Spawns the requested number threads that alternate between busy-waiting and
// sleeping.

namespace perfetto {
namespace {

__attribute__((noreturn)) void BusyWait(long busy_us, long sleep_us) {
  while (1) {
    base::TimeNanos start = base::GetWallTimeNs();
    while ((base::GetWallTimeNs() - start).count() < busy_us * 1000) {
      for (int i = 0; i < 10000; i++) {
        asm volatile("" ::: "memory");
      }
    }
    if (sleep_us > 0)
      base::SleepMicroseconds(static_cast<unsigned>(sleep_us));
    else
      std::this_thread::yield();
  }
}

int BusyThreadsMain(int argc, char** argv) {
  long num_threads = -1;
  long period_us = -1;
  long duty_cycle = -1;

  static struct option long_options[] = {
      {"threads", required_argument, nullptr, 't'},
      {"period_us", required_argument, nullptr, 'p'},
      {"duty_cycle", required_argument, nullptr, 'd'},
      {nullptr, 0, nullptr, 0}};
  int option_index;
  int c;
  while ((c = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
    switch (c) {
      case 't':
        num_threads = atol(optarg);
        break;
      case 'p':
        period_us = atol(optarg);
        break;
      case 'd':
        duty_cycle = atol(optarg);
        break;
      default:
        break;
    }
  }
  if (num_threads < 1 || period_us < 0 || duty_cycle < 1 || duty_cycle > 100) {
    PERFETTO_ELOG("Usage: %s --threads=N --period_us=N --duty_cycle=[1-100]",
                  argv[0]);
    return 1;
  }

  long busy_us = period_us * duty_cycle / 100;
  long sleep_us = period_us - busy_us;

  PERFETTO_LOG(
      "Spawning %ld threads; wait duration: %ldus; sleep duration: %ldus.",
      num_threads, busy_us, sleep_us);
  for (int i = 0; i < num_threads; i++) {
    std::thread th(BusyWait, busy_us, sleep_us);
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
