/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace perfetto::trace_processor {

// Cross-connection state shared by every PerfettoSqlEngine attached to the
// same TraceProcessorImpl. Owned by TraceProcessorImpl and passed by pointer
// into each connection at construction time.
//
// In the multi-connection design this object holds:
//   - the vtab-state map, populated on writer `OnCommit` and consulted by
//     reader connections during cold xConnect;
//   - the function pool, an additive-only registry diffed against by each
//     connection at the start of `Execute` (no DROP, ever);
//   - per-module include locks that serialise concurrent
//     `INCLUDE PERFETTO MODULE` invocations against the same module name.
//
// Phase 2 iter 3 fills in the per-module include lock map. The vtab-state
// map and function pool remain TODOs for later chunks.
class GlobalStagingArea {
 public:
  // RAII guard returned by `AcquireIncludeLock`. Holds a `std::unique_lock`
  // on the per-module mutex; the lock is released on destruction.
  //
  // Concurrency note: Phase 2 is single-threaded so contention is not yet
  // possible. The API exists now so Phase 3 (thread safety) can wire
  // multi-threaded RPC fan-out without re-plumbing include processing.
  class IncludeLockGuard {
   public:
    IncludeLockGuard() = default;
    explicit IncludeLockGuard(std::unique_lock<std::mutex> lock)
        : lock_(std::move(lock)) {}

    IncludeLockGuard(IncludeLockGuard&&) = default;
    IncludeLockGuard& operator=(IncludeLockGuard&&) = default;

    IncludeLockGuard(const IncludeLockGuard&) = delete;
    IncludeLockGuard& operator=(const IncludeLockGuard&) = delete;

   private:
    std::unique_lock<std::mutex> lock_;
  };

  GlobalStagingArea();
  ~GlobalStagingArea();

  GlobalStagingArea(const GlobalStagingArea&) = delete;
  GlobalStagingArea& operator=(const GlobalStagingArea&) = delete;

  GlobalStagingArea(GlobalStagingArea&&) = delete;
  GlobalStagingArea& operator=(GlobalStagingArea&&) = delete;

  // Acquire the per-module mutex for `module_name`. Two connections importing
  // the same module serialise here; different modules don't contend. The
  // mutex itself is created on first request (and lives for the lifetime of
  // this `GlobalStagingArea`); the returned `IncludeLockGuard` releases on
  // destruction.
  IncludeLockGuard AcquireIncludeLock(const std::string& module_name);

 private:
  std::mutex map_mutex_;
  std::unordered_map<std::string, std::unique_ptr<std::mutex>> module_locks_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_GLOBAL_STAGING_AREA_H_
