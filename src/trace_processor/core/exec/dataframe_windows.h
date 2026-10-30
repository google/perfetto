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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_WINDOWS_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_WINDOWS_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/range.h"

namespace perfetto::trace_processor::core::exec {

// Points batches at windows of a dataframe's rows, for the sources which read
// a dataframe.
//
// A column storing one value per row is lent where it lies, so moving the
// window costs nothing. A column which does not is expanded a window at a
// time into a buffer from the run's Context. A window may start at any row.
class DataframeWindows {
 public:
  DataframeWindows();
  ~DataframeWindows();
  DataframeWindows(const DataframeWindows&) = delete;
  DataframeWindows& operator=(const DataframeWindows&) = delete;

  // Prepares windows onto `columns`, expanding into buffers from `context`.
  // Called once, before any window is filled.
  void Init(const std::vector<std::shared_ptr<const dataframe::Column>>&,
            Context& context);

  // Points `out` at rows [from, from + count), all kept.
  void Fill(uint32_t from, uint32_t count, RowBatch& out);

  // Points `out` at the rows `offsets` from `from`, which increase from 0.
  // Only those rows are expanded.
  void FillKept(uint32_t from,
                base::Span<const uint32_t> offsets,
                RowBatch& out);

  // Fills one window of a column which does not store one value per row.
  // Defined in the .cc: an implementation detail with no callers outside it.
  class Expander;

 private:
  struct Expanded {
    uint32_t column;
    std::unique_ptr<Expander> expander;
  };

  // By column, the view of the window last filled, and the buffer it is onto
  // for an expanded column.
  std::vector<ColumnView> lent_;
  std::vector<ColumnBuffer> buffers_;
  std::vector<Expanded> expanded_;
  Context* context_ = nullptr;
};

// Reads a range of a dataframe's rows a batch at a time, for the sources
// which find the rows they read as a range.
class RangeReader {
 public:
  void Init(
      const std::vector<std::shared_ptr<const dataframe::Column>>& columns,
      Context& context) {
    windows_.Init(columns, context);
  }

  // Reads `range` from its first row.
  void Start(Range range) {
    range_ = range;
    next_ = range.b;
  }

  // Points `out` at the next batch of the range. False once all are read.
  PERFETTO_ALWAYS_INLINE bool Read(RowBatch& out) {
    if (next_ == range_.e) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, range_.e - next_);
    windows_.Fill(next_, count, out);
    next_ += count;
    return true;
  }

 private:
  DataframeWindows windows_;
  Range range_{0, 0};
  uint32_t next_ = 0;
};

// Reads a list of a dataframe's rows, in list order, a batch at a time. Each
// batch is a window over a strictly increasing run of the rows, with a
// selection keeping them, so nothing is copied.
class RowListReader {
 public:
  void Init(
      const std::vector<std::shared_ptr<const dataframe::Column>>& columns,
      Context& context) {
    windows_.Init(columns, context);
  }

  // `rows` must outlive the read.
  void Start(base::Span<const uint32_t> rows) {
    next_ = rows.b;
    end_ = rows.e;
  }

  // False once all rows are read.
  bool Read(RowBatch& out) {
    if (next_ == end_) {
      return false;
    }
    uint32_t first = *next_++;
    offsets_[0] = 0;
    uint32_t count = 1;
    for (; next_ != end_; ++next_) {
      uint32_t offset = *next_ - first;
      if (*next_ < first || offset >= kMaxBatchRows ||
          offset <= offsets_[count - 1]) {
        break;
      }
      offsets_[count++] = offset;
    }
    windows_.FillKept(
        first,
        base::Span<const uint32_t>(offsets_.data(), offsets_.data() + count),
        out);
    return true;
  }

 private:
  DataframeWindows windows_;
  const uint32_t* next_ = nullptr;
  const uint32_t* end_ = nullptr;
  // The current batch's rows, as offsets from its first.
  std::array<uint32_t, kMaxBatchRows> offsets_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_WINDOWS_H_
