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
#include <vector>

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

base::Status RowStore::Append(const RowBatch& in, Context& context) {
  if (!has_shape_) {
    shape_.CopyFrom(in);
    has_shape_ = true;
    all_columns_.resize(in.column_count());
    for (uint32_t c = 0; c < in.column_count(); ++c) {
      all_columns_[c] = c;
    }
  }
  if (!in.size()) {
    return base::OkStatus();
  }
  if (in.size() > std::numeric_limits<uint32_t>::max() - size_) {
    return base::ErrStatus("row store: too many rows");
  }
  // The planner guarantees every batch of a step has the same columns.
  PERFETTO_DCHECK(shape_.column_count() == in.column_count());
#if PERFETTO_DCHECK_IS_ON()
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    PERFETTO_DCHECK(SameLogicalType(ColumnShape::Of(shape_.column(c)),
                                    ColumnShape::Of(in.column(c))));
  }
#endif
  size_ += in.size();
  if (Shares(in)) {
    // Rows packed before come first.
    Flush();
    Hold(in);
    return base::OkStatus();
  }
  RowBatch part;
  for (uint32_t done = 0; done < in.size();) {
    uint32_t take = std::min(in.size() - done, kMaxBatchRows - packing_.size());
    part.CopyFrom(in);
    part.mutable_selection().KeepRange(done, take);
    packing_.Append(part, context);
    done += take;
    if (packing_.size() == kMaxBatchRows) {
      Flush();
    }
  }
  return base::OkStatus();
}

bool RowStore::Shares(const RowBatch& in) {
  if (uint64_t{in.size()} * 2 >= kMaxBatchRows) {
    return true;
  }
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    if (in.buffer(c)) {
      return false;
    }
  }
  return true;
}

void RowStore::Hold(const RowBatch& in) {
  if (!held_) {
    nullable_ = FlexVector<bool>::CreateFilled(in.column_count(), false);
    sources_.resize(in.column_count());
  }
  if (held_ == batches_.size()) {
    batches_.push_back(std::make_unique<RowBatch>());
  }
  RowBatch& held = *batches_[held_];
  held.CopyFrom(in);
  for (uint32_t c = 0; c < in.column_count(); ++c) {
    const ColumnView& column = held.column(c);
    sources_[c].push_back(GatherSource::Of(column));
    if (column.validity()) {
      nullable_[c] = true;
    }
  }
  held_rows_ += in.size();
  batch_of_row_.push_back_multiple(held_++, in.size());
  ends_.push_back(held_rows_);
}

void RowStore::Flush() {
  if (!packing_.size()) {
    return;
  }
  RowBatch packed;
  packing_.Take(packed);
  Hold(packed);
}

void RowStore::Clear() {
  for (uint32_t i = 0; i < held_; ++i) {
    batches_[i]->Reset();
  }
  held_ = 0;
  held_rows_ = 0;
  ends_.clear();
  batch_of_row_.clear();
  for (FlexVector<GatherSource>& sources : sources_) {
    sources.clear();
  }
  nullable_.clear();
  packing_.Clear();
  shape_.Reset();
  has_shape_ = false;
  size_ = 0;
}

uint32_t RowStore::View(RowBatch* out, uint32_t offset, uint32_t count) {
  out->Reset();
  if (!count) {
    return 0;
  }
  PERFETTO_DCHECK(offset < size_);
  Flush();
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
  GatherColumns(all_columns_, rows, context, out);
  out->SetRowCount(count);
  return count;
}

void RowStore::GatherColumns(const std::vector<uint32_t>& columns,
                             Span<const uint32_t> rows,
                             Context& context,
                             RowBatch* out) {
  auto count =
      static_cast<uint32_t>(std::min<size_t>(rows.size(), kMaxBatchRows));
  if (!count) {
    return;
  }
  Flush();
  // Rows held one after another in one batch are already laid out as wanted:
  // view them where they are.
  uint32_t first = rows[0];
  PERFETTO_DCHECK(first < size_);
  uint32_t batch = Find(first);
  const RowBatch& held = *batches_[batch];
  uint32_t start = batch ? ends_[batch - 1] : 0;
  bool run = held.selection().prefix() && first + count <= ends_[batch];
  for (uint32_t r = 1; run && r < count; ++r) {
    run = rows[r] == first + r;
  }
  if (run) {
    for (uint32_t c : columns) {
      ColumnView view = held.column(c);
      view.set_start(view.start() + (first - start));
      out->AddColumn(view, held.buffer(c));
    }
    return;
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
  for (uint32_t c : columns) {
    const ColumnView& like = shape_.column(c);
    ColumnBuffer buffer = context.TakeBuffer(ValueWidth(like));
    ColumnChunk& chunk = buffer.chunk();
    chunk.Gather(like, sources_[c].data(), batches.data(), batch_rows.data(),
                 count, nullable_[c]);
    out->AddColumn(chunk.View(like, nullable_[c]), std::move(buffer));
  }
}

}  // namespace perfetto::trace_processor::core::exec
