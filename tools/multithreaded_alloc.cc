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

#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <iterator>
#include <thread>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/profiling/memory/heap_profile.h"

namespace {

std::atomic<bool> done;
std::atomic<uint64_t> allocs{0};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wglobal-constructors"
static uint32_t g_heap_id =
    AHeapProfile_registerHeap(AHeapInfo_create("test_heap"));
#pragma GCC diagnostic pop

void Thread(uint32_t thread_idx) {
  PERFETTO_CHECK(thread_idx < 1 << 24);
  size_t alloc_id = 0;
  size_t thread_allocs = 0;
  while (!done.load(std::memory_order_relaxed)) {
    AHeapProfile_reportAllocation(g_heap_id, alloc_id | thread_idx, 1);
    AHeapProfile_reportFree(g_heap_id, alloc_id | thread_idx);
    alloc_id += 1 << 24;
    thread_allocs++;
  }
  allocs.fetch_add(thread_allocs, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    PERFETTO_FATAL("%s NUMBER_THREADS RUNTIME_MS", argv[0]);
  }

  perfetto::base::Optional<uint64_t> opt_no_threads =
      perfetto::base::CStringToUInt64(argv[1]);
  if (!opt_no_threads) {
    PERFETTO_FATAL("Invalid number of threads: %s", argv[1]);
  }
  uint64_t no_threads = *opt_no_threads;

  perfetto::base::Optional<uint64_t> opt_runtime_ms =
      perfetto::base::CStringToUInt64(argv[2]);
  if (!opt_runtime_ms) {
    PERFETTO_FATAL("Invalid runtime: %s", argv[2]);
  }
  uint64_t runtime_ms = *opt_runtime_ms;

  perfetto::base::TimeMillis end =
      perfetto::base::GetWallTimeMs() + perfetto::base::TimeMillis(runtime_ms);
  std::vector<std::thread> threads;
  for (size_t i = 0; i < static_cast<size_t>(no_threads); ++i)
    threads.emplace_back(Thread, i);

  perfetto::base::TimeMillis current = perfetto::base::GetWallTimeMs();
  while (current < end) {
    usleep(useconds_t((end - current).count()) * 1000);
    current = perfetto::base::GetWallTimeMs();
  }

  done.store(true, std::memory_order_relaxed);

  for (std::thread& th : threads)
    th.join();

  printf("%" PRIu64 "\n", allocs.load(std::memory_order_relaxed));
  return 0;
}
