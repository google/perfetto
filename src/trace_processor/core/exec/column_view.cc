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

#include "src/trace_processor/core/exec/column_view.h"

#include <cstdint>
#include <limits>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Returned by ResolveToRun when the rows are not contiguous.
constexpr uint32_t kNoRun = std::numeric_limits<uint32_t>::max();

// The physical row `count` logical rows start at, if they resolve to a
// contiguous run. Worth checking because a run needs no index array and keeps
// reads contiguous for every operator downstream.
//
// `resolve` is a template parameter so the loop carries no per-row branch on
// how a row is reached.
template <typename Resolve>
uint32_t ResolveToRun(Resolve resolve, uint32_t count) {
  uint32_t first = resolve(0);
  uint32_t expected = first + 1;
  for (uint32_t row = 1; row < count; ++row, ++expected) {
    if (resolve(row) != expected) {
      return kNoRun;
    }
  }
  return first;
}

}  // namespace

void ColumnView::Slice(RowSelection selection,
                       uint32_t count,
                       SelectionPool& pool) {
  if (count == 0) {
    return;
  }
  RowSelection current = selection_;

  if (selection.is_range()) {
    if (current.is_range()) {
      selection_ = RowSelection::Range(current.offset() + selection.offset());
      return;
    }
    // Slicing a prefix or suffix of an index array is just a subspan, so the
    // column keeps the storage it already pointed at.
    const uint32_t* begin = current.data() + selection.offset();
    selection_ =
        RowSelection::Indices(Span<const uint32_t>(begin, begin + count));
    return;
  }

  const uint32_t* rows = selection.data();
  if (current.is_range()) {
    uint32_t base = current.offset();
    uint32_t run = ResolveToRun(
        [rows, base](uint32_t r) { return base + rows[r]; }, count);
    if (run != kNoRun) {
      SetRange(run);
      return;
    }
    auto block = pool.TakeBlock();
    uint32_t* out = block->data();
    for (uint32_t row = 0; row < count; ++row) {
      out[row] = base + rows[row];
    }
    SetOwnedRows(std::move(block), count);
    return;
  }

  const uint32_t* indices = current.data();
  uint32_t run = ResolveToRun(
      [rows, indices](uint32_t r) { return indices[rows[r]]; }, count);
  if (run != kNoRun) {
    SetRange(run);
    return;
  }
  auto block = pool.TakeBlock();
  uint32_t* out = block->data();
  for (uint32_t row = 0; row < count; ++row) {
    out[row] = indices[rows[row]];
  }
  SetOwnedRows(std::move(block), count);
}

}  // namespace perfetto::trace_processor::core::exec
