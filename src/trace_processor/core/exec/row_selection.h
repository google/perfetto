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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_SELECTION_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_SELECTION_H_

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// The maximum number of rows in a batch.
inline constexpr uint32_t kMaxBatchRows = 2048;

// Maps the logical rows of a batch to physical rows of the underlying
// storage, either as a contiguous range or as an array of indices. An indexed
// selection does not own that array.
class RowSelection {
 public:
  static RowSelection Range(uint32_t offset = 0) {
    return RowSelection(nullptr, offset);
  }
  static RowSelection Indices(Span<const uint32_t> rows) {
    return RowSelection(rows.data(), 0);
  }

  uint32_t GetIndex(uint32_t row) const {
    return rows_ ? rows_[row] : offset_ + row;
  }
  bool is_range() const { return rows_ == nullptr; }
  const uint32_t* data() const { return rows_; }
  uint32_t offset() const { return offset_; }

 private:
  RowSelection(const uint32_t* rows, uint32_t offset)
      : rows_(rows), offset_(offset) {}

  const uint32_t* rows_;
  uint32_t offset_;
};

// The storage a batch composes its row selections into.
//
// Blocks are handed out for the life of one batch and reused by the next. A
// batch needs one block per group of columns sharing a selection, so the pool
// stops growing after the first batch.
class SelectionPool {
 public:
  // Returns a block of kMaxBatchRows indices, valid for as long as the batch
  // is. Growing the pool reallocates the vector of blocks, not the blocks.
  uint32_t* TakeBlock() {
    if (next_ == blocks_.size()) {
      blocks_.push_back(FlexVector<uint32_t>::CreateWithSize(kMaxBatchRows));
    }
    return blocks_[next_++].data();
  }

  // Returns every block to the pool for the next batch to use.
  void Reset() { next_ = 0; }

 private:
  std::vector<FlexVector<uint32_t>> blocks_;
  uint32_t next_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_SELECTION_H_
