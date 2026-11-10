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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_SELECTION_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_SELECTION_H_

#include <cstdint>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/span.h"

namespace perfetto::trace_processor::core::exec {

inline constexpr uint32_t kMaxBatchRows = 2048;

// Which rows of a batch are kept: the first of them, or some by index, in
// increasing order. Every column of a batch has the same rows, so one
// selection serves them all, and dropping rows narrows it in place without
// touching a column.
class Selection {
 public:
  Selection() = default;
  Selection(const Selection&) = delete;
  Selection& operator=(const Selection&) = delete;

  // How many rows the batch has: every row kept is below it.
  uint32_t rows() const { return rows_; }

  // How many rows are kept.
  uint32_t size() const { return size_; }

  // Whether the rows kept are the first size() of them, so need no indices.
  bool prefix() const { return prefix_; }

  // The batch row the `i`th row kept is.
  PERFETTO_ALWAYS_INLINE uint32_t operator[](uint32_t i) const {
    PERFETTO_DCHECK(i < size_);
    return prefix_ ? i : indices_[i];
  }

  // Keeps all of `rows` rows.
  void Reset(uint32_t rows) {
    PERFETTO_DCHECK(rows <= kMaxBatchRows);
    rows_ = rows;
    size_ = rows;
    prefix_ = true;
  }

  // Keeps the rows kept at `positions`, which are increasing.
  void Keep(base::Span<const uint32_t> positions);

  // Keeps the batch rows `rows`, which are increasing and all kept already.
  void KeepRows(base::Span<const uint32_t> rows);

  // Keeps `count` of the rows kept, from the `first`.
  void KeepRange(uint32_t first, uint32_t count);

  // Keeps the rows `other` does, copying only the indices in use.
  void CopyFrom(const Selection& other);

 private:
  uint32_t rows_ = 0;
  uint32_t size_ = 0;
  bool prefix_ = true;
  // Unless `prefix_`, the first `size_` are the rows kept; only those are
  // written.
  uint16_t indices_[kMaxBatchRows];
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_SELECTION_H_
