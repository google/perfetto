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
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// How the sources which read `columns` of a dataframe lay them out: each as
// its storage type, in order.
inline Layout DataframeLayout(
    const std::vector<std::shared_ptr<const dataframe::Column>>& columns) {
  Layout layout;
  for (const auto& column : columns) {
    layout.push_back(ColumnLayout::Of(column->storage.type()));
  }
  return layout;
}

// Points batches at windows of a dataframe's rows, for the sources which read
// a dataframe.
//
// A column storing one value per row is lent where it lies, so moving the
// window costs nothing. A column which does not is expanded a window at a
// time into a buffer from the run's Context. Windows go forward only: each
// starts at or after the end of the one before, until Rewind(). A window
// starting further on skips the rows between without expanding them.
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

  // Allows windows to start from the first row again.
  void Rewind();

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
    windows_.Rewind();
    range_ = range;
    next_ = range.b;
  }

  // Points `out` at every column where it lies, keeping no rows: for reading
  // rows where they are stored. Every column must store one value per row.
  void Lend(RowBatch& out) { windows_.Fill(0, 0, out); }

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

// Reads a list of a dataframe's rows a batch at a time, in the list's order,
// for the sources which find the rows they read as a list. Nothing is copied:
// each batch is a window over a run of the list's rows which increase and fit
// in one, and its selection keeps those rows. A batch ends where the list
// stops increasing, so a list in increasing order of rows close together is
// read in few batches; any other in more, smaller ones.
class RowListReader {
 public:
  void Init(
      const std::vector<std::shared_ptr<const dataframe::Column>>& columns,
      Context& context) {
    windows_.Init(columns, context);
  }

  // Reads `rows`, which must stay unchanged until read.
  void Start(Span<const uint32_t> rows) {
    windows_.Rewind();
    next_ = rows.b;
    end_ = rows.e;
    last_end_ = 0;
  }

  // Points `out` at the next batch of the rows. False once all are read.
  bool Read(RowBatch& out) {
    if (next_ == end_) {
      return false;
    }
    uint32_t first = *next_++;
    // A window can only move forward: a row before it starts the next run
    // of windows from the beginning.
    if (first < last_end_) {
      windows_.Rewind();
    }
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
    uint32_t window = offsets_[count - 1] + 1;
    windows_.Fill(first, window, out);
    out.mutable_selection().KeepRows(
        Span<const uint32_t>(offsets_.data(), offsets_.data() + count));
    last_end_ = first + window;
    return true;
  }

 private:
  DataframeWindows windows_;
  const uint32_t* next_ = nullptr;
  const uint32_t* end_ = nullptr;
  // Where the last window ended.
  uint32_t last_end_ = 0;
  // The rows of the current batch, from the first.
  std::array<uint32_t, kMaxBatchRows> offsets_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_DATAFRAME_WINDOWS_H_
