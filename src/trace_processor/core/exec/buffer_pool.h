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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_BUFFER_POOL_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_BUFFER_POOL_H_

#include <array>
#include <cstddef>
#include <memory>

namespace perfetto::trace_processor::core::exec {

// Execution-local writable buffers. Published shared owners make a buffer
// unavailable for reuse. The pool retains at most kCapacity allocations; extra
// outstanding buffers live solely with their consumers. No value copy occurs.
// A pool is not shared between threads. Retained buffers may outlive the pool.
template <typename T>
class BufferPool {
 public:
  BufferPool() = default;
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;
  BufferPool(BufferPool&&) noexcept = default;
  BufferPool& operator=(BufferPool&&) noexcept = default;
  // Returns an exclusively writable buffer, possibly with previous contents.
  // Initialize it before publishing and stop writing once an owner is shared.
  std::shared_ptr<T> Acquire() {
    for (auto& buffer : buffers_) {
      if (!buffer)
        buffer = std::make_shared<T>();
      if (buffer.use_count() == 1)
        return buffer;
    }
    return std::make_shared<T>();
  }

 private:
  static constexpr size_t kCapacity = 2;

  std::array<std::shared_ptr<T>, kCapacity> buffers_;
};

}  // namespace perfetto::trace_processor::core::exec
#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_BUFFER_POOL_H_
