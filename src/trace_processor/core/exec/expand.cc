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

#include "src/trace_processor/core/exec/expand.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/core/common/filter_kernels.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Copies the value stored at `slot` of `packed` into `value`, or the slot
// itself for an Id column, which stores none.
template <uint32_t kWidth>
void CopyValue(const uint8_t* packed, uint32_t slot, uint8_t* value) {
  if (packed) {
    memcpy(value, packed + uint64_t{slot} * kWidth, kWidth);
  } else {
    memcpy(value, &slot, sizeof(slot));
  }
}

// Writes the values of `view`'s first `rows` rows which `selection` keeps into
// `chunk`, with a validity bit for each. Values are copied as bytes, so types
// of the same width share one loop.
template <uint32_t kWidth>
void ExpandColumn(const ColumnView& view,
                  const Selection& selection,
                  uint32_t rows,
                  ColumnChunk& chunk) {
  const auto* packed = static_cast<const uint8_t*>(view.sparse_data());
  const BitVector& bits = *view.validity();
  uint8_t* values = chunk.Values<uint8_t>();
  chunk.SizeValidity();
  chunk.validity.ClearAllBits();
  uint32_t kept = selection.size();
  if (selection.prefix()) {
    // The rows kept are consecutive, so each set bit is the next slot.
    uint32_t slot =
        view.popcount()[view.start() / 64] +
        static_cast<uint32_t>(bits.count_set_bits_until_in_word(view.start()));
    for (uint32_t row = 0; row < kept; ++row) {
      uint8_t* value = values + uint64_t{row} * kWidth;
      if (bits.is_set(view.start() + row)) {
        CopyValue<kWidth>(packed, slot++, value);
        chunk.validity.set(row);
      } else {
        memset(value, 0, kWidth);
      }
    }
    memset(values + uint64_t{kept} * kWidth, 0, uint64_t{rows - kept} * kWidth);
    return;
  }
  memset(values, 0, uint64_t{rows} * kWidth);
  for (uint32_t i = 0; i < kept; ++i) {
    uint32_t row = selection[i];
    uint32_t slot = filter::IndexToStorageIndex<SparseNull>(
        view.start() + row, &bits, view.popcount());
    if (slot != std::numeric_limits<uint32_t>::max()) {
      CopyValue<kWidth>(packed, slot, values + uint64_t{row} * kWidth);
      chunk.validity.set(row);
    }
  }
}

}  // namespace

Expand::Expand(std::vector<uint32_t> columns) : columns_(std::move(columns)) {}

Expand::~Expand() = default;
Expand::State::~State() = default;

void Expand::DescribeColumns(StepColumns* out) const {
  out->reads = columns_;
}

std::unique_ptr<OperatorState> Expand::MakeState(Context& context) const {
  return std::make_unique<State>(context);
}

bool Expand::Process(RowBatch& batch, OperatorState& state) const {
  Context& context = *state.Cast<State>().context;
  for (uint32_t column : columns_) {
    ColumnView view = batch.column(column);
    PERFETTO_DCHECK(view.kind() == ColumnView::Kind::kSparse);
    if (batch.size() == 0) {
      batch.SetNullColumn(column, view.type(), context);
      continue;
    }
    uint32_t width = ValueWidth(view.type());
    ColumnBuffer buffer = context.TakeBuffer(width);
    ColumnChunk& chunk = buffer.chunk();
    if (width == sizeof(uint32_t)) {
      ExpandColumn<sizeof(uint32_t)>(view, batch.selection(), batch.row_count(),
                                     chunk);
    } else {
      PERFETTO_DCHECK(width == sizeof(uint64_t));
      ExpandColumn<sizeof(uint64_t)>(view, batch.selection(), batch.row_count(),
                                     chunk);
    }
    batch.SetColumn(column,
                    ColumnView::Reference(view.type(), chunk.Values<uint8_t>(),
                                          &chunk.validity),
                    std::move(buffer));
  }
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
