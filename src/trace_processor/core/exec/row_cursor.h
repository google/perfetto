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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_CURSOR_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_CURSOR_H_

#include <cstdint>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/operator.h"

namespace perfetto::trace_processor::core::exec {

// The end of a pipeline, for a consumer that drives the loop itself: it asks
// for a row, reads what it wants from that row, then asks for the next one.
//
// A chunk is pulled only once the previous one runs out, so a consumer that
// stops early stops paying. This is the half of the model the pipeline above
// cannot express: operators are handed a chunk and run to the end of it,
// while everything from here on happens when it is asked for.
class RowCursor {
 public:
  explicit RowCursor(Source& source);
  ~RowCursor();

  // Reads up to the first row of a pipeline the caller has just rewound.
  // Returns false if there is none.
  bool Open() {
    index_ = 0;
    size_ = 0;
    return Pull();
  }

  // Reads up to the next row, pulling a chunk if the current one is spent.
  bool Next() { return ++index_ != size_ || Pull(); }

  bool eof() const { return index_ == size_; }

  // The row being read. Every column of a chunk shares one row view, so which
  // column it is read from does not matter.
  PERFETTO_ALWAYS_INLINE uint32_t row() const {
    PERFETTO_DCHECK(index_ < size_);
    return rows_ ? rows_[index_] : base_ + index_;
  }

 private:
  // Reads the next chunk into the row view. Returns false at the end.
  bool Pull();

  Source& source_;
  // The chunk's rows, or null when they are the contiguous run from `base_`.
  const uint32_t* rows_ = nullptr;
  uint32_t base_ = 0;
  uint32_t index_ = 0;
  uint32_t size_ = 0;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_ROW_CURSOR_H_
