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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// The batches an operator holds on to, to hand their rows back later: in
// runs, as they were held, or in any order, gathered.
//
// Holding a batch copies only which of its rows it keeps: its columns stay
// onto the same buffers, shared. Rows are numbered from 0, in the order they
// were appended.
class RowStore {
 public:
  RowStore();
  ~RowStore();

  // Holds the rows `in` keeps. Every batch must have the same columns.
  base::Status Append(const RowBatch& in);

  // How many rows are held.
  uint32_t size() const { return size_; }

  // Points `out` at up to `count` rows from `offset` on, as they are held. A
  // run stops at the end of a batch appended, so it can be shorter: returns
  // how many rows it has.
  uint32_t View(RowBatch* out, uint32_t offset, uint32_t count) const;

  // Copies up to kMaxBatchRows of `rows`, in that order and repeats included,
  // into buffers taken from `context`. Returns how many.
  uint32_t View(RowBatch* out, Span<const uint32_t> rows, Context& context);

  // Lets go of every row held.
  void Clear();

 private:
  // A run of the rows a gather copies, all from one batch held.
  struct Run {
    uint32_t batch;
    uint32_t begin;
    uint32_t end;
  };

  // The batch held which row `row` is in.
  uint32_t Find(uint32_t row) const;

  // The first `held_` are the batches held, the rest kept for reuse.
  std::vector<std::unique_ptr<RowBatch>> batches_;
  uint32_t held_ = 0;
  // By batch held, the row after its last.
  std::vector<uint32_t> ends_;
  // By column, whether any batch held has nulls in it.
  std::vector<bool> nullable_;
  uint32_t size_ = 0;
  // Scratch for gathering.
  std::vector<Run> runs_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_STORE_H_
