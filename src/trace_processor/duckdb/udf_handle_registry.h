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

#ifndef SRC_TRACE_PROCESSOR_DUCKDB_UDF_HANDLE_REGISTRY_H_
#define SRC_TRACE_PROCESSOR_DUCKDB_UDF_HANDLE_REGISTRY_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace perfetto::trace_processor::duckdb_integration {

// Maps an opaque int64 handle (returned by a DuckDB aggregate) to a buffer of
// collected rows, so a downstream scalar "combiner" can take the buffer and run
// a native algorithm over it. This is how the table-valued intrinsics are
// ported to DuckDB without the table-function constant-argument restriction:
// the per-input aggregate returns a handle, the combiner Take()s it.
//
// One registry instance exists per buffer type `T` (via Instance()). Guarded by
// a mutex because, although a single query is single-threaded at the router,
// DuckDB may run the per-input aggregates on worker threads.
template <typename T>
class HandleRegistry {
 public:
  static HandleRegistry& Instance() {
    static HandleRegistry* instance = new HandleRegistry();
    return *instance;
  }

  int64_t Insert(std::unique_ptr<T> buf) {
    std::lock_guard<std::mutex> lock(mu_);
    int64_t handle = next_++;
    buffers_.emplace(handle, std::move(buf));
    return handle;
  }

  // Removes and returns the buffer for `handle`, or null if unknown.
  std::unique_ptr<T> Take(int64_t handle) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
      return nullptr;
    }
    std::unique_ptr<T> buf = std::move(it->second);
    buffers_.erase(it);
    return buf;
  }

 private:
  std::mutex mu_;
  int64_t next_ = 1;
  std::unordered_map<int64_t, std::unique_ptr<T>> buffers_;
};

}  // namespace perfetto::trace_processor::duckdb_integration

#endif  // SRC_TRACE_PROCESSOR_DUCKDB_UDF_HANDLE_REGISTRY_H_
