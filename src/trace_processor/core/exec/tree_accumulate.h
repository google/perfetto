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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_batch_pool.h"

namespace perfetto::trace_processor::core::exec {

// TREE ACCUMULATE UP over a stream of chunks.
//
// A breaker: a node's total is not known until the last of its descendants
// has gone past, so every chunk goes in before any comes out.
//
// What it holds while it waits is the input, copied. A view of the input
// would be cheaper and would be wrong: a source is free to refill its buffers
// on the next pull, so a view is only good until the pull after the one that
// produced it. The rows come back out with one column added.
//
// The parent column holds each row's parent as a node number, or a negative
// value for a root, and every parent precedes its children. That is what a
// tree looks like once it is rows; establishing it is the job of whatever
// comes before this.
//
// Node numbers need not be stream positions, and are not whenever the rows
// were put in this order rather than arriving in it. Without a node column
// they are taken to be stream positions, which is what a source that was
// already in order gives for free.
class TreeAccumulateUp : public Breaker {
 public:
  TreeAccumulateUp(Source& source,
                   RowBatchPool* pool,
                   uint32_t parent_column,
                   uint32_t value_column,
                   std::optional<uint32_t> node_column = std::nullopt);
  ~TreeAccumulateUp() override;

 public:
  base::Status Consume(RowBatch&) override;
  base::Status Finish() override;

 protected:
  RowBatch* Emit() override;
  void Rewind() override;

 private:
  RowBatchPool* pool_;
  uint32_t parent_column_;
  uint32_t value_column_;
  std::optional<uint32_t> node_column_;

  // The columns the fold reads, pulled out flat as the chunks go by.
  std::vector<int64_t> parents_;
  std::vector<int64_t> values_;
  std::vector<int64_t> nodes_;
  std::vector<uint32_t> sizes_;
  uint32_t node_count_ = 0;

  std::vector<PooledRowBatch> retained_;
  // One per retained chunk, in the same order: the totals that chunk's rows
  // get, laid out so the added column reads them flat.
  std::vector<std::vector<int64_t>> totals_;
  size_t next_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TREE_ACCUMULATE_H_
