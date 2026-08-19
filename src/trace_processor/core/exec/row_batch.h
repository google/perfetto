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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/transient_column.h"

namespace perfetto::trace_processor::core::exec {

class RowBatchPool;

// A rectangular batch of columns with one shared cardinality.
class RowBatch {
 public:
  RowBatch() = default;

  uint32_t size() const { return cardinality_; }
  void SetCardinality(uint32_t count) {
    PERFETTO_DCHECK(count <= kMaxBatchRows);
    cardinality_ = count;
  }

  uint32_t column_count() const {
    return static_cast<uint32_t>(columns_.size());
  }
  const TransientColumn& column(uint32_t column) const {
    return columns_[column];
  }
  TransientColumn& mutable_column(uint32_t column) { return columns_[column]; }
  void AddColumn(TransientColumn vector) {
    columns_.push_back(std::move(vector));
  }

  // Points every non-constant column at `rows`, which the caller continues to
  // own. Valid only while every non-constant column still shares the source's
  // contiguous view. `rows` must stay valid until this batch is next filled;
  // that is the lifetime a chunk already has, since a source overwrites it on
  // the following pull.
  bool AdoptPhysicalRows(Span<const uint32_t> rows);

  // Applies strictly increasing logical row ordinals to every column. Returns
  // false when no rows remain.
  bool Slice(RowSelection selection, uint32_t count);

 private:
  friend class RowBatchPool;

  void Reset() {
    cardinality_ = 0;
    columns_.clear();
  }

  uint32_t cardinality_ = 0;
  std::vector<TransientColumn> columns_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_BATCH_H_
