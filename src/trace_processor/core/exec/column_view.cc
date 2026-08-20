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

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Whether `count` rows resolved through `resolve` land on a contiguous run,
// and where that run starts. `resolve` is a template parameter so the walk
// carries no per-row branch on how the rows are reached.
template <typename Resolve>
bool IsContiguousRun(Resolve resolve, uint32_t count, uint32_t* first) {
  *first = resolve(0);
  uint32_t expected = *first + 1;
  for (uint32_t row = 1; row < count; ++row, ++expected) {
    if (resolve(row) != expected) {
      return false;
    }
  }
  return true;
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
    // Slicing a prefix or suffix of an index array is a subspan, so the view
    // keeps the storage it already pointed at.
    const uint32_t* begin = current.data() + selection.offset();
    selection_ =
        RowSelection::Indices(Span<const uint32_t>(begin, begin + count));
    return;
  }

  const uint32_t* rows = selection.data();
  uint32_t first;
  if (current.is_range()) {
    uint32_t base = current.offset();
    if (IsContiguousRun([rows, base](uint32_t r) { return base + rows[r]; },
                        count, &first)) {
      selection_ = RowSelection::Range(first);
      block_ = nullptr;
      return;
    }
    // Rows counted from zero are already exactly the caller's array, whose
    // lifetime covers the batch.
    if (base == 0) {
      selection_ =
          RowSelection::Indices(Span<const uint32_t>(rows, rows + count));
      block_ = nullptr;
      return;
    }
    uint32_t* out = pool.TakeBlock();
    for (uint32_t row = 0; row < count; ++row) {
      out[row] = base + rows[row];
    }
    selection_ = RowSelection::Indices(Span<const uint32_t>(out, out + count));
    block_ = out;
    return;
  }

  const uint32_t* indices = current.data();
  if (IsContiguousRun([rows, indices](uint32_t r) { return indices[rows[r]]; },
                      count, &first)) {
    selection_ = RowSelection::Range(first);
    block_ = nullptr;
    return;
  }
  // A view the batch already composed narrows onto itself: ordinals only ever
  // grow, so an ascending gather never reads a slot it has already written.
  uint32_t* out = block_;
  if (out) {
    for (uint32_t row = 0; row < count; ++row) {
      PERFETTO_DCHECK(indices + rows[row] >= out + row);
    }
  } else {
    out = pool.TakeBlock();
  }
  for (uint32_t row = 0; row < count; ++row) {
    out[row] = indices[rows[row]];
  }
  selection_ = RowSelection::Indices(Span<const uint32_t>(out, out + count));
  block_ = out;
}

}  // namespace perfetto::trace_processor::core::exec
