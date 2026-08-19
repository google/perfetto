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

#include "src/trace_processor/core/exec/transient_column.h"

#include <cstdint>
#include <memory>

#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

struct Composed {
  RowSelection selection;
  std::shared_ptr<const FlexVector<uint32_t>> owned;
};

// Resolves `count` rows through `resolve`, which maps a row to an index into
// the underlying storage. Materialises an index array only if the result is
// not a contiguous run; `resolve` is a type rather than a value so the walk
// carries no per-row branch on how the rows are reached.
template <typename Resolve>
Composed ComposeRows(Resolve resolve, uint32_t count) {
  uint32_t first = resolve(0);
  uint32_t row = 1;
  for (uint32_t expected = first + 1; row < count; ++row, ++expected) {
    if (resolve(row) != expected) {
      break;
    }
  }
  if (row == count) {
    return {RowSelection::Range(first), nullptr};
  }
  auto composed = std::make_shared<FlexVector<uint32_t>>(
      FlexVector<uint32_t>::CreateWithSize(count));
  uint32_t* write = composed->data();
  for (uint32_t r = 0; r < count; ++r, ++write) {
    *write = resolve(r);
  }
  Span<const uint32_t> span = composed->span();
  return {RowSelection::Indices(span), std::move(composed)};
}

}  // namespace

void TransientColumn::Slice(RowSelection selection, uint32_t count) {
  if (count == 0) {
    return;
  }

  RowSelection current = selection_;
  if (current.is_range() && selection.is_range()) {
    owned_selection_.reset();
    selection_ = RowSelection::Range(current.offset() + selection.offset());
    return;
  }

  if (!current.is_range() && selection.is_range()) {
    // Dropping a prefix or a suffix of an index array is a subspan of it. The
    // storage, owned or borrowed, is unchanged and stays referenced.
    const uint32_t* begin = current.data() + selection.offset();
    selection_ =
        RowSelection::Indices(Span<const uint32_t>(begin, begin + count));
    return;
  }

  const uint32_t* rows = selection.data();
  Composed composed =
      current.is_range()
          ? ComposeRows([rows, base = current.offset()](
                            uint32_t row) { return base + rows[row]; },
                        count)
          : ComposeRows([rows, indices = current.data()](
                            uint32_t row) { return indices[rows[row]]; },
                        count);
  selection_ = composed.selection;
  owned_selection_ = std::move(composed.owned);
}

}  // namespace perfetto::trace_processor::core::exec
