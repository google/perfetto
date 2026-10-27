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

#include "src/trace_processor/core/exec/row_store.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

RowStore::RowStore() = default;
RowStore::~RowStore() = default;

base::Status RowStore::Append(const RowBatch& in) {
  if (!in.size()) {
    return base::OkStatus();
  }
  if (in.size() > std::numeric_limits<uint32_t>::max() - size_) {
    return base::ErrStatus("row store: too many rows");
  }
  if (size_) {
    // The planner guarantees every batch of a step has the same columns.
    const RowBatch& first = *batches_[0];
    PERFETTO_DCHECK(first.column_count() == in.column_count());
#if PERFETTO_DCHECK_IS_ON()
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      PERFETTO_DCHECK(SameLogicalType(ColumnShape::Of(first.column(c)),
                                      ColumnShape::Of(in.column(c))));
    }
#endif
  } else {
    nullable_ = FlexVector<bool>::CreateFilled(in.column_count(), false);
    sources_.resize(in.column_count());
  }
  if (held_ == batches_.size()) {
    batches_.push_back(std::make_unique<RowBatch>());
  }
  const RowBatch& held = *batches_[held_];
  batches_[held_]->CopyFrom(in);
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    const ColumnView& column = held.column(c);
    sources_[c].push_back(GatherSource::Of(column));
    if (column.validity()) {
      nullable_[c] = true;
    }
  }
  size_ += in.size();
  batch_of_row_.push_back_multiple(held_++, in.size());
  ends_.push_back(size_);
  return base::OkStatus();
}

void RowStore::Clear() {
  for (uint32_t i = 0; i < held_; ++i) {
    batches_[i]->Reset();
  }
  held_ = 0;
  ends_.clear();
  batch_of_row_.clear();
  for (FlexVector<GatherSource>& sources : sources_) {
    sources.clear();
  }
  nullable_.clear();
  size_ = 0;
}

uint32_t RowStore::View(RowBatch* out, uint32_t offset, uint32_t count) const {
  out->Reset();
  if (!count) {
    return 0;
  }
  PERFETTO_DCHECK(offset < size_);
  uint32_t batch = Find(offset);
  uint32_t start = batch ? ends_[batch - 1] : 0;
  count = std::min(count, ends_[batch] - offset);
  out->CopyFrom(*batches_[batch]);
  out->mutable_selection().KeepRange(offset - start, count);
  return count;
}

uint32_t RowStore::Gather(RowBatch* out,
                          Span<const uint32_t> rows,
                          Context& context) {
  out->Reset();
  auto count =
      static_cast<uint32_t>(std::min<size_t>(rows.size(), kMaxBatchRows));
  if (!count) {
    return 0;
  }
  // Which batch each row is in, and which row of it: found once for every
  // column. Rows taken out of order rarely come in runs from one batch, so
  // each column is then copied a row at a time, not a run at a time.
  std::array<uint32_t, kMaxBatchRows> batches;
  std::array<uint32_t, kMaxBatchRows> batch_rows;
  for (uint32_t r = 0; r < count; ++r) {
    PERFETTO_DCHECK(rows[r] < size_);
    batches[r] = Find(rows[r]);
    batch_rows[r] = RowInBatch(rows[r], batches[r]);
  }
  const RowBatch& first = *batches_[0];
  for (uint32_t c = 0; c < first.column_count(); ++c) {
    ColumnBuffer buffer = context.TakeBuffer(ValueWidth(first.column(c)));
    ColumnChunk& chunk = buffer.chunk();
    chunk.Gather(first.column(c), sources_[c].data(), batches.data(),
                 batch_rows.data(), count, nullable_[c]);
    out->AddColumn(chunk.View(first.column(c), nullable_[c]),
                   std::move(buffer));
  }
  out->SetRowCount(count);
  return count;
}

}  // namespace perfetto::trace_processor::core::exec
