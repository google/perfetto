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

// Returned by ResolveToRun when the rows are not one run.
constexpr uint32_t kNoRun = std::numeric_limits<uint32_t>::max();

// The row `count` logical rows start at, when resolving them lands on a
// single run. Worth asking because a run needs no index array and leaves
// reads contiguous for every operator downstream.
//
// `resolve` is a template parameter so the walk carries no per-row branch on
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
    // Slicing a prefix or suffix of an index array is a subspan, so the view
    // keeps the storage it already pointed at.
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
      selection_ = RowSelection::Range(run);
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
  uint32_t run = ResolveToRun(
      [rows, indices](uint32_t r) { return indices[rows[r]]; }, count);
  if (run != kNoRun) {
    selection_ = RowSelection::Range(run);
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
