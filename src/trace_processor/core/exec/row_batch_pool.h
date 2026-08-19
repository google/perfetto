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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_POOL_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_POOL_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

class RowBatchPool;

// A row batch borrowed from a pool and returned when it goes out of scope. The
// pool must outlive every row batch borrowed from it.
class PooledRowBatch {
 public:
  PooledRowBatch() = default;
  PooledRowBatch(RowBatchPool* pool, std::unique_ptr<RowBatch> chunk)
      : pool_(pool), chunk_(std::move(chunk)) {}
  PooledRowBatch(PooledRowBatch&&) noexcept;
  PooledRowBatch& operator=(PooledRowBatch&&) noexcept;
  PooledRowBatch(const PooledRowBatch&) = delete;
  PooledRowBatch& operator=(const PooledRowBatch&) = delete;
  ~PooledRowBatch();

  explicit operator bool() const { return chunk_ != nullptr; }
  RowBatch& operator*() const { return *chunk_; }
  RowBatch* operator->() const { return chunk_.get(); }

  // Returns the batch to its pool early.
  void Release();

 private:
  RowBatchPool* pool_ = nullptr;
  std::unique_ptr<RowBatch> chunk_;
};

// Recycles row batches across a running pipeline.
class RowBatchPool {
 public:
  PooledRowBatch Acquire() {
    if (free_.empty()) {
      ++allocations_;
      return PooledRowBatch(this, std::make_unique<RowBatch>());
    }
    std::unique_ptr<RowBatch> chunk = std::move(free_.back());
    free_.pop_back();
    return PooledRowBatch(this, std::move(chunk));
  }

  void Return(std::unique_ptr<RowBatch> chunk) {
    PERFETTO_DCHECK(chunk);
    chunk->Reset();
    free_.push_back(std::move(chunk));
  }

  // Row batches the pool has had to allocate. In a steady state this stops
  // growing, which is what the pool exists to guarantee.
  uint32_t allocations() const { return allocations_; }
  uint32_t free_count() const { return static_cast<uint32_t>(free_.size()); }

 private:
  uint32_t allocations_ = 0;
  std::vector<std::unique_ptr<RowBatch>> free_;
};

inline PooledRowBatch::PooledRowBatch(PooledRowBatch&& other) noexcept
    : pool_(other.pool_), chunk_(std::move(other.chunk_)) {
  other.pool_ = nullptr;
}

inline PooledRowBatch& PooledRowBatch::operator=(
    PooledRowBatch&& other) noexcept {
  if (this != &other) {
    Release();
    pool_ = other.pool_;
    chunk_ = std::move(other.chunk_);
    other.pool_ = nullptr;
  }
  return *this;
}

inline PooledRowBatch::~PooledRowBatch() {
  Release();
}

inline void PooledRowBatch::Release() {
  if (chunk_ && pool_) {
    pool_->Return(std::move(chunk_));
  }
  chunk_.reset();
  pool_ = nullptr;
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_POOL_H_
